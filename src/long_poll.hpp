#pragma once

#include "bot_client.hpp"
#include "dispatcher.hpp"

namespace tb {

// 长轮询主循环：offset 持久化推进；每个 update co_spawn 独立协程处理。
// 连接失败记录日志并等待 3 秒后重试（M4 会换成指数退避）。
boost::asio::awaitable<void> longPollLoop(BotClient& bot,
                                          Dispatcher& dispatcher);

}  // namespace tb
