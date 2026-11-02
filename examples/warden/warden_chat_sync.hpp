#pragma once
#include "store.hpp"
#include "warden_types.hpp"
#include "warden_util.hpp"
namespace warden {
inline ChatRecord recordFromChat(const json& chat, const json& member, std::int64_t now) {
    ChatRecord r;
    r.chatId = chat.value("id", std::int64_t{0});
    r.type = chat.value("type", std::string{});
    r.title = chat.value("title", std::string{});
    r.username = chat.value("username", std::string{});
    r.botStatus = member.value("status", std::string{"unknown"});
    const bool creator = r.botStatus == "creator";
    r.canDelete = creator || member.value("can_delete_messages", false);
    r.canRestrict = creator || member.value("can_restrict_members", false);
    r.canPin = creator || member.value("can_pin_messages", false);
    r.canInvite = creator || member.value("can_invite_users", false);
    r.canManage = creator || member.value("can_manage_chat", false);
    // 新群默认开启；已有群的 enabled 不会被 saveChat 覆盖（见 store.cpp）。
    r.enabled = true;
    r.joinedAt = now;
    r.lastSeenAt = now;
    r.refreshedAt = now;
    r.leftAt = (r.botStatus == "left" || r.botStatus == "kicked") ? now : 0;
    return r;
}

net::awaitable<std::optional<ChatRecord>> refreshChat(BotClient& bot,
                                                      Store& store,
                                                      std::int64_t botId,
                                                      std::int64_t chatId) {
    try {
        const auto chat = co_await bot.getChat(chatId);
        const auto member = co_await bot.getChatMember(chatId, botId);
        const auto now = nowSec();
        // getChat 会带当前置顶消息；顺手登记，重启后也能保护"历史置顶"
        if (const auto pin = chat.find("pinned_message"); pin != chat.end() && pin->is_object()) {
            const auto pinnedId = pin->value("message_id", std::int64_t{0});
            if (pinnedId != 0) {
                store.markPinned(chatId, pinnedId, now);
            }
        }
        auto record = recordFromChat(chat, member, now);
        if (!record.active()) {
            store.markLeft(chatId, now);
        } else {
            store.saveChat(record);
        }
        co_return record;
    } catch (const std::exception& e) {
        spdlog::warn("refresh chat {} failed: {}", chatId, e.what());
        co_return std::nullopt;
    }
}

inline net::awaitable<void> refreshAdmins(BotClient& bot, Store& store, std::int64_t chatId) {
    try {
        const auto list = co_await bot.getChatAdministrators(chatId);
        std::vector<AdminRow> rows;
        for (const auto& m : list) {
            if (!m.is_object()) {
                continue;
            }
            const auto user = m.value("user", json::object());
            rows.push_back(AdminRow{user.value("id", std::int64_t{0}),
                                    m.value("status", std::string{}),
                                    displayName(user)});
        }
        store.replaceAdmins(chatId, rows, nowSec());
        spdlog::info("admin roster refreshed: chat={} count={}", chatId, rows.size());
    } catch (const std::exception& e) {
        spdlog::warn("getChatAdministrators {} failed: {}", chatId, e.what());
    }
}

} // namespace warden
