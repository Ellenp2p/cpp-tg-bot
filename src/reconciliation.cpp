#include <tgbot/reconciliation.hpp>

#include <spdlog/spdlog.h>

#include <exception>
#include <string>
#include <utility>

namespace tgbot {

namespace net = boost::asio;

namespace {

std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

Reconciler::Reconciler(RedPacketLedger& ledger, ChainMonitor& monitor,
                       Options options)
    : ledger_(ledger), monitor_(monitor), options_(options) {}

boost::asio::awaitable<ReconciliationReport> Reconciler::run(
    ChainProbe probe, AlertSink alert) {
    ReconciliationReport report;
    const auto now = nowSeconds();

    const auto rows = ledger_.listRefunding(now, options_.minAge.count(),
                                            options_.batch);
    report.scanned = rows.size();

    for (const auto& row : rows) {
        const auto age = now - row.refundStartedAt;

        // 没有 tx hash：广播前就崩了，或者 handler 没及时记录。
        // 没有任何凭据可以查链，只能等超时后交给人看。
        if (row.txRef.empty()) {
            if (age >= options_.stuckAfterSeconds && ledger_.markStuck(row.id)) {
                ++report.stuck;
                if (alert) {
                    co_await alert("error",
                                   "红包 " + std::to_string(row.id) +
                                       " 卡在退款中且没有 tx hash（"
                                       "可能广播前崩了），需要人工确认");
                }
            }
            continue;
        }

        ChainTxStatus status = ChainTxStatus::kUnknown;
        bool probeError = false;
        std::string probeMessage;
        try {
            status = co_await probe(row.txRef);
            monitor_.recordSuccess();
        } catch (const std::exception& e) {
            // co_await 不允许出现在 catch 块里，先记下来，出了 catch 再告警
            monitor_.recordFailure(e.what());
            probeError = true;
            probeMessage = e.what();
        }
        if (probeError) {
            report.probeFailed = true;
            if (alert) {
                co_await alert(
                    "error",
                    std::string{"对账探测失败（链/RPC 异常）: "} + probeMessage);
            }
            break;  // 链都不通，继续扫没有意义
        }

        switch (status) {
            case ChainTxStatus::kConfirmed:
                ledger_.finishRefund(row.id, row.txRef);
                ++report.confirmed;
                spdlog::info("对账：红包 {} 链上已成功，标记 refunded", row.id);
                break;

            case ChainTxStatus::kFailed:
                // 交易失败 = 钱没动，退回重试
                ledger_.revertRefund(row.id);
                ++report.requeued;
                spdlog::warn("对账：红包 {} 链上失败，退回重试", row.id);
                break;

            case ChainTxStatus::kNotFound:
                if (age >= options_.stuckAfterSeconds) {
                    if (ledger_.markStuck(row.id)) {
                        ++report.stuck;
                        if (alert) {
                            co_await alert(
                                "warn", "红包 " + std::to_string(row.id) +
                                            " 的 tx " + row.txRef +
                                            " 在链上查不到，已标记 stuck");
                        }
                    }
                } else {
                    ledger_.revertRefund(row.id);  // 多半还没广播出去，重试
                    ++report.requeued;
                }
                break;

            case ChainTxStatus::kPending:
                ++report.stillPending;
                break;

            case ChainTxStatus::kUnknown:
                monitor_.recordFailure("probe returned unknown");
                report.probeFailed = true;
                break;
        }
    }

    co_return report;
}

}  // namespace tgbot
