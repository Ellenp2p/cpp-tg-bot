#include <tgbot/long_poll.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <map>
#include <string>

namespace tgbot {

namespace net = boost::asio;

namespace {

// ---- offset 持久化：重启后从上次确认的位置继续 ----
// Telegram 只保留 24h 内的 update，重启最多重放窗口内未确认的，不会丢消息。

constexpr const char* kOffsetFile = "tg_bot.offset";

std::int64_t loadOffset() {
    std::ifstream in(kOffsetFile);
    std::int64_t v = 0;
    if (in >> v && v >= 0) {
        return v;
    }
    return 0;
}

void saveOffset(std::int64_t offset) {
    // 原子写：先写临时文件再 rename，中途被杀也不会留下半截
    const std::string tmp = std::string(kOffsetFile) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!(out << offset << '\n')) {
            return;
        }
    }
    std::rename(tmp.c_str(), kOffsetFile);
}

// ---- 按 chat 派发：strand 保证同一聊天的 update 严格串行处理 ----

// 从 update 中提取聊天 id；取不到（无 chat 的特殊 update）归入 0 号 strand，
// 这些 update 之间也保持串行
std::int64_t chatIdOf(const nlohmann::json& update) {
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

}  // namespace

net::awaitable<void> longPollLoop(BotClient& bot, Dispatcher& dispatcher) {
    const auto executor = co_await net::this_coro::executor;
    std::int64_t offset = loadOffset();
    if (offset > 0) {
        spdlog::info("resuming from persisted offset {}", offset);
    }
    // 指数退避：1s -> 2s -> 4s ... 封顶 30s，成功一次即重置
    auto backoff = std::chrono::seconds{1};

    // 每个 chat 一个 strand：同一聊天的 update 串行处理（handler 执行与
    // 挂起期间都不会被同聊天的新 update 打断），不同聊天之间仍然并发。
    // 所有 strand 跑在同一个 io_context 线程上，无锁。
    // 注意：strand 表按见过的 chat 持续增长；用户量级大时需加淘汰策略。
    using Strand = net::strand<net::any_io_executor>;
    std::map<std::int64_t, Strand> chatStrands;

    while (true) {
        nlohmann::json updates;
        std::string error;
        long errorCode = 0;
        try {
            updates = co_await bot.getUpdates(offset, /*timeout=*/30);
        } catch (const TelegramApiError& e) {
            error = e.what();
            errorCode = e.errorCode;
        } catch (const std::exception& e) {
            error = e.what();  // co_await 不允许出现在 catch 块内，先记录再等
        }
        if (!error.empty()) {
            if (errorCode == 409) {
                spdlog::error(
                    "HTTP 409: 另一个进程正在用同一个 token 跑长轮询，"
                    "请停掉它或换 token；{}s 后重试", backoff.count());
            } else {
                spdlog::error("getUpdates failed: {}; retry in {}s", error,
                              backoff.count());
            }
            net::steady_timer timer(executor, backoff);
            co_await timer.async_wait(net::use_awaitable);
            backoff = std::min(backoff * 2, std::chrono::seconds{30});
            continue;
        }
        backoff = std::chrono::seconds{1};

        for (const auto& update : updates) {
            offset = std::max(offset,
                              update.value("update_id", std::int64_t{0}) + 1);
            const auto chatId = chatIdOf(update);
            auto& strand =
                chatStrands.try_emplace(chatId, executor).first->second;
            net::co_spawn(strand, dispatcher.dispatch(update, bot),
                          net::detached);
        }
        if (!updates.empty()) {
            saveOffset(offset);
        }
    }
}

}  // namespace tgbot
