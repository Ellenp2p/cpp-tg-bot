#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>

namespace tgbot {

// 协程睡眠：handler 里 co_await co_sleep(60s) 即可，比手写 steady_timer 省事。
// 注意：handler 跑在按 chat 的 strand 上，睡眠期间**同一聊天**的后续 update
// 会被阻塞（不同聊天不受影响）。要做"发完就忘"的延时动作（例如延时删消息），
// 应该另起 co_spawn 到普通 executor，而不是在 handler 里睡。
inline boost::asio::awaitable<void> co_sleep(std::chrono::steady_clock::duration d) {
    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor, d);
    co_await timer.async_wait(boost::asio::use_awaitable);
}

} // namespace tgbot
