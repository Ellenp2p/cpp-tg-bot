#pragma once
#include <spdlog/spdlog.h>
#include "scheduler.hpp"
#include "warden_types.hpp"
#include "warden_util.hpp"
namespace warden {
inline int gReplyTtlSec = 20;
inline void setReplyTtlSec(int seconds) {
    gReplyTtlSec = seconds;
}
inline net::awaitable<void> replyEphemeral(BotClient& bot,
                                           DeleteScheduler& sched,
                                           const json& msg,
                                           std::string text,
                                           json markup = nullptr,
                                           tgbot::ParseMode parseMode = tgbot::ParseMode::kNone,
                                           bool replyToTrigger = false) {
    const auto chatId = chatIdOf(msg);
    if (chatId == 0) {
        co_return;
    }
    const auto triggerId = replyToTrigger ? msg.value("message_id", std::int64_t{0}) : 0;
    tgbot::SendOptions options;
    options.parseMode = parseMode;
    options.replyMarkup = markup;
    options.replyToMessageId = triggerId;
    if (chatTypeOf(msg) == "private") {
        co_await bot.sendMessage(chatId, text, options);
        co_return;
    }

    const auto from = msg.value("from", json::object());
    const auto userId = from.value("id", std::int64_t{0});
    // 匿名管理员（GroupAnonymousBot）没有可投递的真实用户，无法私聊
    static constexpr std::int64_t kAnonymousBotId = 1087968824;
    const bool anonymous = from.value("is_bot", false) || userId == kAnonymousBotId;
    if (!anonymous) {
        tgbot::EphemeralMessageParameters eph;
        eph.receiverUserId = userId;
        try {
            const auto sentMsg =
                co_await bot.sendEphemeralMessage(chatId, eph, text, markup, parseMode, triggerId);
            const auto ephemeralId = sentMsg.value("ephemeral_message_id", std::int64_t{0});
            if (ephemeralId != 0 && gReplyTtlSec > 0) {
                // ephemeral 消息不能用 deleteMessage，必须用
                // deleteEphemeralMessage(chat_id, receiver_user_id, id)。
                // message_id 存负数，避免和真实群消息 ID 在 deletions 主键里撞车。
                try {
                    sched.schedule(
                        chatId, -ephemeralId, "ephemeral", nowSec() + gReplyTtlSec, userId);
                } catch (const std::exception& e) {
                    spdlog::warn(
                        "schedule ephemeral delete failed: "
                        "chat={} eph={}: {}",
                        chatId,
                        ephemeralId,
                        e.what());
                }
            }
            co_return;
        } catch (const std::exception& e) {
            spdlog::info(
                "ephemeral reply unavailable (chat={}), fallback to "
                "public: {}",
                chatId,
                e.what());
        }
    }

    // 群内普通回复兜底：所有人可见，但会按 reply TTL 删除。
    try {
        const auto sent = co_await bot.sendMessage(chatId, text, options);
        const auto replyId = sent.value("message_id", std::int64_t{0});
        if (replyId != 0 && gReplyTtlSec > 0) {
            try {
                sched.schedule(chatId, replyId, "bot_reply", nowSec() + gReplyTtlSec);
            } catch (const std::exception& e) {
                spdlog::warn(
                    "schedule public reply delete failed: "
                    "chat={} msg={}: {}",
                    chatId,
                    replyId,
                    e.what());
            }
        }
        co_return;
    } catch (const std::exception& e) {
        spdlog::debug("public fallback failed (chat={}): {}", chatId, e.what());
    }

    // 最后再尝试私聊；群消息的 reply_parameters 不能带到私聊。
    try {
        auto privateOptions = options;
        privateOptions.replyToMessageId = 0;
        const auto sent = co_await bot.sendMessage(userId, text, privateOptions);
        const auto replyId = sent.value("message_id", std::int64_t{0});
        if (replyId != 0 && gReplyTtlSec > 0) {
            try {
                sched.schedule(userId, replyId, "bot_reply", nowSec() + gReplyTtlSec);
            } catch (const std::exception& e) {
                spdlog::warn(
                    "schedule private reply delete failed: "
                    "user={} msg={}: {}",
                    userId,
                    replyId,
                    e.what());
            }
        }
        co_return;
    } catch (const std::exception& e) {
        spdlog::debug("private fallback failed (user={}): {}", userId, e.what());
    }

    spdlog::warn(
        "group reply dropped: ephemeral, public and private fallback failed "
        "(chat={} user={})",
        chatId,
        userId);
}

// 群里的 `/命令` 立即删除。ephemeral command 的 message_id=0，bot 删不掉。
inline net::awaitable<void> sweepGroupCommand(BotClient& bot, json msg) {
    if (!isGroupType(chatTypeOf(msg))) {
        co_return;
    }
    const auto chatId = chatIdOf(msg);
    const auto messageId = msg.value("message_id", std::int64_t{0});
    const auto ephemeralId = msg.value("ephemeral_message_id", std::int64_t{0});
    spdlog::info("command sweep: chat={} msg={} eph={}", chatId, messageId, ephemeralId);
    if (chatId == 0 || messageId == 0) {
        spdlog::warn(
            "command sweep skipped: chat={} msg=0 eph={} "
            "(ephemeral command cannot be deleted by bot)",
            chatId,
            ephemeralId);
        co_return;
    }
    try {
        co_await bot.apiCall(
            "deleteMessage",
            {{"chat_id", std::to_string(chatId)}, {"message_id", std::to_string(messageId)}},
            tgbot::RetryPolicy::kNoTransportRetry);
        spdlog::info("command deleted: chat={} msg={}", chatId, messageId);
    } catch (const std::exception& e) {
        spdlog::warn("command delete failed: chat={} msg={}: {}", chatId, messageId, e.what());
    }
}

} // namespace warden
