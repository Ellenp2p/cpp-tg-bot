#include <tgbot/sqlite_state_store.hpp>

#include <sqlite3.h>

#include <stdexcept>
#include <utility>

namespace tgbot {

namespace {

// 小工具：prepare + bind 一个语句，出错抛异常（含 SQL 文本）
sqlite3_stmt* prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string{"sqlite prepare failed: "} +
                                 sqlite3_errmsg(db) + " [" + sql + "]");
    }
    return stmt;
}

void bindText(sqlite3_stmt* stmt, int index, std::string_view value) {
    // SQLITE_TRANSIENT：让 SQLite 拷贝数据，调用方无需保活到 step 之后
    sqlite3_bind_text(stmt, index, value.data(),
                      static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

}  // namespace

SqliteStateStore::SqliteStateStore(const std::string& dbPath) {
    if (sqlite3_open(dbPath.c_str(), &db_) != SQLITE_OK) {
        const std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw std::runtime_error("sqlite open failed: " + msg);
    }
    // WAL：读写不互斥，崩溃后自动恢复；单写者场景下最简单可靠
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_,
                 "CREATE TABLE IF NOT EXISTS tgbot_kv ("
                 "  k TEXT PRIMARY KEY,"
                 "  v TEXT NOT NULL"
                 ");",
                 nullptr, nullptr, nullptr);
}

SqliteStateStore::~SqliteStateStore() {
    if (db_) {
        sqlite3_close(db_);
    }
}

std::optional<std::string> SqliteStateStore::get(std::string_view key) {
    sqlite3_stmt* stmt = prepare(db_, "SELECT v FROM tgbot_kv WHERE k = ?");
    bindText(stmt, 1, key);
    std::optional<std::string> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* text =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        result = text ? std::string{text}
                      : std::string{};  // 空串值与 NULL 区分
    }
    sqlite3_finalize(stmt);
    return result;
}

void SqliteStateStore::set(std::string_view key, std::string_view value) {
    sqlite3_stmt* stmt =
        prepare(db_, "INSERT OR REPLACE INTO tgbot_kv (k, v) VALUES (?, ?)");
    bindText(stmt, 1, key);
    bindText(stmt, 2, value);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error(std::string{"sqlite set failed: "} +
                                 sqlite3_errmsg(db_));
    }
    sqlite3_finalize(stmt);
}

void SqliteStateStore::erase(std::string_view key) {
    sqlite3_stmt* stmt = prepare(db_, "DELETE FROM tgbot_kv WHERE k = ?");
    bindText(stmt, 1, key);
    sqlite3_step(stmt);  // 删不存在的关键字不是错误
    sqlite3_finalize(stmt);
}

}  // namespace tgbot
