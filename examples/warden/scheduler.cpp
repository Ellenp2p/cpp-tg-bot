#include "scheduler.hpp"

#include "delete_result.hpp"

#include <tgbot/util.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <string>
#include <utility>

namespace warden {

namespace net = boost::asio;

namespace {

std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

DeleteScheduler::DeleteScheduler(tgbot::BotClient& bot, Store& store, Options options)
    : bot_(bot), store_(store), options_(std::move(options)) {}

void DeleteScheduler::start(const net::any_io_executor& executor) {
    auto self = shared_from_this();
    net::co_spawn(
        executor,
        [self]() -> net::awaitable<void> { co_await self->bootstrapLoop(); },
        net::detached);
}

net::awaitable<void> DeleteScheduler::bootstrapLoop() {
    // 关键顺序：先把"停机期间被置顶"的消息补登记，再允许 worker 删除。
    // 否则重启后逾期任务会立刻执行，可能在 getChat 补扫之前把置顶消息删掉。
    if (options_.pinBackfill) {
        const auto chatIds = store_.chatsWithPendingDeletions();
        if (!chatIds.empty()) {
            spdlog::info(
                "pin backfill: checking {} chat(s) with pending "
                "deletions",
                chatIds.size());
        }
        for (const auto chatId : chatIds) {
            try {
                const auto chat = co_await bot_.getChat(chatId);
                if (const auto pin = chat.find("pinned_message");
                    pin != chat.end() && pin->is_object()) {
                    const auto pinnedId = pin->value("message_id", std::int64_t{0});
                    if (pinnedId != 0) {
                        store_.markPinned(chatId, pinnedId, nowSeconds());
                        store_.cancelMessageDeletion(chatId, pinnedId);
                        spdlog::info("pin backfill protected: chat={} msg={}", chatId, pinnedId);
                    }
                }
            } catch (const std::exception& e) {
                // 群没了 / 机器人被踢：跳过，后面的删除任务会按失败处理
                spdlog::warn("pin backfill getChat failed: chat={}: {}", chatId, e.what());
            }
            if (options_.pinBackfillIntervalMs > 0) {
                co_await tgbot::co_sleep(std::chrono::milliseconds{options_.pinBackfillIntervalMs});
            }
        }
    }

    const auto executor = co_await net::this_coro::executor;
    auto self = shared_from_this();
    for (int i = 0; i < options_.workers; ++i) {
        net::co_spawn(
            executor,
            [self]() -> net::awaitable<void> { co_await self->workerLoop(); },
            net::detached);
    }
    net::co_spawn(
        executor,
        [self]() -> net::awaitable<void> { co_await self->janitorLoop(); },
        net::detached);
    spdlog::info("delete scheduler started: workers={}, global={}ms, chat={}ms",
                 options_.workers,
                 options_.globalIntervalMs,
                 options_.perChatIntervalMs);
}

void DeleteScheduler::schedule(std::int64_t chatId,
                               std::int64_t messageId,
                               std::string kind,
                               std::int64_t dueAt,
                               std::int64_t targetUserId) {
    store_.addDeletion(chatId, messageId, std::move(kind), dueAt, nowSeconds(), targetUserId);
}

void DeleteScheduler::cancelChat(std::int64_t chatId) {
    store_.cancelChatDeletions(chatId);
}

void DeleteScheduler::cancelKind(std::int64_t chatId, std::string_view kind) {
    store_.cancelKindDeletions(chatId, kind);
}

void DeleteScheduler::cancelMessage(std::int64_t chatId, std::int64_t messageId) {
    store_.cancelMessageDeletion(chatId, messageId);
}

net::awaitable<void> DeleteScheduler::workerLoop() {
    // 单个 worker 同一时刻只处理一条：并发度靠 workers 数量控制，
    // 这样不会出现"一次拉起几千个协程"的尖峰。
    for (;;) {
        const auto job = store_.claimDeletion(nowSeconds());
        if (!job.has_value()) {
            co_await tgbot::co_sleep(std::chrono::milliseconds{200});
            continue;
        }
        try {
            co_await deleteOne(*job);
        } catch (const std::exception& e) {
            // deleteOne 自己已经分类处理；这里只是最后一道保险，
            // 避免一条坏任务把 worker 整个带走。
            spdlog::error(
                "delete worker crashed on {}/{}: {}", job->chatId, job->messageId, e.what());
            store_.retryDeletion(job->chatId, job->messageId, nowSeconds() + 30, e.what());
        }
    }
}

net::awaitable<void> DeleteScheduler::deleteOne(DeletionJob job) {
    // 置顶消息永不自动删除。排程时已跳过，但可能排完之后才被管理员置顶；
    // 这里再查一次，避免"置顶保护"被竞态绕过。
    if (job.kind != "ephemeral" && job.kind != "bot_reply" &&
        store_.isPinned(job.chatId, job.messageId)) {
        store_.finishDeletion(job.chatId, job.messageId, true, "pinned");
        spdlog::info("skip pinned message: chat={} msg={}", job.chatId, job.messageId);
        co_return;
    }

    co_await pace(job.chatId);

    // pace 会睡到节流槽位；睡眠期间可能刚被置顶，醒来再查一次。
    // ephemeral 消息不是群消息，没有置顶概念。
    if (job.kind != "ephemeral" && job.kind != "bot_reply" &&
        store_.isPinned(job.chatId, job.messageId)) {
        store_.finishDeletion(job.chatId, job.messageId, true, "pinned");
        spdlog::info("skip pinned message: chat={} msg={}", job.chatId, job.messageId);
        co_return;
    }

    bool finished = false;
    bool failed = false;
    std::string error;

    try {
        if (job.kind == "ephemeral") {
            const auto ephemeralId = job.messageId < 0 ? -job.messageId : job.messageId;
            co_await bot_.deleteEphemeralMessage(job.chatId, job.targetUserId, ephemeralId);
        } else {
            co_await bot_.apiCall("deleteMessage",
                                  {{"chat_id", std::to_string(job.chatId)},
                                   {"message_id", std::to_string(job.messageId)}},
                                  tgbot::RetryPolicy::kNoTransportRetry);
        }
        finished = true;
    } catch (const tgbot::TelegramApiError& e) {
        error = e.what();
        if (e.errorCode == 400 && deleteErrorMeansGone(e.what())) {
            // 用户/管理员已经删了，或超过 48h 删除窗口：任务完成
            finished = true;
            spdlog::debug("message already gone: chat={} msg={}", job.chatId, job.messageId);
        } else if (e.errorCode == 400 || e.errorCode == 403) {
            failed = true;
        }
        // 429 / 5xx：走下面的退避重试
    } catch (const std::exception& e) {
        error = e.what();
    }

    if (finished) {
        store_.finishDeletion(job.chatId, job.messageId, true, error);
        spdlog::debug("deleted {}: chat={} msg={}", job.kind, job.chatId, job.messageId);
        co_return;
    }

    if (failed) {
        store_.finishDeletion(job.chatId, job.messageId, false, error);
        spdlog::warn("delete permanently failed: chat={} msg={} kind={}: {}",
                     job.chatId,
                     job.messageId,
                     job.kind,
                     error);
        // ephemeral 回复删不掉只是界面残留，不值得半夜叫 owner
        if (job.kind != "ephemeral" && job.kind != "bot_reply" && job.kind != "command") {
            const auto now = nowSeconds();
            auto& last = lastAlertAt_[job.chatId];
            if (now - last > 3600) {
                last = now;
                co_await alert("⚠️ Warden 删除失败（群 " + std::to_string(job.chatId) + "）：" +
                               error + "\n请检查我是否仍有「删除消息」管理员权限。");
            }
        }
        co_return;
    }

    // 瞬态失败：指数退避，最多 maxAttempts 次
    if (job.attempts + 1 >= options_.maxAttempts) {
        store_.finishDeletion(job.chatId, job.messageId, false, error);
        spdlog::warn(
            "delete gave up after {} attempts: chat={} msg={} "
            "kind={}: {}",
            job.attempts + 1,
            job.chatId,
            job.messageId,
            job.kind,
            error);
        if (job.kind != "ephemeral" && job.kind != "bot_reply" && job.kind != "command") {
            const auto now = nowSeconds();
            auto& last = lastAlertAt_[job.chatId];
            if (now - last > 3600) {
                last = now;
                co_await alert("⚠️ Warden 连续删除失败（群 " + std::to_string(job.chatId) + "）：" +
                               error);
            }
        }
        co_return;
    }

    const auto backoff = std::min<std::int64_t>(300, std::int64_t{5} * (job.attempts + 1));
    store_.retryDeletion(job.chatId, job.messageId, nowSeconds() + backoff, error);
    spdlog::warn(
        "delete retry in {}s: chat={} msg={}: {}", backoff, job.chatId, job.messageId, error);
}

net::awaitable<void> DeleteScheduler::pace(std::int64_t chatId) {
    const auto now = std::chrono::steady_clock::now();
    auto when = now;
    if (nextGlobal_ > when) {
        when = nextGlobal_;
    }
    if (const auto it = nextChat_.find(chatId); it != nextChat_.end()) {
        if (it->second > when) {
            when = it->second;
        }
    }
    nextGlobal_ = when + std::chrono::milliseconds{options_.globalIntervalMs};
    nextChat_[chatId] = when + std::chrono::milliseconds{options_.perChatIntervalMs};
    if (when > now) {
        co_await tgbot::co_sleep(when - now);
    }
}

net::awaitable<void> DeleteScheduler::janitorLoop() {
    for (;;) {
        // 先清一次再睡，避免重启后旧数据还要等 6 小时
        const auto before = nowSeconds() - 3 * 86400;
        const auto historyBefore = nowSeconds() - 10 * 86400;
        try {
            store_.purgeFinishedDeletions(before);
            store_.purgeAuditLog(historyBefore);
            store_.purgeOldPinnedMessages(historyBefore);
            store_.maintenance(nowSeconds());
            bot_.pruneRateLimiter();

            // 进程级按群节流表：长时间没活动的群直接丢掉，避免只增不减。
            const auto steadyCutoff = std::chrono::steady_clock::now() - std::chrono::hours{24};
            for (auto it = nextChat_.begin(); it != nextChat_.end();) {
                if (it->second < steadyCutoff) {
                    it = nextChat_.erase(it);
                } else {
                    ++it;
                }
            }
            const auto alertCutoff = nowSeconds() - 7 * 86400;
            for (auto it = lastAlertAt_.begin(); it != lastAlertAt_.end();) {
                if (it->second < alertCutoff) {
                    it = lastAlertAt_.erase(it);
                } else {
                    ++it;
                }
            }

            spdlog::info("delete janitor: pending={}, failed={}",
                         store_.pendingDeletions(),
                         store_.failedDeletions());
        } catch (const std::exception& e) {
            spdlog::warn("delete janitor failed: {}", e.what());
        }
        co_await tgbot::co_sleep(std::chrono::hours{6});
    }
}

net::awaitable<void> DeleteScheduler::alert(std::string text) {
    if (!options_.alert) {
        co_return;
    }
    try {
        co_await options_.alert(std::move(text));
    } catch (const std::exception& e) {
        spdlog::warn("alert owner failed: {}", e.what());
    }
}

} // namespace warden
