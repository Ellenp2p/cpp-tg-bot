#include <tgbot/long_poll.hpp>

#include "chat_dispatch_util.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace tgbot {

namespace net = boost::asio;
namespace fs = std::filesystem;

namespace {

// ---- offset 持久化 ----

std::int64_t loadOffset(const std::string& path) {
    std::ifstream in(path);
    std::int64_t v = 0;
    if (in >> v && v >= 0) {
        return v;
    }
    return 0;
}

// 原子写：写临时文件 → flush + fsync → rename 覆盖目标。
//
// 必须用 std::filesystem::rename：Windows CRT 的 std::rename 在目标文件已存在
// 时直接失败（返回 -1），POSIX 才是覆盖语义。用 std::rename 的后果是
// "第一次运行能写、之后永远不更新 offset"，而且会留下一堆 .tmp。
bool saveOffset(const std::string& path, std::int64_t offset) {
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        spdlog::warn("open offset tmp '{}' failed", tmp);
        return false;
    }
    const bool wrote = std::fprintf(f, "%lld\n",
                                    static_cast<long long>(offset)) > 0;
    std::fflush(f);
#if defined(_WIN32)
    ::_commit(::_fileno(f));
#else
    ::fsync(::fileno(f));
#endif
    std::fclose(f);
    if (!wrote) {
        spdlog::warn("write offset tmp '{}' failed", tmp);
        return false;
    }

    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        spdlog::warn("rename offset '{}' -> '{}' failed: {}", tmp, path,
                     ec.message());
        return false;
    }
    return true;
}

// 启动时清空积压：负 offset 拉取会忘记之前所有 updates，再确认掉最后一条
// 即可清空队列。返回新的 offset；探测失败时回落到持久化的值。
net::awaitable<std::int64_t> flushPendingUpdates(BotClient& bot,
                                                 const LongPollOptions& opts,
                                                 bool& webhookCleanupTried) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        nlohmann::json last;
        bool ok = false;
        bool conflict = false;
        std::string err;
        try {
            last =
                co_await bot.getUpdates(-1, /*timeout=*/0, opts.allowedUpdates);
            ok = true;
        } catch (const TelegramApiError& e) {
            err = e.what();
            conflict = (e.errorCode == 409);
            // 永久错误（错 token 之类）默认直接上抛退出；配置成"一直重试"时
            // 就当普通失败处理，回落到持久化 offset 后由主循环继续退避
            if (isPermanentError(e.errorCode) &&
                opts.permanentErrorPolicy ==
                    LongPollOptions::PermanentErrorPolicy::kExit) {
                throw;
            }
        } catch (const std::exception& e) {
            err = e.what();
        }

        if (ok) {
            if (!last.is_array() || last.empty()) {
                spdlog::info("no pending updates, start from offset 0");
                co_return 0;
            }
            const auto next =
                last.back().value("update_id", std::int64_t{0}) + 1;
            co_await bot.getUpdates(next, /*timeout=*/0, opts.allowedUpdates);
            saveOffset(opts.offsetFile, next);
            spdlog::info("dropped pending updates, start from offset {}", next);
            co_return next;
        }

        if (conflict && !webhookCleanupTried) {
            // 长轮询与 webhook 互斥：之前用 webhook 模式跑过的话，这里会一直
            // 409。先摘掉 webhook 再重试，否则用户只能看到"另一个进程在跑"
            // 这种误导性提示，然后永远起不来。
            webhookCleanupTried = true;
            spdlog::warn(
                "getUpdates 409 Conflict：检测到 webhook 仍在生效，"
                "正在 deleteWebhook 后重试");
            co_await bot.deleteWebhook(false);
            continue;
        }

        spdlog::warn("flush pending updates failed: {}; 回落到持久化 offset",
                     err);
        break;
    }
    co_return loadOffset(opts.offsetFile);
}

net::awaitable<void> longPollLoopImpl(BotClient& bot, Dispatcher& dispatcher,
                                      LongPollOptions opts,
                                      const std::atomic<bool>* shuttingDown) {
    const auto executor = co_await net::this_coro::executor;

    bool webhookCleanupTried = false;
    std::int64_t offset = 0;
    if (opts.dropPendingUpdates) {
        offset = co_await flushPendingUpdates(bot, opts, webhookCleanupTried);
    } else {
        offset = loadOffset(opts.offsetFile);
        if (offset > 0) {
            spdlog::info("resuming from persisted offset {}", offset);
        }
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
            updates = co_await bot.getUpdates(offset, opts.pollTimeoutSeconds,
                                              opts.allowedUpdates);
        } catch (const TelegramApiError& e) {
            error = e.what();
            errorCode = e.errorCode;
        } catch (const std::exception& e) {
            error = e.what();  // co_await 不允许出现在 catch 块内，先记录后处理
        }

        // 残留 webhook：自动清理一次，清完立刻重试（不进退避）
        if (errorCode == 409 && !webhookCleanupTried) {
            webhookCleanupTried = true;
            spdlog::warn(
                "getUpdates 409 Conflict：检测到 webhook 仍在生效，尝试 "
                "deleteWebhook(false) 后立即重试");
            try {
                co_await bot.deleteWebhook(false);
            } catch (const std::exception& e) {
                spdlog::error("deleteWebhook failed: {}", e.what());
            }
            continue;
        }

        // 永久错误（401/403/404）。默认退出：getUpdates 永久失败意味着一条
        // 消息都收不到，进程别假装还活着。注意这只管 getUpdates——业务 handler
        // 里的 403（被踢出群、没有删消息权限）只会被 Dispatcher 记日志。
        if (!error.empty()) {
            const bool permanent = (errorCode != 0) && isPermanentError(errorCode);
            if (permanent && opts.permanentErrorPolicy ==
                                 LongPollOptions::PermanentErrorPolicy::kExit) {
                spdlog::error(
                    "getUpdates 永久失败（{}）: {}；停止长轮询并退出（想一直重试"
                    "可设 LongPollOptions::permanentErrorPolicy = kRetryForever）",
                    errorCode, error);
                throw TelegramApiError{errorCode, error};
            }
            if (permanent) {
                spdlog::error("getUpdates 永久错误（{}）: {}；按配置继续退避重试",
                              errorCode, error);
            } else if (errorCode == 409) {
                spdlog::error(
                    "HTTP 409：webhook 已清理后仍然冲突，说明另一个进程正在用"
                    "同一个 token 跑长轮询，请停掉它或换 token；{}s 后重试",
                    backoff.count());
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
            saveOffset(opts.offsetFile, offset);
        }
    }
}

}  // namespace

net::awaitable<void> longPollLoop(BotClient& bot, Dispatcher& dispatcher,
                                  const std::atomic<bool>* shuttingDown) {
    co_await longPollLoopImpl(bot, dispatcher, LongPollOptions{}, shuttingDown);
}

net::awaitable<void> longPollLoop(BotClient& bot, Dispatcher& dispatcher,
                                  const LongPollOptions& options,
                                  const std::atomic<bool>* shuttingDown) {
    co_await longPollLoopImpl(bot, dispatcher, options, shuttingDown);
}

}  // namespace tgbot
