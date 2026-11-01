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

RefundService::RefundService(RedPacketLedger& ledger, Options options)
    : ledger_(ledger), options_(options) {}

boost::asio::awaitable<std::size_t> RefundService::flush(
    BatchRefundHandler handler) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    const auto rows = ledger_.listRefundable(now, options_.maxBatch);

    RefundBatch batch;
    batch.id = "batch-" + std::to_string(++sequence_) + "-" +
               std::to_string(now);
    std::vector<std::int64_t> claimed;

    for (const auto& row : rows) {
        if (row.remainingCents <= 0) {
            continue;  // 抢完了，没有余额可退
        }
        const auto amount =
            Amount::fromCents(row.remainingCents, options_.decimals);
        if (!amount.has_value()) {
            spdlog::warn("红包 {} 金额 {} 分无法转换成 {} 位精度", row.id,
                         row.remainingCents, options_.decimals);
            continue;
        }
        if (!ledger_.beginRefund(row.id)) {
            continue;  // 别人已经抢到退款权
        }
        claimed.push_back(row.id);
        batch.items.push_back(
            RefundItem{row.id, row.creatorId, row.remainingCents, *amount});
    }

    lastBatchSize_ = batch.items.size();
    if (batch.items.empty()) {
        co_return 0;
    }

    RefundBatchResult result;
    try {
        result = co_await handler(batch);
    } catch (const std::exception& e) {
        // 整批失败：全部退回 expired，由任务队列退避后重试
        for (const auto id : claimed) {
            ledger_.revertRefund(id);
        }
        spdlog::warn("退款批次 {} 失败（{} 笔，共 {}）: {}", batch.id,
                     batch.items.size(), batch.total().toString(), e.what());
        throw;
    }

    std::size_t done = 0;
    for (const auto& item : batch.items) {
        const bool failed =
            std::find(result.failedPacketIds.begin(),
                      result.failedPacketIds.end(),
                      item.packetId) != result.failedPacketIds.end();
        if (failed) {
            ledger_.revertRefund(item.packetId);
            continue;
        }
        ledger_.finishRefund(item.packetId, result.ref);
        ++done;
    }

    spdlog::info("退款批次 {} 完成：{} 笔成功 / {}，凭据={}", batch.id, done,
                 batch.items.size(), result.ref);
    co_return done;
}

boost::asio::awaitable<void> RefundService::handleJob(
    const JobRecord& job, BatchRefundHandler handler) {
    if (job.kind != "red_packet_refund") {
        co_return;  // 不是我的任务
    }
    co_await flush(std::move(handler));
}

}  // namespace tgbot
