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
    // open → exhausted（抢完）/ expired（过期）
    //      → refunding（已抢占退款权）→ refunded
    // refunding 超时/超次数 → stuck（需要人看）
    std::string state;
    std::int64_t createdAt = 0;  // unix 秒
    std::int64_t expiresAt = 0;  // unix 秒
    std::string txRef;           // 链上凭据（tx hash），广播一拿到就写
    std::int64_t refundStartedAt = 0;  // 进入 refunding 的时刻
    int refundAttempts = 0;            // 已经尝试退过几次
};

struct GrabRow {
    std::int64_t userId = 0;
    std::int64_t cents = 0;
    std::int64_t createdAt = 0;
    std::string userName;
};

// 红包账本：所有金额都在这里，UI 只负责展示。
//
// 关键约束都靠 SQL 保证，而不是靠调用方自觉：
//   1. 领取是带条件的 UPDATE（remaining_cents >= 本次金额），不会超发
//   2. 退款是状态机：open/expired → refunding → refunded，refunding 只会被抢到一次
//   3. tx_ref 在广播时就写入，进程崩了也能靠它对账
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
    // 列出"该退款"的红包：state='expired'，或 state='open' 但已过期
    std::vector<RedPacketRow> listRefundable(std::int64_t nowSeconds,
                                             std::size_t limit) const;
    // open/expired → refunding；返回 true 才由你负责执行退款
    bool beginRefund(std::int64_t id);
    // 广播一拿到 hash 就立刻写（别等到确认，否则崩溃窗口就没法对账）
    bool setTxRef(std::int64_t id, std::string_view txRef);
    // refunding → refunded，记下外部凭据
    void finishRefund(std::int64_t id, std::string_view txRef);
    // refunding → expired（批量里明确失败的那几笔，留给下一轮重试）
    bool revertRefund(std::int64_t id);

    // 对账用：列出"卡在 refunding 超过 N 秒"的红包
    std::vector<RedPacketRow> listRefunding(std::int64_t nowSeconds,
                                            std::int64_t minAgeSeconds,
                                            std::size_t limit) const;
    // refunding → stuck（超时/超次数，需要人工或对账确认）
    bool markStuck(std::int64_t id);
    // stuck → expired（对账确认链上没成功，重新排队）
    bool recoverFromStuck(std::int64_t id);
    std::size_t countStuck() const;

private:
    void exec(const char* sql) const;
    void migrate();
    bool hasColumn(const char* table, const char* column) const;
    sqlite3* db_ = nullptr;
};

}  // namespace tgbot
