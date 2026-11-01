#include <tgbot/red_packet_service.hpp>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <exception>
#include <string>
#include <utility>

namespace tgbot {

namespace net = boost::asio;

std::int64_t createRedPacket(RedPacketLedger& ledger, JobQueue& jobs,
                             const RedPacketSpec& spec) {
    const auto id = ledger.create(spec.chatId, spec.creatorId, spec.totalCents,
                                  spec.count, spec.ttl);

    Job job;
    job.key = "red_packet_refund:" + std::to_string(id);
    job.kind = "red_packet_refund";
    job.payload = nlohmann::json{{"packet_id", id}}.dump();
    job.runAt = std::chrono::system_clock::now() + spec.ttl;
    jobs.enqueue(job);
    return id;
}

JobQueue::Handler makeRedPacketJobHandler(RedPacketLedger& ledger,
                                          RefundHandler refund) {
    return [&ledger, refund = std::move(refund)](const JobRecord& job)
               -> net::awaitable<void> {
        if (job.kind != "red_packet_refund") {
            co_return;  // 不是我的任务，交给别的 handler
        }

        std::int64_t packetId = 0;
        try {
            packetId = nlohmann::json::parse(job.payload)
                           .value("packet_id", std::int64_t{0});
        } catch (const std::exception& e) {
            spdlog::warn("退款任务 {} payload 损坏: {}", job.key, e.what());
            co_return;
        }
        if (packetId == 0) {
            co_return;
        }

        const auto row = ledger.get(packetId);
        if (!row.has_value()) {
            spdlog::warn("退款任务 {}: 红包 {} 不存在", job.key, packetId);
            co_return;
        }
        if (row->state == "refunded" || row->state == "refunding") {
            co_return;  // 已经退过 / 正在退
        }
        if (row->state != "open" && row->state != "expired") {
            co_return;  // 抢完了，没有剩余
        }

        // 抢占退款权：只有把状态改成 refunding 的那一次才真正执行
        if (!ledger.beginRefund(packetId)) {
            co_return;
        }

        if (row->remainingCents <= 0) {
            ledger.finishRefund(packetId, "");
            co_return;
        }

        const auto ref = co_await refund(*row);
        ledger.finishRefund(packetId, ref);
        spdlog::info("红包 {} 过期退款完成: {} 分 → {}, 凭据={}", packetId,
                     row->remainingCents, row->creatorId, ref);
    };
}

}  // namespace tgbot
