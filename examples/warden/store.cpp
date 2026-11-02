#include "store.hpp"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <algorithm>
#include <ctime>
#include <stdexcept>
#include <string>

namespace warden {

namespace {

class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string{"sqlite prepare failed: "} + sqlite3_errmsg(db));
        }
    }
    ~Stmt() {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    void bind(int index, std::int64_t value) { sqlite3_bind_int64(stmt_, index, value); }
    void bind(int index, std::string_view value) {
        sqlite3_bind_text(
            stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    }
    void bind(int index, bool value) { sqlite3_bind_int(stmt_, index, value ? 1 : 0); }
    bool row() { return sqlite3_step(stmt_) == SQLITE_ROW; }
    void run() {
        if (sqlite3_step(stmt_) != SQLITE_DONE) {
            throw std::runtime_error(std::string{"sqlite step failed: "} + sqlite3_errmsg(db_));
        }
    }
    std::int64_t i64(int index) { return sqlite3_column_int64(stmt_, index); }
    bool boolean(int index) { return sqlite3_column_int(stmt_, index) != 0; }
    std::string text(int index) {
        const auto* p = sqlite3_column_text(stmt_, index);
        return p == nullptr ? std::string{} : std::string{reinterpret_cast<const char*>(p)};
    }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

constexpr const char* kChatColumns =
    "chat_id,type,title,username,bot_status,can_delete,can_restrict,can_pin,"
    "can_invite,can_manage,enabled,joined_at,left_at,last_seen_at,"
    "refreshed_at";

ChatRecord readChat(Stmt& stmt) {
    ChatRecord r;
    r.chatId = stmt.i64(0);
    r.type = stmt.text(1);
    r.title = stmt.text(2);
    r.username = stmt.text(3);
    r.botStatus = stmt.text(4);
    r.canDelete = stmt.boolean(5);
    r.canRestrict = stmt.boolean(6);
    r.canPin = stmt.boolean(7);
    r.canInvite = stmt.boolean(8);
    r.canManage = stmt.boolean(9);
    r.enabled = stmt.boolean(10);
    r.joinedAt = stmt.i64(11);
    r.leftAt = stmt.i64(12);
    r.lastSeenAt = stmt.i64(13);
    r.refreshedAt = stmt.i64(14);
    return r;
}

} // namespace

Store::Store(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        throw std::runtime_error("cannot open warden database: " + path);
    }
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec("PRAGMA busy_timeout=5000");

    exec(
        "CREATE TABLE IF NOT EXISTS chats("
        " chat_id INTEGER PRIMARY KEY,"
        " type TEXT NOT NULL DEFAULT '',"
        " title TEXT NOT NULL DEFAULT '',"
        " username TEXT NOT NULL DEFAULT '',"
        " bot_status TEXT NOT NULL DEFAULT 'unknown',"
        " can_delete INTEGER NOT NULL DEFAULT 0,"
        " can_restrict INTEGER NOT NULL DEFAULT 0,"
        " can_pin INTEGER NOT NULL DEFAULT 0,"
        " can_invite INTEGER NOT NULL DEFAULT 0,"
        " can_manage INTEGER NOT NULL DEFAULT 0,"
        " enabled INTEGER NOT NULL DEFAULT 0,"
        " joined_at INTEGER NOT NULL DEFAULT 0,"
        " left_at INTEGER NOT NULL DEFAULT 0,"
        " last_seen_at INTEGER NOT NULL DEFAULT 0,"
        " refreshed_at INTEGER NOT NULL DEFAULT 0)");

    exec(
        "CREATE TABLE IF NOT EXISTS chat_settings("
        " chat_id INTEGER NOT NULL,"
        " kind TEXT NOT NULL,"
        " delay_sec INTEGER NOT NULL,"
        " PRIMARY KEY(chat_id, kind))");

    exec(
        "CREATE TABLE IF NOT EXISTS deletions("
        " chat_id INTEGER NOT NULL,"
        " message_id INTEGER NOT NULL,"
        " target_user INTEGER NOT NULL DEFAULT 0,"
        " kind TEXT NOT NULL DEFAULT '',"
        " due_at INTEGER NOT NULL,"
        " attempts INTEGER NOT NULL DEFAULT 0,"
        " state TEXT NOT NULL DEFAULT 'pending',"
        " last_error TEXT NOT NULL DEFAULT '',"
        " created_at INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY(chat_id, message_id))");
    exec(
        "CREATE INDEX IF NOT EXISTS idx_deletions_due"
        " ON deletions(state, due_at)");
    // 老库迁移：target_user 是后加的，已有库 ALTER 一次；重复列错误忽略
    try {
        exec(
            "ALTER TABLE deletions ADD COLUMN target_user INTEGER NOT NULL "
            "DEFAULT 0");
    } catch (const std::exception&) {
        // duplicate column name: 新库/已迁移库
    }
    // 旧版本 finishDeletion 把 state 误写成 1/0，导致这些行永远不被
    // pending 查询或 janitor 清理；启动时归一化一次。
    exec("UPDATE deletions SET state='done' WHERE state='1'");
    exec("UPDATE deletions SET state='failed' WHERE state='0'");

    exec(
        "CREATE TABLE IF NOT EXISTS pinned_messages("
        " chat_id INTEGER NOT NULL,"
        " message_id INTEGER NOT NULL,"
        " pinned_at INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY(chat_id, message_id))");

    exec(
        "CREATE TABLE IF NOT EXISTS chat_admins("
        " chat_id INTEGER NOT NULL,"
        " user_id INTEGER NOT NULL,"
        " status TEXT NOT NULL DEFAULT '',"
        " display_name TEXT NOT NULL DEFAULT '',"
        " updated_at INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY(chat_id, user_id))");
    exec(
        "CREATE INDEX IF NOT EXISTS idx_chat_admins_user"
        " ON chat_admins(user_id)");

    exec(
        "CREATE TABLE IF NOT EXISTS audit_log("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " ts INTEGER NOT NULL,"
        " chat_id INTEGER NOT NULL DEFAULT 0,"
        " actor_id INTEGER NOT NULL DEFAULT 0,"
        " action TEXT NOT NULL,"
        " detail TEXT NOT NULL DEFAULT '')");
    exec(
        "CREATE INDEX IF NOT EXISTS idx_audit_chat"
        " ON audit_log(chat_id, ts)");
}

Store::~Store() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

void Store::exec(const char* sql) const {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string message = err != nullptr ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("sqlite exec failed: " + message);
    }
}

void Store::ensureChat(std::int64_t chatId, std::string type, std::string title, std::int64_t now) {
    // 重复见到同一个群时只刷新 last_seen，并把"已退出"标记清掉：
    // 命令/消息能到达就说明机器人现在确实在群里
    Stmt stmt(db_,
              "INSERT INTO chats"
              "(chat_id,type,title,bot_status,joined_at,last_seen_at)"
              " VALUES(?,?,?,'member',?,?)"
              " ON CONFLICT(chat_id) DO UPDATE SET"
              " left_at=0, last_seen_at=excluded.last_seen_at");
    stmt.bind(1, chatId);
    stmt.bind(2, type);
    stmt.bind(3, title);
    stmt.bind(4, now);
    stmt.bind(5, now);
    stmt.run();
    lastSeenWrite_[chatId] = now;
}

void Store::saveChat(const ChatRecord& r) {
    Stmt stmt(
        db_,
        "INSERT INTO chats("
        "chat_id,type,title,username,bot_status,can_delete,can_restrict,"
        "can_pin,can_invite,can_manage,enabled,joined_at,left_at,"
        "last_seen_at,refreshed_at)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(chat_id) DO UPDATE SET"
        " type=CASE WHEN excluded.type<>'' THEN excluded.type ELSE chats.type END,"
        " title=CASE WHEN excluded.title<>'' THEN excluded.title ELSE chats.title END,"
        " username=CASE WHEN excluded.username<>'' THEN excluded.username ELSE chats.username END,"
        " bot_status=CASE WHEN excluded.bot_status<>'' THEN excluded.bot_status ELSE "
        "chats.bot_status END,"
        " can_delete=excluded.can_delete,"
        " can_restrict=excluded.can_restrict,"
        " can_pin=excluded.can_pin,"
        " can_invite=excluded.can_invite,"
        " can_manage=excluded.can_manage,"
        " enabled=CASE WHEN excluded.enabled=1 THEN 1 ELSE chats.enabled END,"
        " joined_at=CASE WHEN chats.joined_at=0 THEN excluded.joined_at ELSE chats.joined_at END,"
        " left_at=excluded.left_at,"
        " last_seen_at=MAX(chats.last_seen_at, excluded.last_seen_at),"
        " refreshed_at=MAX(chats.refreshed_at, excluded.refreshed_at)");
    stmt.bind(1, r.chatId);
    stmt.bind(2, r.type);
    stmt.bind(3, r.title);
    stmt.bind(4, r.username);
    stmt.bind(5, r.botStatus);
    stmt.bind(6, r.canDelete);
    stmt.bind(7, r.canRestrict);
    stmt.bind(8, r.canPin);
    stmt.bind(9, r.canInvite);
    stmt.bind(10, r.canManage);
    stmt.bind(11, r.enabled);
    stmt.bind(12, r.joinedAt);
    stmt.bind(13, r.leftAt);
    stmt.bind(14, r.lastSeenAt);
    stmt.bind(15, r.refreshedAt);
    stmt.run();
    if (const auto it = settingsCache_.find(r.chatId); it != settingsCache_.end()) {
        settingsLru_.erase(it->second.lruPos);
        settingsCache_.erase(it);
    }
}

std::optional<ChatRecord> Store::chat(std::int64_t chatId) {
    Stmt stmt(db_, (std::string{"SELECT "} + kChatColumns + " FROM chats WHERE chat_id=?").c_str());
    stmt.bind(1, chatId);
    if (!stmt.row()) {
        return std::nullopt;
    }
    return readChat(stmt);
}

std::vector<ChatRecord> Store::listChats(int limit, int offset, bool onlyActive) {
    const std::string sql = std::string{"SELECT "} + kChatColumns + " FROM chats" +
                            (onlyActive ? " WHERE left_at=0" : "") +
                            " ORDER BY last_seen_at DESC, chat_id DESC LIMIT ? OFFSET ?";
    Stmt stmt(db_, sql.c_str());
    stmt.bind(1, static_cast<std::int64_t>(limit));
    stmt.bind(2, static_cast<std::int64_t>(offset));
    std::vector<ChatRecord> out;
    while (stmt.row()) {
        out.push_back(readChat(stmt));
    }
    return out;
}

int Store::countChats(bool onlyActive) {
    Stmt stmt(
        db_,
        onlyActive ? "SELECT COUNT(*) FROM chats WHERE left_at=0" : "SELECT COUNT(*) FROM chats");
    return stmt.row() ? static_cast<int>(stmt.i64(0)) : 0;
}

int Store::countEnabledChats() {
    Stmt stmt(db_, "SELECT COUNT(*) FROM chats WHERE left_at=0 AND enabled=1");
    return stmt.row() ? static_cast<int>(stmt.i64(0)) : 0;
}

int Store::countWithoutDeleteRights() {
    Stmt stmt(db_,
              "SELECT COUNT(*) FROM chats WHERE left_at=0 AND"
              " (bot_status<>'creator' AND (bot_status<>'administrator'"
              "  OR can_delete=0))");
    return stmt.row() ? static_cast<int>(stmt.i64(0)) : 0;
}

void Store::setEnabled(std::int64_t chatId, bool enabled) {
    Stmt stmt(db_, "UPDATE chats SET enabled=? WHERE chat_id=?");
    stmt.bind(1, enabled);
    stmt.bind(2, chatId);
    stmt.run();
    const auto it = settingsCache_.find(chatId);
    if (it != settingsCache_.end()) {
        it->second.value->enabled = enabled;
    }
}

void Store::markLeft(std::int64_t chatId, std::int64_t now) {
    Stmt stmt(db_,
              "UPDATE chats SET left_at=?, bot_status='left', enabled=0"
              " WHERE chat_id=?");
    stmt.bind(1, now);
    stmt.bind(2, chatId);
    stmt.run();
    if (const auto it = settingsCache_.find(chatId); it != settingsCache_.end()) {
        settingsLru_.erase(it->second.lruPos);
        settingsCache_.erase(it);
    }
}

void Store::touch(std::int64_t chatId, std::int64_t now) {
    const auto it = lastSeenWrite_.find(chatId);
    if (it != lastSeenWrite_.end() && now - it->second < 120) {
        return;
    }
    Stmt stmt(db_, "UPDATE chats SET last_seen_at=? WHERE chat_id=?");
    stmt.bind(1, now);
    stmt.bind(2, chatId);
    stmt.run();
    if (lastSeenWrite_.size() > 20000) {
        maintenance(now);
    }
    lastSeenWrite_[chatId] = now;
}

std::shared_ptr<ChatSettings> Store::loadSettings(std::int64_t chatId) {
    if (const auto cached = settingsCache_.find(chatId); cached != settingsCache_.end()) {
        settingsLru_.splice(settingsLru_.begin(), settingsLru_, cached->second.lruPos);
        return cached->second.value;
    }

    auto s = std::make_shared<ChatSettings>();
    {
        Stmt stmt(db_, "SELECT enabled FROM chats WHERE chat_id=?");
        stmt.bind(1, chatId);
        if (stmt.row()) {
            s->enabled = stmt.boolean(0);
        }
    }
    {
        Stmt stmt(db_, "SELECT kind, delay_sec FROM chat_settings WHERE chat_id=?");
        stmt.bind(1, chatId);
        while (stmt.row()) {
            s->delays[stmt.text(0)] = static_cast<int>(stmt.i64(1));
        }
    }

    settingsLru_.push_front(chatId);
    settingsCache_.emplace(chatId, SettingsCacheEntry{s, settingsLru_.begin()});
    while (settingsCache_.size() > kMaxSettingsCache) {
        const auto victim = settingsLru_.back();
        settingsLru_.pop_back();
        settingsCache_.erase(victim);
    }
    return s;
}

std::shared_ptr<const ChatSettings> Store::settings(std::int64_t chatId) {
    return loadSettings(chatId);
}

void Store::setDelay(std::int64_t chatId, std::string_view kind, int seconds) {
    Stmt stmt(db_,
              "INSERT INTO chat_settings(chat_id,kind,delay_sec) VALUES(?,?,?)"
              " ON CONFLICT(chat_id,kind) DO UPDATE SET"
              " delay_sec=excluded.delay_sec");
    stmt.bind(1, chatId);
    stmt.bind(2, kind);
    stmt.bind(3, static_cast<std::int64_t>(seconds));
    stmt.run();
    auto s = loadSettings(chatId);
    s->delays[std::string{kind}] = seconds;
}

void Store::clearSettings(std::int64_t chatId) {
    Stmt stmt(db_, "DELETE FROM chat_settings WHERE chat_id=?");
    stmt.bind(1, chatId);
    stmt.run();
    auto s = loadSettings(chatId);
    s->delays.clear();
}

void Store::maintenance(std::int64_t now) {
    const auto cutoff = now - 7 * 86400;
    for (auto it = lastSeenWrite_.begin(); it != lastSeenWrite_.end();) {
        if (it->second < cutoff) {
            it = lastSeenWrite_.erase(it);
        } else {
            ++it;
        }
    }
    // 极端情况下（大量一次性群）给一个硬上限，避免 janitor 之前先涨爆。
    if (lastSeenWrite_.size() > 20000) {
        lastSeenWrite_.clear();
    }
}

void Store::addDeletion(std::int64_t chatId,
                        std::int64_t messageId,
                        std::string kind,
                        std::int64_t dueAt,
                        std::int64_t now,
                        std::int64_t targetUserId) {
    Stmt stmt(db_,
              "INSERT INTO deletions("
              "chat_id,message_id,target_user,kind,due_at,attempts,state,"
              "last_error,created_at)"
              " VALUES(?,?,?,?,?,0,'pending','',?)"
              " ON CONFLICT(chat_id,message_id) DO UPDATE SET"
              " target_user=excluded.target_user, kind=excluded.kind,"
              " due_at=excluded.due_at, state='pending', attempts=0,"
              " last_error=''");
    stmt.bind(1, chatId);
    stmt.bind(2, messageId);
    stmt.bind(3, targetUserId);
    stmt.bind(4, kind);
    stmt.bind(5, dueAt);
    stmt.bind(6, now);
    stmt.run();
}

std::optional<DeletionJob> Store::claimDeletion(std::int64_t now) {
    exec("BEGIN IMMEDIATE");
    DeletionJob job;
    bool found = false;
    {
        Stmt stmt(db_,
                  "SELECT chat_id,message_id,target_user,kind,due_at,attempts"
                  " FROM deletions WHERE state='pending' AND due_at<=?"
                  " ORDER BY due_at LIMIT 1");
        stmt.bind(1, now);
        if (stmt.row()) {
            found = true;
            job.chatId = stmt.i64(0);
            job.messageId = stmt.i64(1);
            job.targetUserId = stmt.i64(2);
            job.kind = stmt.text(3);
            job.dueAt = stmt.i64(4);
            job.attempts = static_cast<int>(stmt.i64(5));
        }
    }
    if (!found) {
        exec("ROLLBACK");
        return std::nullopt;
    }
    {
        Stmt stmt(db_,
                  "UPDATE deletions SET state='inflight'"
                  " WHERE chat_id=? AND message_id=?");
        stmt.bind(1, job.chatId);
        stmt.bind(2, job.messageId);
        stmt.run();
    }
    exec("COMMIT");
    return job;
}

void Store::finishDeletion(std::int64_t chatId,
                           std::int64_t messageId,
                           bool ok,
                           std::string_view error) {
    // 只允许 inflight → done/failed：任务被 /autodel off 取消后，
    // 迟到的 worker 不能把已删除的行重新标成完成/失败
    Stmt stmt(db_,
              "UPDATE deletions SET state=?, last_error=?"
              " WHERE chat_id=? AND message_id=? AND state='inflight'");
    // 必须显式 string_view：const char* 的三元表达式会优先匹配 bind(bool)，
    // 把 state 写成 1/0，而不是 done/failed。
    stmt.bind(1, std::string_view{ok ? "done" : "failed"});
    stmt.bind(2, error);
    stmt.bind(3, chatId);
    stmt.bind(4, messageId);
    stmt.run();
}

void Store::retryDeletion(std::int64_t chatId,
                          std::int64_t messageId,
                          std::int64_t nextDue,
                          std::string_view error) {
    Stmt stmt(db_,
              "UPDATE deletions SET state='pending', due_at=?,"
              " attempts=attempts+1, last_error=?"
              " WHERE chat_id=? AND message_id=? AND state='inflight'");
    stmt.bind(1, nextDue);
    stmt.bind(2, error);
    stmt.bind(3, chatId);
    stmt.bind(4, messageId);
    stmt.run();
}

void Store::cancelChatDeletions(std::int64_t chatId) {
    // 只取消媒体删除；命令与机器人回复的清理任务必须继续执行，
    // 否则 /autodel off 会把"20 秒后删回复"也一起取消。
    Stmt stmt(db_,
              "DELETE FROM deletions WHERE chat_id=?"
              " AND state IN ('pending','inflight')"
              " AND kind NOT IN ('ephemeral','bot_reply','command')");
    stmt.bind(1, chatId);
    stmt.run();
}

void Store::cancelKindDeletions(std::int64_t chatId, std::string_view kind) {
    Stmt stmt(db_,
              "DELETE FROM deletions WHERE chat_id=? AND kind=?"
              " AND state IN ('pending','inflight')");
    stmt.bind(1, chatId);
    stmt.bind(2, kind);
    stmt.run();
}

void Store::cancelMessageDeletion(std::int64_t chatId, std::int64_t messageId) {
    Stmt stmt(db_,
              "DELETE FROM deletions WHERE chat_id=? AND message_id=?"
              " AND state IN ('pending','inflight')");
    stmt.bind(1, chatId);
    stmt.bind(2, messageId);
    stmt.run();
}

int Store::pendingDeletions() {
    Stmt stmt(db_,
              "SELECT COUNT(*) FROM deletions"
              " WHERE state IN ('pending','inflight')");
    return stmt.row() ? static_cast<int>(stmt.i64(0)) : 0;
}

std::vector<std::int64_t> Store::chatsWithPendingDeletions() {
    Stmt stmt(db_,
              "SELECT DISTINCT chat_id FROM deletions"
              " WHERE state IN ('pending','inflight')"
              " AND kind NOT IN ('ephemeral','bot_reply','command')");
    std::vector<std::int64_t> out;
    while (stmt.row()) {
        out.push_back(stmt.i64(0));
    }
    return out;
}

int Store::pendingDeletionsFor(std::int64_t chatId) {
    Stmt stmt(db_,
              "SELECT COUNT(*) FROM deletions WHERE chat_id=?"
              " AND state IN ('pending','inflight')");
    stmt.bind(1, chatId);
    return stmt.row() ? static_cast<int>(stmt.i64(0)) : 0;
}

int Store::failedDeletions() {
    Stmt stmt(db_, "SELECT COUNT(*) FROM deletions WHERE state='failed'");
    return stmt.row() ? static_cast<int>(stmt.i64(0)) : 0;
}

int Store::resetInflight() {
    exec("UPDATE deletions SET state='pending' WHERE state='inflight'");
    return sqlite3_changes(db_);
}

void Store::purgeFinishedDeletions(std::int64_t before) {
    Stmt stmt(db_,
              "DELETE FROM deletions WHERE state IN ('done','failed')"
              " AND due_at<?");
    stmt.bind(1, before);
    stmt.run();
}

void Store::purgeAuditLog(std::int64_t before) {
    Stmt stmt(db_, "DELETE FROM audit_log WHERE ts<?");
    stmt.bind(1, before);
    stmt.run();
}

void Store::purgeOldPinnedMessages(std::int64_t before) {
    // 只保留一段时间的置顶保护；超过 Telegram 48h 删除窗口的旧记录没有
    // 待删任务会用到。当前置顶的消息会在下次补扫时重新写入。
    Stmt stmt(db_, "DELETE FROM pinned_messages WHERE pinned_at<?");
    stmt.bind(1, before);
    stmt.run();
}

void Store::markPinned(std::int64_t chatId, std::int64_t messageId, std::int64_t now) {
    Stmt stmt(db_,
              "INSERT OR REPLACE INTO pinned_messages"
              "(chat_id,message_id,pinned_at) VALUES(?,?,?)");
    stmt.bind(1, chatId);
    stmt.bind(2, messageId);
    stmt.bind(3, now);
    stmt.run();
}

bool Store::isPinned(std::int64_t chatId, std::int64_t messageId) {
    Stmt stmt(db_,
              "SELECT 1 FROM pinned_messages"
              " WHERE chat_id=? AND message_id=? LIMIT 1");
    stmt.bind(1, chatId);
    stmt.bind(2, messageId);
    return stmt.row();
}

void Store::replaceAdmins(std::int64_t chatId,
                          const std::vector<AdminRow>& rows,
                          std::int64_t now) {
    exec("BEGIN IMMEDIATE");
    {
        Stmt del(db_, "DELETE FROM chat_admins WHERE chat_id=?");
        del.bind(1, chatId);
        del.run();
    }
    for (const auto& row : rows) {
        Stmt ins(db_,
                 "INSERT OR REPLACE INTO chat_admins"
                 "(chat_id,user_id,status,display_name,updated_at)"
                 " VALUES(?,?,?,?,?)");
        ins.bind(1, chatId);
        ins.bind(2, row.userId);
        ins.bind(3, row.status);
        ins.bind(4, row.displayName);
        ins.bind(5, now);
        ins.run();
    }
    exec("COMMIT");
}

void Store::addAdmin(std::int64_t chatId,
                     std::int64_t userId,
                     std::string_view status,
                     std::string_view displayName,
                     std::int64_t now) {
    Stmt stmt(db_,
              "INSERT INTO chat_admins"
              "(chat_id,user_id,status,display_name,updated_at)"
              " VALUES(?,?,?,?,?)"
              " ON CONFLICT(chat_id,user_id) DO UPDATE SET"
              " status=excluded.status, display_name=excluded.display_name,"
              " updated_at=excluded.updated_at");
    stmt.bind(1, chatId);
    stmt.bind(2, userId);
    stmt.bind(3, status);
    stmt.bind(4, displayName);
    stmt.bind(5, now);
    stmt.run();
}

std::vector<ChatRecord> Store::chatsForAdmin(std::int64_t userId) {
    // 必须 c.*：kChatColumns 不带表前缀，JOIN 后 chat_id 会歧义
    const std::string sql =
        "SELECT c.* FROM chats c JOIN chat_admins a ON a.chat_id=c.chat_id"
        " WHERE a.user_id=? AND c.left_at=0"
        " ORDER BY c.last_seen_at DESC LIMIT 50";
    Stmt stmt(db_, sql.c_str());
    stmt.bind(1, userId);
    std::vector<ChatRecord> out;
    while (stmt.row()) {
        out.push_back(readChat(stmt));
    }
    return out;
}

bool Store::isChatAdmin(std::int64_t chatId, std::int64_t userId) {
    Stmt stmt(db_,
              "SELECT 1 FROM chat_admins WHERE chat_id=? AND user_id=?"
              " AND status IN ('creator','administrator') LIMIT 1");
    stmt.bind(1, chatId);
    stmt.bind(2, userId);
    return stmt.row();
}

void Store::audit(std::int64_t chatId,
                  std::int64_t actorId,
                  std::string_view action,
                  std::string_view detail) {
    Stmt stmt(db_,
              "INSERT INTO audit_log(ts,chat_id,actor_id,action,detail)"
              " VALUES(?,?,?,?,?)");
    stmt.bind(1, static_cast<std::int64_t>(std::time(nullptr)));
    stmt.bind(2, chatId);
    stmt.bind(3, actorId);
    stmt.bind(4, action);
    stmt.bind(5, detail);
    stmt.run();
}

std::int64_t Store::lastAuditAt(std::int64_t chatId, std::string_view action) {
    Stmt stmt(db_,
              "SELECT COALESCE(MAX(ts),0) FROM audit_log"
              " WHERE chat_id=? AND action=?");
    stmt.bind(1, chatId);
    stmt.bind(2, action);
    return stmt.row() ? stmt.i64(0) : 0;
}

void Store::checkpoint() {
    try {
        exec("PRAGMA wal_checkpoint(TRUNCATE)");
    } catch (const std::exception& e) {
        spdlog::warn("wal_checkpoint failed: {}", e.what());
    }
}

} // namespace warden
