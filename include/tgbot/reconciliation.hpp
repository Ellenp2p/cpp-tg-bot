#pragma once

#include <tgbot/chain_health.hpp>
#include <tgbot/red_packet_ledger.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace tgbot {

struct ReconciliationReport {
    std::size_t scanned = 0;      // 本轮检查了多少笔
    std::size_t confirmed = 0;    // 链上已成功 → refunded
    std::size_t requeued = 0;     // 链上失败/查不到 → 退回 expired 重新排队
    std::size_t stuck = 0;        // 超时没结果 → 标记 stuck 并告警
    std::size_t stillPending = 0; // 还在 mempool
    bool probeFailed = false;     // 探测本身出错（链/RPC 问题）
};

// 对账任务：补上"广播成功但进程崩了"的窗口。
//
// 只要 tx_ref 在广播时就落库（RefundService 的 TxRefRecorder 负责），
// 这里就能按 hash 回查链上真实状态，而不是盲目重发。
class Reconciler {
public:
    struct Options {
        std::chrono::seconds minAge{120};     // 进入 refunding 多久后才值得查
        std::int64_t stuckAfterSeconds = 600; // 多久还没结果就判 stuck
        std::size_t batch = 50;
    };

    Reconciler(RedPacketLedger& ledger, ChainMonitor& monitor, Options options);

    boost::asio::awaitable<ReconciliationReport> run(ChainProbe probe, AlertSink alert);

private:
    RedPacketLedger& ledger_;
    ChainMonitor& monitor_;
    Options options_;
};

} // namespace tgbot
