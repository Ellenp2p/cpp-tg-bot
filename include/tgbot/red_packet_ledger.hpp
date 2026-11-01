#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace tgbot {

// 红包账本里的一行
struct RedPacketRow {
    std::int64_t id = 0;
    std::int64_t chatId = 0;
    std::int64_t messageId = 0;
    std::int64_t creatorId = 0;
    std::int64_t totalCents = 0;
    std::int64_t totalCount = 0;
    std::int64_t remainingCents = 0;
    std::int64_t remainingCount = 0;
    // open → exhausted（抢完）/ expired（过期）→ refunding → refunded
    std::string state;
    std::int64_t createdAt = 0;  // unix 秒
    std::int64_t expiresAt = 0;  // unix 秒
    std::string txRef;  // 外部凭据（链上转账就是 tx hash）
};

struct GrabRow {
    std::int64_t userId = 0;
    std::int64_t cents = 0;
    std::int64_t createdAt = 0;
    std::string userName;
};

// 红包账本：所有金额都在这里，UI 只负责展示。
//
// 两个关键约束，靠 SQL 保证而不是靠调用方自觉：
//   1. 领取是带条件的 UPDATE（remaining_cents >= 本次金额），不会超发
//   2. 退款用状态机 open/expired → refunding → refunded，只会退一次
class RedPacketLedger {
public:
    // path 传 ":memory:" 可用于测试
    explicit RedPacketLedger(std::string path);
    ~RedPacketLedger();
    RedPacketLedger(const RedPacketLedger&) = delete;
    RedPacketLedger& operator=(const RedPacketLedger&) = delete;

    std::int64_t create(std::int64_t chatId, std::int64_t creatorId,
                        std::int64_t totalCents, int count,
                        std::chrono::seconds ttl);

    std::optional<RedPacketRow> get(std::int64_t id) const;
    std::vector<GrabRow> grabs(std::int64_t id) const;
    bool hasGrabbed(std::int64_t id, std::int64_t userId) const;

    // 原子领取：成功返回 true；余额不足/已结束/重复领取都返回 false
    bool grab(std::int64_t id, std::int64_t userId, std::string_view userName,
              std::int64_t cents);

    // 绑定到具体消息（发出去之后才知道 message_id）
    void attachMessage(std::int64_t id, std::int64_t messageId);

    // open → expired；返回 true 表示这次真的改了状态
    bool markExpired(std::int64_t id);
    // open/expired → refunding；返回 true 才由你负责执行退款
    bool beginRefund(std::int64_t id);
    // refunding → refunded，记下外部凭据
    void finishRefund(std::int64_t id, std::string_view txRef);

private:
    void exec(const char* sql) const;
    sqlite3* db_ = nullptr;
};

}  // namespace tgbot
