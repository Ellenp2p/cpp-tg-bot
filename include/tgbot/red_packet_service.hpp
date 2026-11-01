#pragma once

#include <tgbot/job_queue.hpp>
#include <tgbot/red_packet_ledger.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace tgbot {

// 退款钩子。
//   - 今天：记账/打日志/在群里发一条"剩余已退回"
//   - 接链上：发起一笔转账，返回 tx hash
// 抛异常 = 本次退款失败，任务队列会指数退避重试；
// 因为账本的 refunding/refunded 状态机，重试绝不会造成重复退款。
using RefundHandler =
    std::function<boost::asio::awaitable<std::string>(const RedPacketRow&)>;

struct RedPacketSpec {
    std::int64_t chatId = 0;
    std::int64_t creatorId = 0;
    std::int64_t totalCents = 0;
    int count = 1;
    std::chrono::seconds ttl{3600};
};

// 建红包：落账 + 排一条"到期退款"的持久化任务（进程重启也不会丢）
std::int64_t createRedPacket(RedPacketLedger& ledger, JobQueue& jobs,
                             const RedPacketSpec& spec);

// 组装 worker 处理函数（按 kind 分发；你自己的任务可以并联起来）
JobQueue::Handler makeRedPacketJobHandler(RedPacketLedger& ledger,
                                          RefundHandler refund);

}  // namespace tgbot
