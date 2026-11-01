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
//
// 注：SQLite 状态存储是可选后端（需要 sqlite3 依赖），不在本头里：
//   #include <tgbot/sqlite_state_store.hpp>

#include <tgbot/amount.hpp>
#include <tgbot/bot_client.hpp>
#include <tgbot/chain_health.hpp>
#include <tgbot/dispatcher.hpp>
#include <tgbot/job_queue.hpp>
#include <tgbot/long_poll.hpp>
#include <tgbot/lottery_panel.hpp>
#include <tgbot/panel.hpp>
#include <tgbot/red_packet_batch.hpp>
#include <tgbot/red_packet_ledger.hpp>
#include <tgbot/red_packet_panel.hpp>
#include <tgbot/red_packet_service.hpp>
#include <tgbot/reconciliation.hpp>
#include <tgbot/rich_text.hpp>
#include <tgbot/state_store.hpp>
#include <tgbot/stream_message.hpp>
#include <tgbot/util.hpp>
#include <tgbot/webhook.hpp>
