#pragma once

#include <tgbot/bot_client.hpp>
#include <tgbot/dispatcher.hpp>

#include <atomic>
#include <cstddef>
#include <string>

namespace tgbot {

// webhook 模式配置。两种部署形态：
// - TLS 直连：tlsCert / tlsKey 都设置 → 进程内直接终结 HTTPS
//   （私有化/单二进制交付，与"纯静态二进制"目标一致）
// - 反代终结：不设证书 → 明文监听 host:port，由 Caddy/nginx 终结 TLS
//   后转发到本端口（公网部署常用，证书申请/续期交给反代）。
//   反代必须把 X-Telegram-Bot-Api-Secret-Token 头透传过来。
struct WebhookConfig {
    std::string publicUrl;   // Telegram 投递地址，如 https://bot.example.com/hook
    std::string secretToken; // 可选；非空则校验 X-Telegram-Bot-Api-Secret-Token
    // 监听地址。默认只监听回环：反代形态下明文端口不应该暴露在公网；
    // 需要直连或容器内网暴露时显式设成 "0.0.0.0"
    std::string host = "127.0.0.1";
    unsigned short port = 8080;
    std::string tlsCert;     // PEM 证书链（与 tlsKey 同时设置才启用 TLS）
    std::string tlsKey;      // PEM 私钥
    int maxConnections = 40; // Telegram 并发投递上限（官方默认 40）

    // 有界 update 队列 + per-chat lane + 固定 worker 协程
    std::size_t maxPendingUpdates = 2000;
    std::size_t maxPendingBytes = 32 * 1024 * 1024;
    int updateWorkers = 8;
    // 启动时清空积压 updates（"停机即完全停止"语义，与长轮询模式一致）
    bool dropPendingUpdates = true;
    // 订阅的 update 类型（JSON 数组字符串）。留空则沿用服务端上一次设置；
    // 需要 my_chat_member / chat_member 时显式填，否则可能被残留配置过滤掉
    std::string allowedUpdates;
    // 启动时执行 setWebhook 注册（默认开）。本地自测接收端时可关：
    // 不碰真实 bot 的 webhook 配置，直接向本地端口伪造 POST 即可
    bool registerOnStart = true;
};

// webhook 主循环（同一 io_context 协程内 co_await）：
//   1. 启动 HTTP/1.1 监听（TLS 可选）
//   2. setWebhook 注册投递地址（drop_pending_updates 清积压）
//   3. 循环接受连接 → 校验 method/path/secret token → 尝试压入有界队列
//      → 成功回 200；队列满回 503，让 Telegram 重试
//      → 按 per-chat lane + 固定 worker 协程处理（与长轮询同一套语义）
// Telegram 对非 2xx 响应会自动重试；shuttingDown 置位后回 503 且不再派发，
// 进程退出期间积压的 update 由下次启动的 drop_pending_updates 清掉。
boost::asio::awaitable<void> webhookLoop(BotClient& bot,
                                         Dispatcher& dispatcher,
                                         const WebhookConfig& config,
                                         const std::atomic<bool>* shuttingDown = nullptr);

} // namespace tgbot
