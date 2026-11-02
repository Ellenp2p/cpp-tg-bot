#pragma once

// 定时删除框架：把"某条消息在某个时刻删除"变成一条落库任务，
// 由固定数量的协程并发消费。核心保证：
//   - 先落库再计时：进程崩溃/重启后，未完成的任务还在，逾期任务立即执行
//   - (chat_id, message_id) 唯一：重复排程/重复投递不会删两次
//   - 删除失败分类：已删除/太老 → 视为完成；无权限 → 失败并告警；
//     网络/5xx → 退避重试，超过上限转失败
//   - 删除走自己的节流器，不复用 sendMessage 的"单群 1 条/秒"，
//     否则一个活跃群里的贴纸会排到几分钟后

#include "store.hpp"

#include <tgbot/bot_client.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace warden {

class DeleteScheduler : public std::enable_shared_from_this<DeleteScheduler> {
public:
    struct Options {
        int workers = 4;
        int globalIntervalMs = 34;   // 全局 ~30 次/秒
        int perChatIntervalMs = 250; // 单群 ~4 次/秒
        int maxAttempts = 5;
        // 启动时先对"有待删任务的群"各调一次 getChat，补登记当前置顶消息，
        // 防止机器人在停机期间被置顶的媒体重启后被误删。
        bool pinBackfill = true;
        int pinBackfillIntervalMs = 50;
        // 永久失败时通知 owner（可为空）。实现方自己负责容错。
        std::function<boost::asio::awaitable<void>(std::string)> alert;
    };

    DeleteScheduler(tgbot::BotClient& bot, Store& store, Options options);

    // 启动 worker + 清理协程。必须在 io_context.run() 之前调用。
    void start(const boost::asio::any_io_executor& executor);

    // 到期时间由调用方按消息的 date（Telegram 服务器时间）计算，
    // 这样停机补收的旧消息如果已经超时，会立刻删除而不是再等一轮。
    void schedule(std::int64_t chatId,
                  std::int64_t messageId,
                  std::string kind,
                  std::int64_t dueAt,
                  std::int64_t targetUserId = 0);
    void cancelChat(std::int64_t chatId);
    void cancelKind(std::int64_t chatId, std::string_view kind);
    void cancelMessage(std::int64_t chatId, std::int64_t messageId);

private:
    // 先补扫置顶，再启动 worker；补扫期间队列不动，避免漏掉历史置顶
    boost::asio::awaitable<void> bootstrapLoop();
    boost::asio::awaitable<void> workerLoop();
    boost::asio::awaitable<void> deleteOne(DeletionJob job);
    boost::asio::awaitable<void> pace(std::int64_t chatId);
    boost::asio::awaitable<void> janitorLoop();
    boost::asio::awaitable<void> alert(std::string text);

    tgbot::BotClient& bot_;
    Store& store_;
    Options options_;

    std::chrono::steady_clock::time_point nextGlobal_{};
    std::map<std::int64_t, std::chrono::steady_clock::time_point> nextChat_;
    std::map<std::int64_t, std::int64_t> lastAlertAt_;
};

} // namespace warden
