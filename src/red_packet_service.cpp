#include <tgbot/red_packet_service.hpp>

#include <nlohmann/json.hpp>

#include <string>

namespace tgbot {

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

}  // namespace tgbot
