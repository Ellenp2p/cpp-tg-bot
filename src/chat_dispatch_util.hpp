#pragma once

// 有界 update 队列 + per-chat lane + 固定 worker 协程。
//
// 关键约束：
//   - 同一 chat 的 update 严格串行（lane.active 保证）；
//   - 不同 chat 由多个 worker 并行；
//   - 队列有上限，满了 dispatch() 返回 false，调用方必须背压；
//   - 队列按 chat lane 存，lane 空了立即 erase，不按群无限增长；
//   - 全部跑在同一个 io_context 单线程上，无锁。

#include <tgbot/bot_client.hpp>
#include <tgbot/dispatcher.hpp>
#include <tgbot/util.hpp>

#include <nlohmann/json.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <unordered_map>
#include <utility>

namespace tgbot {
namespace detail {

inline std::int64_t chatIdOf(const nlohmann::json& update) {
    const auto chatIdIn = [](const nlohmann::json& j) -> std::int64_t {
        auto c = j.find("chat");
        if (c == j.end() || !c->is_object()) {
            return 0;
        }
        return c->value("id", std::int64_t{0});
    };
    const auto messageChatId = [&](const nlohmann::json& j) -> std::int64_t {
        auto m = j.find("message");
        if (m == j.end() || !m->is_object()) {
            return 0;
        }
        return chatIdIn(*m);
    };

    auto cq = update.find("callback_query");
    if (cq != update.end() && cq->is_object()) {
        if (const auto id = messageChatId(*cq); id != 0) {
            return id;
        }
    }
    for (const char* key : {"my_chat_member", "chat_member"}) {
        auto it = update.find(key);
        if (it != update.end() && it->is_object()) {
            if (const auto id = chatIdIn(*it); id != 0) {
                return id;
            }
        }
    }
    return messageChatId(update);
}

class UpdateWorkers {
public:
    explicit UpdateWorkers(boost::asio::any_io_executor executor,
                           std::size_t maxPending = 2000,
                           int workers = 8)
        : state_(std::make_shared<State>(std::move(executor), maxPending)) {
        const auto count = std::max(1, workers);
        for (int i = 0; i < count; ++i) {
            boost::asio::co_spawn(
                state_->executor,
                [state = state_]() -> boost::asio::awaitable<void> { co_await workerLoop(state); },
                boost::asio::detached);
        }
    }

    ~UpdateWorkers() {
        // 停止接收新 update；worker 会把已入队的处理完再退出。
        state_->accepting = false;
    }

    UpdateWorkers(const UpdateWorkers&) = delete;
    UpdateWorkers& operator=(const UpdateWorkers&) = delete;

    // 返回 false = 队列已满，调用方必须背压（webhook 503 / 长轮询暂停）。
    bool dispatch(const nlohmann::json& update, BotClient& bot, Dispatcher& dispatcher) {
        if (!state_->accepting || state_->pending >= state_->maxPending) {
            return false;
        }
        const auto chatId = chatIdOf(update);
        auto& lane = state_->lanes[chatId];
        lane.pending.push_back(Job{update, &dispatcher, &bot});
        ++state_->pending;
        // 入队即计入 in-flight，保证 drain 不会在队列还有任务时提前退出。
        dispatcher.beginUpdate();
        if (!lane.active) {
            lane.active = true;
            state_->ready.push_back(chatId);
        }
        return true;
    }

    std::size_t pending() const { return state_->pending; }
    std::size_t maxPending() const { return state_->maxPending; }

private:
    struct Job {
        nlohmann::json update;
        Dispatcher* dispatcher;
        BotClient* bot;
    };

    struct Lane {
        std::deque<Job> pending;
        bool active = false;
    };

    struct State {
        State(boost::asio::any_io_executor ex, std::size_t maxPending)
            : executor(std::move(ex)), maxPending(maxPending) {}

        boost::asio::any_io_executor executor;
        std::unordered_map<std::int64_t, Lane> lanes;
        std::deque<std::int64_t> ready;
        std::size_t pending = 0;
        std::size_t maxPending = 2000;
        bool accepting = true;
    };

    static boost::asio::awaitable<void> workerLoop(std::shared_ptr<State> state) {
        for (;;) {
            if (!state->accepting && state->pending == 0 && state->ready.empty()) {
                co_return;
            }
            if (state->ready.empty()) {
                co_await tgbot::co_sleep(std::chrono::milliseconds{2});
                continue;
            }

            const auto chatId = state->ready.front();
            state->ready.pop_front();
            auto laneIt = state->lanes.find(chatId);
            if (laneIt == state->lanes.end() || laneIt->second.pending.empty()) {
                continue;
            }

            Job job = std::move(laneIt->second.pending.front());
            laneIt->second.pending.pop_front();
            --state->pending;

            try {
                co_await job.dispatcher->dispatch(std::move(job.update), *job.bot);
            } catch (const std::exception& e) {
                spdlog::error("update dispatch failed: {}", e.what());
            } catch (...) {
                spdlog::error("update dispatch failed: unknown exception");
            }
            job.dispatcher->endUpdate();

            // job 执行期间可能有新 update 入队；重新查，别持有引用跨 co_await。
            laneIt = state->lanes.find(chatId);
            if (laneIt == state->lanes.end()) {
                continue;
            }
            if (laneIt->second.pending.empty()) {
                state->lanes.erase(laneIt);
            } else {
                state->ready.push_back(chatId);
            }
        }
    }

    std::shared_ptr<State> state_;
};

} // namespace detail
} // namespace tgbot
