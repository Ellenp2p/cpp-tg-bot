#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>

namespace tgbot {

// 协程睡眠：handler 里 co_await co_sleep(60s) 即可，比手写 steady_timer 省事。
// 挂起期间不占用任何 strand，同聊天的其他消息照常处理。
inline boost::asio::awaitable<void> co_sleep(
    std::chrono::steady_clock::duration d) {
    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor, d);
    co_await timer.async_wait(boost::asio::use_awaitable);
}

}  // namespace tgbot
