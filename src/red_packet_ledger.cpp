#include <tgbot/red_packet_ledger.hpp>

#include <sqlite3.h>

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
        " tx_ref TEXT NOT NULL DEFAULT '')");
    exec(
        "CREATE TABLE IF NOT EXISTS red_packet_grabs("
        " packet_id INTEGER NOT NULL,"
        " user_id INTEGER NOT NULL,"
        " cents INTEGER NOT NULL,"
        " user_name TEXT NOT NULL DEFAULT '',"
        " created_at INTEGER NOT NULL,"
        " PRIMARY KEY(packet_id, user_id))");
}

RedPacketLedger::~RedPacketLedger() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
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
    Statement stmt(db_,
                   "SELECT id, chat_id, message_id, creator_id, total_cents,"
                   " total_count, remaining_cents, remaining_count, state,"
                   " created_at, expires_at, tx_ref"
                   " FROM red_packets WHERE id=?");
    stmt.bindInt64(1, id);
    if (!stmt.step()) {
        return std::nullopt;
    }
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
    return row;
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
        // 条件更新：余额不够、已结束、已抢完都会影响 0 行
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
            // 同一人重复领取：把刚才的扣减回滚掉
            exec("ROLLBACK");
            return false;
        }
    }

    exec("COMMIT");
    return true;
}

void RedPacketLedger::attachMessage(std::int64_t id, std::int64_t messageId) {
    Statement stmt(db_,
                   "UPDATE red_packets SET message_id=? WHERE id=?");
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

bool RedPacketLedger::beginRefund(std::int64_t id) {
    Statement stmt(db_,
                   "UPDATE red_packets SET state='refunding'"
                   " WHERE id=? AND state IN ('open','expired')");
    stmt.bindInt64(1, id);
    stmt.run();
    return sqlite3_changes(db_) == 1;
}

std::vector<RedPacketRow> RedPacketLedger::listRefundable(
    std::int64_t nowSeconds, std::size_t limit) const {
    std::vector<RedPacketRow> rows;
    Statement stmt(
        db_,
        "SELECT id, chat_id, message_id, creator_id, total_cents,"
        " total_count, remaining_cents, remaining_count, state,"
        " created_at, expires_at, tx_ref"
        " FROM red_packets"
        " WHERE state='expired' OR (state='open' AND expires_at<=?)"
        " ORDER BY id LIMIT ?");
    stmt.bindInt64(1, nowSeconds);
    stmt.bindInt64(2, static_cast<std::int64_t>(limit));
    while (stmt.step()) {
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
        rows.push_back(std::move(row));
    }
    return rows;
}

bool RedPacketLedger::revertRefund(std::int64_t id) {
    Statement stmt(db_,
                   "UPDATE red_packets SET state='expired'"
                   " WHERE id=? AND state='refunding'");
    stmt.bindInt64(1, id);
    stmt.run();
    return sqlite3_changes(db_) == 1;
}

void RedPacketLedger::finishRefund(std::int64_t id, std::string_view txRef) {
    Statement stmt(db_,
                   "UPDATE red_packets SET state='refunded', tx_ref=?"
                   " WHERE id=? AND state='refunding'");
    stmt.bindText(1, txRef);
    stmt.bindInt64(2, id);
    stmt.run();
}

}  // namespace tgbot
