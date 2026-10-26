#pragma once

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <map>

namespace tgbot {

// 发送限速器：全局约 30 msg/s（Telegram 限制），单聊天 1 msg/s。
//
// 实现为"最小发送间隔"调度：每个协程发送前 co_await 到属于自己的时间槽，
// 由于所有协程跑在单个 io_context 线程上，无需加锁。
// 前提：BotClient 只能在同一个 io_context 的单线程里使用。
class RateLimiter {
public:
    boost::asio::awaitable<void> beforeSend(std::int64_t chatId);

private:
    static constexpr auto kGlobalInterval = std::chrono::milliseconds{34};  // ~30/s
    static constexpr auto kChatInterval = std::chrono::seconds{1};

    std::chrono::steady_clock::time_point nextGlobalSend_{};
    std::map<std::int64_t, std::chrono::steady_clock::time_point> nextChatSend_;
};

}  // namespace tgbot
