#pragma once

#include <tgbot/job_queue.hpp>
#include <tgbot/red_packet_ledger.hpp>

#include <chrono>
#include <cstdint>

namespace tgbot {

struct RedPacketSpec {
    std::int64_t chatId = 0;
    std::int64_t creatorId = 0;
    std::int64_t totalCents = 0;
    int count = 1;
    std::chrono::seconds ttl{3600};
};

// 建红包：落账 + 排一条"到期退款"的持久化任务（进程重启也不会丢）。
// 退款本身是批量的，见 red_packet_batch.hpp 的 RefundService。
std::int64_t createRedPacket(RedPacketLedger& ledger, JobQueue& jobs,
                             const RedPacketSpec& spec);

}  // namespace tgbot
