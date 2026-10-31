#pragma once

// tgbot 框架总入口：一个头文件包含全部公共 API
//
//   #include <tgbot/tg_bot.hpp>
//
// 核心组件：
//   BotClient    Bot API 封装（sendMessage / deleteMessage / getChatMember ...）
//   Dispatcher   事件注册与分发（onCommand / onAnyMessage / onCallbackQuery）
//   longPollLoop 长轮询主循环（按聊天 strand 串行、offset 持久化、断线退避）
//   webhookLoop  webhook 模式（Beast HTTP/1.1 服务端，TLS 可选，secret 校验，
//                与长轮询共用 Dispatcher 和按 chat strand 串行语义）
//   co_sleep     协程睡眠工具

#include <tgbot/bot_client.hpp>
#include <tgbot/dispatcher.hpp>
#include <tgbot/long_poll.hpp>
#include <tgbot/sqlite_state_store.hpp>
#include <tgbot/state_store.hpp>
#include <tgbot/util.hpp>
#include <tgbot/webhook.hpp>
