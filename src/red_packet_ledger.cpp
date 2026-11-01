#include <tgbot/red_packet_ledger.hpp>

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace tgbot {

namespace {

std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

class Statement {
public:
    explicit Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string{"sqlite prepare failed: "} +
                                     sqlite3_errmsg(db));
        }
    }
    ~Statement() {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bindInt64(int index, std::int64_t value) {
        sqlite3_bind_int64(stmt_, index, value);
    }
    void bindText(int index, std::string_view value) {
        sqlite3_bind_text(stmt_, index, value.data(),
                          static_cast<int>(value.size()), SQLITE_TRANSIENT);
    }
    bool step() { return sqlite3_step(stmt_) == SQLITE_ROW; }
    void run() {
        if (sqlite3_step(stmt_) != SQLITE_DONE) {
            throw std::runtime_error("sqlite step failed");
        }
    }
    std::int64_t columnInt64(int index) {
        return sqlite3_column_int64(stmt_, index);
    }
    std::string columnText(int index) {
        const auto* text = sqlite3_column_text(stmt_, index);
        return text == nullptr
                   ? std::string{}
                   : std::string{reinterpret_cast<const char*>(text)};
    }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

// 所有 SELECT 都用同一份列清单，避免索引错位（sqlite3_column_* 是 0-based）
constexpr const char* kColumns =
    "id, chat_id, message_id, creator_id, total_cents, total_count,"
    " remaining_cents, remaining_count, state, created_at, expires_at,"
    " tx_ref, refund_started_at, refund_attempts";

RedPacketRow readRow(Statement& stmt) {
    RedPacketRow row;
    row.id = stmt.columnInt64(0);
    row.chatId = stmt.columnInt64(1);
    row.messageId = stmt.columnInt64(2);
    row.creatorId = stmt.columnInt64(3);
    row.totalCents = stmt.columnInt64(4);
    row.totalCount = stmt.columnInt64(5);
    row.remainingCents = stmt.columnInt64(6);
    row.remainingCount = stmt.columnInt64(7);
    row.state = stmt.columnText(8);
    row.createdAt = stmt.columnInt64(9);
    row.expiresAt = stmt.columnInt64(10);
    row.txRef = stmt.columnText(11);
    row.refundStartedAt = stmt.columnInt64(12);
    row.refundAttempts = static_cast<int>(stmt.columnInt64(13));
    return row;
}

}  // namespace

RedPacketLedger::RedPacketLedger(std::string path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        throw std::runtime_error("cannot open ledger database: " + path);
    }
    exec("PRAGMA journal_mode=WAL");
    exec(
        "CREATE TABLE IF NOT EXISTS red_packets("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " chat_id INTEGER NOT NULL,"
        " message_id INTEGER NOT NULL DEFAULT 0,"
        " creator_id INTEGER NOT NULL,"
        " total_cents INTEGER NOT NULL,"
        " total_count INTEGER NOT NULL,"
        " remaining_cents INTEGER NOT NULL,"
        " remaining_count INTEGER NOT NULL,"
        " state TEXT NOT NULL DEFAULT 'open',"
        " created_at INTEGER NOT NULL,"
        " expires_at INTEGER NOT NULL,"
        " tx_ref TEXT NOT NULL DEFAULT '',"
        " refund_started_at INTEGER NOT NULL DEFAULT 0,"
        " refund_attempts INTEGER NOT NULL DEFAULT 0)");
    exec(
        "CREATE TABLE IF NOT EXISTS red_packet_grabs("
        " packet_id INTEGER NOT NULL,"
        " user_id INTEGER NOT NULL,"
        " cents INTEGER NOT NULL,"
        " user_name TEXT NOT NULL DEFAULT '',"
        " created_at INTEGER NOT NULL,"
        " PRIMARY KEY(packet_id, user_id))");
    migrate();
}

RedPacketLedger::~RedPacketLedger() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

void RedPacketLedger::migrate() {
    // 老库升级：加上对账需要的两列
    if (!hasColumn("red_packets", "refund_started_at")) {
        exec("ALTER TABLE red_packets ADD COLUMN refund_started_at "
             "INTEGER NOT NULL DEFAULT 0");
        spdlog::info("ledger: 已迁移，新增 refund_started_at");
    }
    if (!hasColumn("red_packets", "refund_attempts")) {
        exec("ALTER TABLE red_packets ADD COLUMN refund_attempts "
             "INTEGER NOT NULL DEFAULT 0");
        spdlog::info("ledger: 已迁移，新增 refund_attempts");
    }
}

bool RedPacketLedger::hasColumn(const char* table, const char* column) const {
    Statement stmt(db_, "PRAGMA table_info(red_packets)");
    (void)table;
    while (stmt.step()) {
        // PRAGMA table_info: cid, name, type, notnull, dflt_value, pk
        if (stmt.columnText(1) == column) {
            return true;
        }
    }
    return false;
}

void RedPacketLedger::exec(const char* sql) const {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string message = err != nullptr ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("sqlite exec failed: " + message);
    }
}

std::int64_t RedPacketLedger::create(std::int64_t chatId,
                                     std::int64_t creatorId,
                                     std::int64_t totalCents, int count,
                                     std::chrono::seconds ttl) {
    const auto now = nowSeconds();
    const auto expires = now + ttl.count();
    Statement stmt(
        db_,
        "INSERT INTO red_packets(chat_id, creator_id, total_cents, total_count,"
        " remaining_cents, remaining_count, state, created_at, expires_at)"
        " VALUES(?, ?, ?, ?, ?, ?, 'open', ?, ?)");
    stmt.bindInt64(1, chatId);
    stmt.bindInt64(2, creatorId);
    stmt.bindInt64(3, totalCents);
    stmt.bindInt64(4, count);
    stmt.bindInt64(5, totalCents);
    stmt.bindInt64(6, count);
    stmt.bindInt64(7, now);
    stmt.bindInt64(8, expires);
    stmt.run();
    return sqlite3_last_insert_rowid(db_);
}

std::optional<RedPacketRow> RedPacketLedger::get(std::int64_t id) const {
    const std::string sql = std::string{"SELECT "} + kColumns +
                            " FROM red_packets WHERE id=?";
    Statement stmt(db_, sql.c_str());
    stmt.bindInt64(1, id);
    if (!stmt.step()) {
        return std::nullopt;
    }
    return readRow(stmt);
}

std::vector<GrabRow> RedPacketLedger::grabs(std::int64_t id) const {
    std::vector<GrabRow> rows;
    Statement stmt(db_,
                   "SELECT user_id, cents, user_name, created_at"
                   " FROM red_packet_grabs WHERE packet_id=? ORDER BY created_at"
                   " , user_id");
    stmt.bindInt64(1, id);
    while (stmt.step()) {
        GrabRow row;
        row.userId = stmt.columnInt64(0);
        row.cents = stmt.columnInt64(1);
        row.userName = stmt.columnText(2);
        row.createdAt = stmt.columnInt64(3);
        rows.push_back(std::move(row));
    }
    return rows;
}

bool RedPacketLedger::hasGrabbed(std::int64_t id, std::int64_t userId) const {
    Statement stmt(
        db_,
        "SELECT 1 FROM red_packet_grabs WHERE packet_id=? AND user_id=?");
    stmt.bindInt64(1, id);
    stmt.bindInt64(2, userId);
    return stmt.step();
}

bool RedPacketLedger::grab(std::int64_t id, std::int64_t userId,
                           std::string_view userName, std::int64_t cents) {
    if (cents <= 0) {
        return false;
    }
    exec("BEGIN IMMEDIATE");

    {
        Statement update(
            db_,
            "UPDATE red_packets SET"
            " remaining_cents = remaining_cents - ?,"
            " remaining_count = remaining_count - 1,"
            " state = CASE WHEN remaining_count - 1 <= 0 THEN 'exhausted'"
            "              ELSE state END"
            " WHERE id=? AND state='open' AND remaining_count>0"
            "   AND remaining_cents>=?");
        update.bindInt64(1, cents);
        update.bindInt64(2, id);
        update.bindInt64(3, cents);
        update.run();
        if (sqlite3_changes(db_) != 1) {
            exec("ROLLBACK");
            return false;
        }
    }

    {
        Statement insert(
            db_,
            "INSERT OR IGNORE INTO red_packet_grabs"
            "(packet_id, user_id, cents, user_name, created_at)"
            " VALUES(?, ?, ?, ?, ?)");
        insert.bindInt64(1, id);
        insert.bindInt64(2, userId);
        insert.bindInt64(3, cents);
        insert.bindText(4, userName);
        insert.bindInt64(5, nowSeconds());
        insert.run();
        if (sqlite3_changes(db_) != 1) {
            exec("ROLLBACK");  // 同一人重复领取
            return false;
        }
    }

    exec("COMMIT");
    return true;
}

void RedPacketLedger::attachMessage(std::int64_t id, std::int64_t messageId) {
    Statement stmt(db_, "UPDATE red_packets SET message_id=? WHERE id=?");
    stmt.bindInt64(1, messageId);
    stmt.bindInt64(2, id);
    stmt.run();
}

bool RedPacketLedger::markExpired(std::int64_t id) {
    Statement stmt(db_,
                   "UPDATE red_packets SET state='expired'"
                   " WHERE id=? AND state='open'");
    stmt.bindInt64(1, id);
    stmt.run();
    return sqlite3_changes(db_) == 1;
}

std::vector<RedPacketRow> RedPacketLedger::listRefundable(
    std::int64_t nowSecondsValue, std::size_t limit) const {
    const std::string sql =
        std::string{"SELECT "} + kColumns +
        " FROM red_packets WHERE state='expired'"
        " OR (state='open' AND expires_at<=?) ORDER BY id LIMIT ?";
    std::vector<RedPacketRow> rows;
    Statement stmt(db_, sql.c_str());
    stmt.bindInt64(1, nowSecondsValue);
    stmt.bindInt64(2, static_cast<std::int64_t>(limit));
    while (stmt.step()) {
        rows.push_back(readRow(stmt));
    }
    return rows;
}

std::vector<RedPacketRow> RedPacketLedger::listRefunding(
    std::int64_t nowSecondsValue, std::int64_t minAgeSeconds,
    std::size_t limit) const {
    const std::string sql =
        std::string{"SELECT "} + kColumns +
        " FROM red_packets WHERE state IN ('refunding','stuck')"
        "   AND refund_started_at<=? ORDER BY refund_started_at LIMIT ?";
    std::vector<RedPacketRow> rows;
    Statement stmt(db_, sql.c_str());
    stmt.bindInt64(1, nowSecondsValue - minAgeSeconds);
    stmt.bindInt64(2, static_cast<std::int64_t>(limit));
    while (stmt.step()) {
        rows.push_back(readRow(stmt));
    }
    return rows;
}

bool RedPacketLedger::beginRefund(std::int64_t id) {
    Statement stmt(db_,
                   "UPDATE red_packets SET state='refunding',"
                   " refund_started_at=?, refund_attempts=refund_attempts+1"
                   " WHERE id=? AND state IN ('open','expired')");
    stmt.bindInt64(1, nowSeconds());
    stmt.bindInt64(2, id);
    stmt.run();
    return sqlite3_changes(db_) == 1;
}

bool RedPacketLedger::setTxRef(std::int64_t id, std::string_view txRef) {
    Statement stmt(db_,
                   "UPDATE red_packets SET tx_ref=? WHERE id=?"
                   " AND state IN ('refunding','stuck')");
    stmt.bindText(1, txRef);
    stmt.bindInt64(2, id);
    stmt.run();
    return sqlite3_changes(db_) == 1;
}

void RedPacketLedger::finishRefund(std::int64_t id, std::string_view txRef) {
    Statement stmt(db_,
                   "UPDATE red_packets SET state='refunded', tx_ref=?"
                   " WHERE id=? AND state IN ('refunding','stuck')");
    stmt.bindText(1, txRef);
    stmt.bindInt64(2, id);
    stmt.run();
}

bool RedPacketLedger::revertRefund(std::int64_t id) {
    Statement stmt(db_,
                   "UPDATE red_packets SET state='expired'"
                   " WHERE id=? AND state='refunding'");
    stmt.bindInt64(1, id);
    stmt.run();
    return sqlite3_changes(db_) == 1;
}

bool RedPacketLedger::markStuck(std::int64_t id) {
    Statement stmt(db_,
                   "UPDATE red_packets SET state='stuck'"
                   " WHERE id=? AND state='refunding'");
    stmt.bindInt64(1, id);
    stmt.run();
    return sqlite3_changes(db_) == 1;
}

bool RedPacketLedger::recoverFromStuck(std::int64_t id) {
    Statement stmt(db_,
                   "UPDATE red_packets SET state='expired'"
                   " WHERE id=? AND state='stuck'");
    stmt.bindInt64(1, id);
    stmt.run();
    return sqlite3_changes(db_) == 1;
}

std::size_t RedPacketLedger::countStuck() const {
    Statement stmt(db_,
                   "SELECT COUNT(*) FROM red_packets WHERE state='stuck'");
    return stmt.step() ? static_cast<std::size_t>(stmt.columnInt64(0)) : 0;
}

}  // namespace tgbot
