#include <tgbot/rate_limiter.hpp>

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>

namespace tgbot {

namespace net = boost::asio;

net::awaitable<void> RateLimiter::beforeSend(std::int64_t chatId) {
    const auto now = std::chrono::steady_clock::now();
    // 全局槽位与该聊天的槽位取较晚者
    const auto next = std::max({now, nextGlobalSend_, nextChatSend_[chatId]});
    if (next > now) {
        net::steady_timer timer(co_await net::this_coro::executor, next);
        co_await timer.async_wait(net::use_awaitable);
    }
    // 醒后再取一次时间，避免等待期间被其他协程占掉的槽位重叠
    const auto slot = std::max(std::chrono::steady_clock::now(), next);
    nextGlobalSend_ = slot + kGlobalInterval;
    nextChatSend_[chatId] = slot + kChatInterval;
}

}  // namespace tgbot
