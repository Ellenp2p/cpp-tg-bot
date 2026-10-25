#include "long_poll.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <exception>

namespace tb {

namespace net = boost::asio;

net::awaitable<void> longPollLoop(BotClient& bot, Dispatcher& dispatcher) {
    const auto executor = co_await net::this_coro::executor;
    std::int64_t offset = 0;
    // 指数退避：1s -> 2s -> 4s ... 封顶 30s，成功一次即重置
    auto backoff = std::chrono::seconds{1};

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
            net::co_spawn(executor,
                          dispatcher.dispatch(update, bot),
                          net::detached);
        }
    }
}

}  // namespace tb
