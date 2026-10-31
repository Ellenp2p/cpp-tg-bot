#pragma once

// 内部共享（src/ 各实现文件复用，不对外安装）：
// 按 chat 的 strand 派发工具。同一聊天的 update 严格串行处理，
// 不同聊天之间并发；全部跑在同一个 io_context 单线程上，无锁。
//
// 注意：strand 表按见过的 chat 持续增长；用户量级大时需加淘汰策略。

#include <tgbot/bot_client.hpp>
#include <tgbot/dispatcher.hpp>

#include <nlohmann/json.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/strand.hpp>

#include <cstdint>
#include <map>

namespace tgbot {
namespace detail {

// 从 update 中提取聊天 id；取不到（无 chat 的特殊 update）归入 0 号
// strand，这些 update 之间也保持串行
inline std::int64_t chatIdOf(const nlohmann::json& update) {
    const auto chatIdIn = [](const nlohmann::json& j) -> std::int64_t {
        auto m = j.find("message");
        if (m == j.end() || !m->is_object()) {
            return 0;
        }
        auto c = m->find("chat");
        if (c == m->end() || !c->is_object()) {
            return 0;
        }
        return c->value("id", std::int64_t{0});
    };
    auto cq = update.find("callback_query");
    if (cq != update.end() && cq->is_object()) {
        if (const auto id = chatIdIn(*cq); id != 0) {
            return id;
        }
    }
    return chatIdIn(update);
}

// 每个 chat 一个 strand：spawn 到对应 strand 串行执行 Dispatcher::dispatch
class ChatStrands {
public:
    explicit ChatStrands(boost::asio::any_io_executor executor)
        : executor_(std::move(executor)) {}

    void dispatch(const nlohmann::json& update, BotClient& bot,
                  Dispatcher& dispatcher) {
        const auto id = chatIdOf(update);
        auto& strand = strands_.try_emplace(id, executor_).first->second;
        boost::asio::co_spawn(
            strand, dispatcher.dispatch(update, bot),
            boost::asio::detached);
    }

private:
    boost::asio::any_io_executor executor_;
    std::map<std::int64_t,
             boost::asio::strand<boost::asio::any_io_executor>>
        strands_;
};

}  // namespace detail
}  // namespace tgbot
