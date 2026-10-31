#pragma once

#include <tgbot/bot_client.hpp>
#include <tgbot/dispatcher.hpp>

#include <atomic>

namespace tgbot {

// 长轮询主循环：offset 持久化推进；每个 update co_spawn 独立协程处理。
// 连接失败指数退避重试。
// shuttingDown 可选：置位后循环不再派发新 update（停机期间的消息直接忽略，
// 由下次启动时的积压清空兜底）。
boost::asio::awaitable<void> longPollLoop(
    BotClient& bot, Dispatcher& dispatcher,
    const std::atomic<bool>* shuttingDown = nullptr);

}  // namespace tgbot
