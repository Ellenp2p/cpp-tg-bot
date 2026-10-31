#pragma once

#include <tgbot/bot_client.hpp>
#include <tgbot/dispatcher.hpp>

#include <atomic>
#include <string>

namespace tgbot {

// webhook 模式配置。两种部署形态：
// - TLS 直连：tlsCert / tlsKey 都设置 → 进程内直接终结 HTTPS
//   （私有化/单二进制交付，与"纯静态二进制"目标一致）
// - 反代终结：不设证书 → 明文监听 host:port，由 Caddy/nginx 终结 TLS
//   后转发到本端口（公网部署常用，证书申请/续期交给反代）。
//   反代必须把 X-Telegram-Bot-Api-Secret-Token 头透传过来。
struct WebhookConfig {
    std::string publicUrl;    // Telegram 投递地址，如 https://bot.example.com/hook
    std::string secretToken;  // 可选；非空则校验 X-Telegram-Bot-Api-Secret-Token
    std::string host = "0.0.0.0";
    unsigned short port = 8080;
    std::string tlsCert;      // PEM 证书链（与 tlsKey 同时设置才启用 TLS）
    std::string tlsKey;       // PEM 私钥
    int maxConnections = 40;  // Telegram 并发投递上限（官方默认 40）
    // 启动时清空积压 updates（"停机即完全停止"语义，与长轮询模式一致）
    bool dropPendingUpdates = true;
    // 启动时执行 setWebhook 注册（默认开）。本地自测接收端时可关：
    // 不碰真实 bot 的 webhook 配置，直接向本地端口伪造 POST 即可
    bool registerOnStart = true;
};

// webhook 主循环（同一 io_context 协程内 co_await）：
//   1. 启动 HTTP/1.1 监听（TLS 可选）
//   2. setWebhook 注册投递地址（drop_pending_updates 清积压）
//   3. 循环接受连接 → 校验 method/path/secret token → 立即回 200
//      → 按 chat strand 异步派发（与长轮询同一套串行语义）
// Telegram 对非 2xx 响应会自动重试；shuttingDown 置位后回 503 且不再派发，
// 进程退出期间积压的 update 由下次启动的 drop_pending_updates 清掉。
boost::asio::awaitable<void> webhookLoop(
    BotClient& bot, Dispatcher& dispatcher, const WebhookConfig& config,
    const std::atomic<bool>* shuttingDown = nullptr);

}  // namespace tgbot
