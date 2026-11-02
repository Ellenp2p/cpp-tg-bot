#pragma once

// Warden 的持久层：一个 SQLite 文件装下
//   chats         机器人见过的群 + 自身权限快照
//   chat_settings 每个群、每种媒体的删除时限
//   deletions     待执行的删除任务（停机恢复的关键）
//   chat_admins   群管理员缓存（"我管理的群"用）
//   audit_log     管理动作审计
//
// 约定：单 io_context 线程内同步调用（SQLite 单条写入是微秒级），
// 不做跨进程并发。热路径（每条消息）只读内存缓存，不碰库。

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

struct sqlite3;

namespace warden {

struct ChatRecord {
    std::int64_t chatId = 0;
    std::string type;
    std::string title;
    std::string username;
    std::string botStatus = "unknown"; // creator/administrator/member/left/kicked
    bool canDelete = false;
    bool canRestrict = false;
    bool canPin = false;
    bool canInvite = false;
    bool canManage = false;
    bool enabled = false;
    std::int64_t joinedAt = 0;
    std::int64_t leftAt = 0;
    std::int64_t lastSeenAt = 0;
    std::int64_t refreshedAt = 0;

    bool active() const { return leftAt == 0; }
    bool isAdmin() const { return botStatus == "creator" || botStatus == "administrator"; }
};

struct DeletionJob {
    std::int64_t chatId = 0;
    std::int64_t messageId = 0;
    // 仅 kind=="ephemeral" 使用：ephemeral 消息的接收者 user_id
    std::int64_t targetUserId = 0;
    std::string kind;
    std::int64_t dueAt = 0;
    int attempts = 0;
};

struct AdminRow {
    std::int64_t userId = 0;
    std::string status; // creator / administrator
    std::string displayName;
};

// 群设置（带内存缓存；delays 里没有的 kind 走 kindDefaultDelay）
struct ChatSettings {
    bool enabled = false;
    std::map<std::string, int, std::less<>> delays;
};

class Store {
public:
    explicit Store(const std::string& path);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // ---- 群登记 ----
    // 只插入不覆盖（消息里第一次见到某个群时用）
    void ensureChat(std::int64_t chatId, std::string type, std::string title, std::int64_t now);
    // 全量更新（my_chat_member / 刷新权限后）。空字符串不会覆盖已有值。
    void saveChat(const ChatRecord& record);
    std::optional<ChatRecord> chat(std::int64_t chatId);
    // onlyActive=true 时排除已退出/被踢的群
    std::vector<ChatRecord> listChats(int limit, int offset, bool onlyActive);
    int countChats(bool onlyActive);
    int countEnabledChats();
    int countWithoutDeleteRights();

    void setEnabled(std::int64_t chatId, bool enabled);
    void markLeft(std::int64_t chatId, std::int64_t now);
    // 最后活跃时间（节流：同一群 2 分钟内只写一次库）
    void touch(std::int64_t chatId, std::int64_t now);

    // ---- 设置 ----
    // shared_ptr 让缓存可以安全淘汰：调用方持有期间，janitor 删除 cache
    // entry 不会悬垂。
    std::shared_ptr<const ChatSettings> settings(std::int64_t chatId);
    // 清理 last_seen 写合并等进程级缓存，由 janitor 定期调用
    void maintenance(std::int64_t now);
    void setDelay(std::int64_t chatId, std::string_view kind, int seconds);
    void clearSettings(std::int64_t chatId);

    // ---- 删除任务 ----
    // 同一 (chat,message) 重复排程会更新到期时间而不是插两条
    void addDeletion(std::int64_t chatId,
                     std::int64_t messageId,
                     std::string kind,
                     std::int64_t dueAt,
                     std::int64_t now,
                     std::int64_t targetUserId = 0);
    std::optional<DeletionJob> claimDeletion(std::int64_t now);
    void finishDeletion(std::int64_t chatId,
                        std::int64_t messageId,
                        bool ok,
                        std::string_view error);
    void retryDeletion(std::int64_t chatId,
                       std::int64_t messageId,
                       std::int64_t nextDue,
                       std::string_view error);
    void cancelChatDeletions(std::int64_t chatId);
    void cancelKindDeletions(std::int64_t chatId, std::string_view kind);
    void cancelMessageDeletion(std::int64_t chatId, std::int64_t messageId);
    int pendingDeletions();
    int pendingDeletionsFor(std::int64_t chatId);
    // 有未完成删除任务的群（启动时做置顶补扫用，去重）
    std::vector<std::int64_t> chatsWithPendingDeletions();
    int failedDeletions();
    // 进程启动时调用：把上次崩溃时卡在 inflight 的任务放回 pending
    int resetInflight();
    void purgeFinishedDeletions(std::int64_t before);
    void purgeAuditLog(std::int64_t before);
    void purgeOldPinnedMessages(std::int64_t before);

    // ---- 置顶消息 ----
    // 置顶过的消息永不自动删除（保守策略：即使后来取消置顶也不补删，
    // 避免"置顶/取消置顶"与定时器竞争导致误删）。
    void markPinned(std::int64_t chatId, std::int64_t messageId, std::int64_t now);
    bool isPinned(std::int64_t chatId, std::int64_t messageId);

    // ---- 管理员缓存 ----
    void replaceAdmins(std::int64_t chatId, const std::vector<AdminRow>& rows, std::int64_t now);
    // 单条补充：管理员用命令时顺手把 TA 记进名册，不用整表重拉
    void addAdmin(std::int64_t chatId,
                  std::int64_t userId,
                  std::string_view status,
                  std::string_view displayName,
                  std::int64_t now);
    std::vector<ChatRecord> chatsForAdmin(std::int64_t userId);
    // 缓存名册里的群管理员判断（面板回调鉴权用；实时命令仍走 getChatMember）
    bool isChatAdmin(std::int64_t chatId, std::int64_t userId);

    // ---- 审计 ----
    void audit(std::int64_t chatId,
               std::int64_t actorId,
               std::string_view action,
               std::string_view detail);
    std::int64_t lastAuditAt(std::int64_t chatId, std::string_view action);

    void checkpoint();

private:
    void exec(const char* sql) const;

    std::shared_ptr<ChatSettings> loadSettings(std::int64_t chatId);

    sqlite3* db_ = nullptr;
    struct SettingsCacheEntry {
        std::shared_ptr<ChatSettings> value;
        std::list<std::int64_t>::iterator lruPos;
    };
    static constexpr std::size_t kMaxSettingsCache = 4096;
    std::unordered_map<std::int64_t, SettingsCacheEntry> settingsCache_;
    std::list<std::int64_t> settingsLru_; // front = 最近使用
    std::unordered_map<std::int64_t, std::int64_t> lastSeenWrite_;
};

} // namespace warden
