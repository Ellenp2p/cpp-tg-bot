#pragma once

#include <tgbot/state_store.hpp>

#include <string>

struct sqlite3;  // 前向声明，头文件不暴露 sqlite3.h

namespace tgbot {

// SQLite 后端：单文件数据库，嵌入式、零配置，与全静态交付兼容
// （纯静态二进制照样跑，只是旁边多一个 .db 文件）。
//
//   SqliteStateStore store("bot_state.db");
//
// 特性：
//   - 建库自动 CREATE TABLE IF NOT EXISTS，老库直接用
//   - 单写者（WAL 模式），与框架单实例设计一致
//   - 同步 API：SQLite 写入是微秒级，不比 JSON 文件慢
class SqliteStateStore : public StateStore {
public:
    explicit SqliteStateStore(const std::string& dbPath);
    ~SqliteStateStore() override;

    SqliteStateStore(const SqliteStateStore&) = delete;
    SqliteStateStore& operator=(const SqliteStateStore&) = delete;

    std::optional<std::string> get(std::string_view key) override;
    void set(std::string_view key, std::string_view value) override;
    void erase(std::string_view key) override;

private:
    sqlite3* db_ = nullptr;
};

}  // namespace tgbot
