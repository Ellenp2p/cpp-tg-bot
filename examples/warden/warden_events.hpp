#pragma once
#include <memory>
#include <unordered_set>
#include "scheduler.hpp"
#include "store.hpp"
#include "warden_chat_sync.hpp"
#include "warden_menus.hpp"
#include "warden_texts.hpp"
#include "warden_types.hpp"
#include "warden_util.hpp"
namespace warden {
inline net::awaitable<void> handleMyChatMember(
    BotClient& bot,
    Store& store,
    DeleteScheduler& sched,
    GroupMenuPublisher& menus,
    std::int64_t botId,
    std::string_view botName,
    std::shared_ptr<std::unordered_set<std::int64_t>> knownChats,
    json update) {
    const auto chat = update.value("chat", json::object());
    const auto chatId = chat.value("id", std::int64_t{0});
    if (chatId == 0 || !isGroupType(chat.value("type", std::string{}))) {
        co_return; // 频道/私聊不在本机器人职责内
    }
    const auto newMember = update.value("new_chat_member", json::object());
    const auto oldMember = update.value("old_chat_member", json::object());
    const auto newStatus = newMember.value("status", std::string{"unknown"});
    const auto oldStatus = oldMember.value("status", std::string{"unknown"});
    const auto now = nowSec();

    spdlog::info("my_chat_member: chat={} ({}) {} -> {}",
                 chatId,
                 chat.value("title", std::string{}),
                 oldStatus,
                 newStatus);

    if (newStatus == "left" || newStatus == "kicked") {
        store.markLeft(chatId, now);
        sched.cancelChat(chatId);
        menus.forget(chatId);
        if (knownChats) {
            knownChats->erase(chatId);
        }
        store.audit(chatId, 0, "bot.left", newStatus);
        co_return;
    }

    store.ensureChat(
        chatId, chat.value("type", std::string{}), chat.value("title", std::string{}), now);
    const auto record = co_await refreshChat(bot, store, botId, chatId);
    co_await refreshAdmins(bot, store, chatId);
    if (record.has_value() && record->isAdmin()) {
        co_await menus.publish(bot, chatId);
    }

    const bool isNewMembership =
        oldStatus == "left" || oldStatus == "kicked" || oldStatus == "unknown";
    if (isNewMembership) {
        try {
            co_await bot.sendMessage(
                chatId,
                introText(botName) + (record.has_value() && record->isAdmin()
                                          ? ""
                                          : "\n\n⚠️ 我现在还不是管理员，请把我设为管理员并"
                                            "勾选「删除消息」。"));
        } catch (const std::exception& e) {
            spdlog::warn("send intro to {} failed: {}", chatId, e.what());
        }
    }
}

inline net::awaitable<void> handleMedia(
    BotClient& bot,
    Store& store,
    DeleteScheduler& sched,
    json msg,
    std::shared_ptr<std::unordered_set<std::int64_t>> knownChats) {
    const std::string kind = detectMediaKind(msg);
    if (kind.empty()) {
        co_return;
    }
    const auto chat = msg.value("chat", json::object());
    const auto type = chat.value("type", std::string{});
    if (!isGroupType(type)) {
        co_return;
    }
    const auto chatId = chat.value("id", std::int64_t{0});
    const auto messageId = msg.value("message_id", std::int64_t{0});
    const auto now = nowSec();
    const auto messageDate = msg.value("date", now);

    if (knownChats->size() > 10000) {
        knownChats->clear(); // 下次消息重新 ensureChat，代价可接受
    }
    if (knownChats->insert(chatId).second) {
        store.ensureChat(chatId, type, chat.value("title", std::string{}), now);
    }
    store.touch(chatId, now);

    // 置顶消息跳过（包括管理员发的）。排程前先查缓存；排程后才置顶的，
    // 由 pin 事件取消任务 + scheduler 删除前复查兜底。
    if (store.isPinned(chatId, messageId)) {
        spdlog::debug("skip pinned media: chat={} msg={}", chatId, messageId);
        co_return;
    }

    const auto settings = store.settings(chatId);
    if (!settings->enabled) {
        co_return;
    }
    int delay = kindDefaultDelay(kind);
    if (const auto it = settings->delays.find(kind); it != settings->delays.end()) {
        delay = it->second;
    }
    if (delay <= 0) {
        co_return;
    }

    // Telegram 只允许删除 48 小时内的消息；留 1 小时余量
    if (now - messageDate > 47 * 3600) {
        spdlog::debug(
            "skip too-old message: chat={} msg={} age={}s", chatId, messageId, now - messageDate);
        co_return;
    }

    auto due = messageDate + delay;
    if (due <= now) {
        due = now + 1; // 停机补收的旧消息：立刻删
    }
    sched.schedule(chatId, messageId, kind, due);
    spdlog::debug(
        "scheduled delete: chat={} msg={} kind={} in {}s", chatId, messageId, kind, due - now);
}

} // namespace warden
