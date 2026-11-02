#pragma once

#include <tgbot/amount.hpp>
#include <tgbot/chain_health.hpp>
#include <tgbot/job_queue.hpp>
#include <tgbot/red_packet_ledger.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tgbot {

// 一笔待退款
struct RefundItem {
    std::int64_t packetId = 0;
    std::int64_t creatorId = 0;
    std::int64_t cents = 0; // 便于展示/对账
    Amount amount;          // 链上最小单位（wei / token 最小单位）
};

struct RefundBatch {
    std::string id; // 本地批次号，日志/对账用
    std::vector<RefundItem> items;

    Amount total() const;
};

struct RefundBatchResult {
    // 链上凭据：tx hash / 批次 id。写回每个红包的 tx_ref
    std::string ref;
    // 明确失败的红包：退回 expired 状态，留到下一轮重试。
    // 不填 = 整批成功。抛异常 = 整批失败（也是留到下一轮）。
    std::vector<std::int64_t> failedPacketIds;
};

// 广播一拿到 tx hash 就立刻调用它落库。
// 千万不要"等确认了再记"——那样崩溃发生在中间就没法对账了。
using TxRefRecorder = std::function<void(std::string_view txRef)>;

// 批量退款处理器：一次调用把一整批退款发出去。
// 链下可以只是记账；链上就是一次批量转账（multisend / 合约 payout），
// 比逐笔发交易省 gas、也省 RPC 往返。
using BatchRefundHandler =
    std::function<boost::asio::awaitable<RefundBatchResult>(const RefundBatch&, TxRefRecorder)>;

// 把"已经到期但没退"的红包合并成批次退款的链下框架。
//
//   flush() 一次抓取所有可退红包 → 逐个抢 refunding → 调一次 handler
//   → 成功的落 refunded + tx_ref，失败的退回 expired 等下一轮
//
// 熔断：连续失败到阈值后暂停退款（避免 RPC 挂了还疯狂重试），
// 恢复交给对账任务探测。
class RefundService {
public:
    struct Options {
        int decimals = 18;         // 链上精度：ETH 18，USDT 6
        std::size_t maxBatch = 50; // 单批上限（一次交易别塞太多）
        int maxAttempts = 5;       // 单笔重试上限，超了就 stuck + 告警
    };

    RefundService(RedPacketLedger& ledger,
                  ChainMonitor& monitor,
                  Options options,
                  AlertSink alert = {});

    // 任务队列入口：不管触发的是哪个红包，都会把当前所有到期的合并处理
    boost::asio::awaitable<void> handleJob(const JobRecord& job, BatchRefundHandler handler);

    // 手动冲刷一批，返回本次成功退掉的红包数
    boost::asio::awaitable<std::size_t> flush(BatchRefundHandler handler);

    std::size_t lastBatchSize() const { return lastBatchSize_; }

private:
    RedPacketLedger& ledger_;
    ChainMonitor& monitor_;
    Options options_;
    AlertSink alert_;
    std::size_t lastBatchSize_ = 0;
    std::uint64_t sequence_ = 0;
};

} // namespace tgbot
