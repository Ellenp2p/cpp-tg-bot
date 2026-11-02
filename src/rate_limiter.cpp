#include <tgbot/rate_limiter.hpp>

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>

namespace tgbot {

namespace net = boost::asio;

net::awaitable<void> RateLimiter::beforeSend(std::int64_t chatId) {
    const auto executor = co_await net::this_coro::executor;
    const auto now = std::chrono::steady_clock::now();

    // 顺序很关键：**先占槽位、再等待**。
    // 如果先等再占，N 个并发协程会读到同一个 "下一个可用时刻"，然后同一瞬间
    // 一起醒来、一起发送——同聊天 1 msg/s 与全局 30 msg/s 都会被成倍突破，
    // 直接换来 Telegram 的 429。
    const auto slot = std::max({now, nextGlobalSend_, nextChatSend_[chatId]});
    nextGlobalSend_ = slot + kGlobalInterval;
    nextChatSend_[chatId] = slot + kChatInterval;

    if (slot > now) {
        net::steady_timer timer(executor);
        timer.expires_at(slot);
        co_await timer.async_wait(net::use_awaitable);
    }
}

} // namespace tgbot
