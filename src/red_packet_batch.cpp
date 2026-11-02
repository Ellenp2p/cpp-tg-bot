#include <tgbot/red_packet_batch.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <string>
#include <utility>

namespace tgbot {

namespace net = boost::asio;

Amount RefundBatch::total() const {
    Amount sum;
    for (const auto& item : items) {
        sum = sum + item.amount;
    }
    return sum;
}

RefundService::RefundService(RedPacketLedger& ledger,
                             ChainMonitor& monitor,
                             Options options,
                             AlertSink alert)
    : ledger_(ledger), monitor_(monitor), options_(options), alert_(std::move(alert)) {}

boost::asio::awaitable<std::size_t> RefundService::flush(BatchRefundHandler handler) {
    if (monitor_.paused()) {
        spdlog::warn(
            "退款暂停：链/RPC 连续失败 {} 次（最近错误：{}），"
            "等待对账任务探测恢复",
            monitor_.consecutiveFailures(),
            monitor_.lastError());
        co_return 0;
    }

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    const auto rows = ledger_.listRefundable(now, options_.maxBatch);

    RefundBatch batch;
    batch.id = "batch-" + std::to_string(++sequence_) + "-" + std::to_string(now);
    std::vector<std::int64_t> claimed;

    for (const auto& row : rows) {
        if (row.remainingCents <= 0) {
            continue; // 抢完了，没有余额可退
        }
        if (row.refundAttempts >= options_.maxAttempts) {
            // 反复失败：不再自动重试，标 stuck 等对账/人工
            if (ledger_.markStuck(row.id)) {
                spdlog::error(
                    "红包 {} 退款已尝试 {} 次仍失败，标记 stuck", row.id, row.refundAttempts);
                if (alert_) {
                    co_await alert_("error",
                                    "红包 " + std::to_string(row.id) + " 退款连续失败 " +
                                        std::to_string(row.refundAttempts) +
                                        " 次，已停止自动重试，请检查链上状态");
                }
            }
            continue;
        }
        const auto amount = Amount::fromCents(row.remainingCents, options_.decimals);
        if (!amount.has_value()) {
            spdlog::warn("红包 {} 金额 {} 分无法转换成 {} 位精度",
                         row.id,
                         row.remainingCents,
                         options_.decimals);
            continue;
        }
        if (!ledger_.beginRefund(row.id)) {
            continue; // 别人已经抢到退款权
        }
        claimed.push_back(row.id);
        batch.items.push_back(RefundItem{row.id, row.creatorId, row.remainingCents, *amount});
    }

    lastBatchSize_ = batch.items.size();
    if (batch.items.empty()) {
        co_return 0;
    }

    // 广播一拿到 hash 就落库，崩溃了也能靠它对账
    const TxRefRecorder recorder = [this, &batch](std::string_view ref) {
        for (const auto& item : batch.items) {
            ledger_.setTxRef(item.packetId, ref);
        }
    };

    RefundBatchResult result;
    bool batchFailed = false;
    std::string batchError;
    try {
        result = co_await handler(batch, recorder);
        monitor_.recordSuccess();
    } catch (const std::exception& e) {
        // co_await 不能在 catch 块里，所以这里只记录，处理放到 catch 之外
        monitor_.recordFailure(e.what());
        batchFailed = true;
        batchError = e.what();
    }

    if (batchFailed) {
        for (const auto id : claimed) {
            ledger_.revertRefund(id);
        }
        spdlog::warn("退款批次 {} 失败（{} 笔，共 {}）: {}",
                     batch.id,
                     batch.items.size(),
                     batch.total().toString(),
                     batchError);
        if (alert_ && monitor_.paused()) {
            co_await alert_("error", "链/RPC 疑似不可用，已暂停自动退款：" + batchError);
        }
        throw std::runtime_error(batchError); // 让任务队列退避重试
    }

    std::size_t done = 0;
    for (const auto& item : batch.items) {
        const bool failed = std::find(result.failedPacketIds.begin(),
                                      result.failedPacketIds.end(),
                                      item.packetId) != result.failedPacketIds.end();
        if (failed) {
            ledger_.revertRefund(item.packetId);
            continue;
        }
        ledger_.finishRefund(item.packetId, result.ref);
        ++done;
    }

    spdlog::info("退款批次 {} 完成：{} 笔成功 / {}，凭据={}",
                 batch.id,
                 done,
                 batch.items.size(),
                 result.ref);
    co_return done;
}

boost::asio::awaitable<void> RefundService::handleJob(const JobRecord& job,
                                                      BatchRefundHandler handler) {
    if (job.kind != "red_packet_refund") {
        co_return; // 不是我的任务
    }
    co_await flush(std::move(handler));
}

} // namespace tgbot
