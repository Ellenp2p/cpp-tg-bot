#pragma once

#include <tgbot/bot_client.hpp>
#include <tgbot/dispatcher.hpp>

#include <atomic>
#include <cstddef>
#include <string>

namespace tgbot {

// 长轮询配置
struct LongPollOptions {
    // offset 持久化文件（相对 CWD）。务必指向可写目录——放只读目录时
    // 只会打警告，不影响收消息，但重启后会重复投递。
    std::string offsetFile = "tg_bot.offset";

    // true（默认）：启动时丢掉停机期间积压的 updates —— "停机即完全停止"语义，
    //   重启不补放旧消息；此时 offsetFile 只作为启动探测失败时的兜底。
    // false：从持久化 offset 续传，重启后补放未确认的消息（Telegram 只保留
    //   最近 24 小时的 update，所以最多补这么久）。
    // 两种语义都合理，但必须显式选一个，别让文档和代码各说各话。
    bool dropPendingUpdates = true;

    // getUpdates 长轮询超时（秒）：没有新消息时一个循环会阻塞这么久
    int pollTimeoutSeconds = 30;

    // 有界 update 队列 + per-chat lane + 固定 worker 协程
    std::size_t maxPendingUpdates = 2000;
    int updateWorkers = 8;

    // 订阅的 update 类型（JSON 数组字符串）。默认只有 message/callback_query；
    // 需要 edited_message / chat_member / inline_query / message_reaction 等
    // 必须显式改这里，框架不会自动加
    std::string allowedUpdates = kDefaultAllowedUpdates;

    // 401/403/404（token 失效 / 无权限 / 方法不存在）这类永久错误的处理方式。
    // 只影响长轮询里的 getUpdates：业务 handler 中的 403（被踢出群、没有删消息
    // 权限等）永远只记日志，不会因为一次失败就让进程退出。
    enum class PermanentErrorPolicy {
        // 默认：打一条 error 说明原因后退出进程（退出码 1），交给 systemd /
        // 容器的重启策略处理。理由：getUpdates 永久失败 = 一条消息都收不到，
        // 进程"活着但已经死了"，比直接退出更难被发现
        kExit,
        // 当成普通网络错误退避重试（1s→30s，永不放弃）。适合"偶发 401 希望
        // 自己扛过去"的场景，代价是 token 真失效时会变成刷日志的僵尸进程
        kRetryForever,
    };
    PermanentErrorPolicy permanentErrorPolicy = PermanentErrorPolicy::kExit;
};

// 长轮询主循环：update 入有界队列，按 chat lane 串行 + 固定 worker 协程处理。
// - 连接失败指数退避重试（1s→30s）
// - 409 会先尝试 deleteWebhook（残留 webhook 是 409 最常见的成因）再重试
// - 401/403/404 这类永久错误直接上抛，交由调用方退出进程（别再无限刷日志）
// shuttingDown 可选：置位后循环不再派发新 update（停机期间的消息由下次启动
// 的积压清空兜底）。
boost::asio::awaitable<void> longPollLoop(BotClient& bot,
                                          Dispatcher& dispatcher,
                                          const std::atomic<bool>* shuttingDown = nullptr);

// 带配置的版本
boost::asio::awaitable<void> longPollLoop(BotClient& bot,
                                          Dispatcher& dispatcher,
                                          const LongPollOptions& options,
                                          const std::atomic<bool>* shuttingDown = nullptr);

} // namespace tgbot
