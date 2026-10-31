#include <tgbot/long_poll.hpp>

#include "chat_dispatch_util.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
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

}  // namespace

net::awaitable<void> longPollLoop(BotClient& bot, Dispatcher& dispatcher,
                                  const std::atomic<bool>* shuttingDown) {
    const auto executor = co_await net::this_coro::executor;
    std::int64_t offset = loadOffset();
    if (offset > 0) {
        spdlog::info("resuming from persisted offset {}", offset);
    }

    // 启动时丢弃停机期间积压的所有 updates（"停机即完全停止"语义）：
    // 负 offset 拉取会忘记之前所有 updates，再确认掉最后一条即可清空队列。
    // 重启后不补放旧消息；要求不丢消息的部署应改用 webhook。
    try {
        const auto last = co_await bot.getUpdates(-1, /*timeout=*/0);
        if (!last.empty()) {
            offset = last.back().value("update_id", std::int64_t{0}) + 1;
            co_await bot.getUpdates(offset, /*timeout=*/0);  // 确认最后一条
            saveOffset(offset);
            spdlog::info("dropped pending updates, start from offset {}",
                         offset);
        }
    } catch (const std::exception& e) {
        spdlog::warn("flush pending updates failed: {}; use persisted offset",
                     e.what());
    }
    // 指数退避：1s -> 2s -> 4s ... 封顶 30s，成功一次即重置
    auto backoff = std::chrono::seconds{1};

    // 每个 chat 一个 strand：同一聊天的 update 串行处理（handler 执行与
    // 挂起期间都不会被同聊天的新 update 打断），不同聊天之间仍然并发。
    // 所有 strand 跑在同一个 io_context 线程上，无锁。
    // 与 webhook 模式共用 detail::ChatStrands（src/chat_dispatch_util.hpp）。
    detail::ChatStrands chatStrands(executor);

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

        // 停机中：忽略所有新到的 update，不再派发（不推进 offset 文件，
        // 也不触发任何业务副作用；下次启动的积压清空会把它们丢掉）
        if (shuttingDown != nullptr && shuttingDown->load()) {
            spdlog::info("shutting down, ignoring {} update(s)", updates.size());
            continue;
        }

        for (const auto& update : updates) {
            offset = std::max(offset,
                              update.value("update_id", std::int64_t{0}) + 1);
            chatStrands.dispatch(update, bot, dispatcher);
        }
        if (!updates.empty()) {
            saveOffset(offset);
        }
    }
}

}  // namespace tgbot
