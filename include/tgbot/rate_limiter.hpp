#pragma once

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <map>

namespace tgbot {

// 发送限速器：全局约 30 msg/s（Telegram 限制），单聊天 1 msg/s。
//
// 注意 Telegram 对**群**的额外限制是 20 msg/min，比 1 msg/s 更严格；
// 高频场景需要按 chat 类型再收紧（目前框架不知道 chat 类型，故未实现）。
//
// 实现为"最小发送间隔"调度：每个协程发送前先占住自己的时间槽、再等到该
// 时刻，由于所有协程跑在单个 io_context 线程上，无需加锁。
// 前提：BotClient 只能在同一个 io_context 的单线程里使用。
class RateLimiter {
public:
    boost::asio::awaitable<void> beforeSend(std::int64_t chatId);

    // 清理长时间未发送的 chat 槽位，避免按群无限增长。
    void pruneInactive(std::chrono::steady_clock::time_point cutoff) {
        for (auto it = nextChatSend_.begin(); it != nextChatSend_.end();) {
            if (it->second < cutoff) {
                it = nextChatSend_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    static constexpr auto kGlobalInterval = std::chrono::milliseconds{34}; // ~30/s
    static constexpr auto kChatInterval = std::chrono::seconds{1};

    std::chrono::steady_clock::time_point nextGlobalSend_{};
    std::map<std::int64_t, std::chrono::steady_clock::time_point> nextChatSend_;
};

} // namespace tgbot
