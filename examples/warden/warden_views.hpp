#pragma once
#include <algorithm>
#include <chrono>
#include "media.hpp"
#include "store.hpp"
#include "warden_types.hpp"
#include "warden_util.hpp"
namespace warden {
inline constexpr int kPageSize = 6;
inline int effectiveDelay(Store& store, std::int64_t chatId, std::string_view kind) {
    const auto settings = store.settings(chatId);
    if (const auto it = settings->delays.find(kind); it != settings->delays.end()) {
        return it->second;
    }
    return kindDefaultDelay(kind);
}

inline std::string settingsSummary(Store& store, std::int64_t chatId) {
    std::string s;
    for (const auto* kind : kMediaKinds) {
        s += "• ";
        s += std::string{kindLabel(kind)};
        s += "：";
        s += formatDelay(effectiveDelay(store, chatId, kind));
        s += "\n";
    }
    return s;
}

// 拥有者面板里"点一下换一档"的档位。0 = 关闭。
inline int nextPreset(int current) {
    static constexpr int kPresets[] = {0, 30, 60, 300, 900, 3600, 21600, 86400};
    constexpr std::size_t n = std::size(kPresets);
    for (std::size_t i = 0; i < n; ++i) {
        if (current == kPresets[i]) {
            return kPresets[(i + 1) % n];
        }
    }
    return 300; // 非标准值（手改过库）→ 回到 5 分钟
}

inline bool isKnownKind(std::string_view kind) {
    return std::find_if(kMediaKinds.begin(), kMediaKinds.end(), [&](const char* k) {
               return kind == k;
           }) != kMediaKinds.end();
}

// ---- 拥有者后台 ----

inline bool isOwner(const Config& cfg, std::int64_t userId) {
    return isOwnerId(cfg.owners, userId);
}

// ---- /autodel 参数解析（兼容英文命令和中文自然语言）----

inline std::string dashboardText(Store& store, std::chrono::steady_clock::time_point start) {
    const auto uptime =
        std::chrono::duration_cast<std::chrono::minutes>(std::chrono::steady_clock::now() - start)
            .count();
    return "🛡 Warden 控制台\n"
           "\n"
           "群组：" +
           std::to_string(store.countChats(true)) + "（启用自动清理 " +
           std::to_string(store.countEnabledChats()) +
           "）\n"
           "缺「删除消息」权限：" +
           std::to_string(store.countWithoutDeleteRights()) +
           "\n"
           "待删除任务：" +
           std::to_string(store.pendingDeletions()) + "（失败 " +
           std::to_string(store.failedDeletions()) +
           "）\n"
           "运行时长：" +
           std::to_string(uptime) +
           " 分钟\n"
           "\n"
           "你是机器人拥有者：可在任意群使用管理命令，也可以在这里直接改"
           "任意群的清理时限；群管理员只能管理自己的群。\n"
           "选择「群列表」查看每个群的权限和设置。";
}

inline json dashboardKeyboard() {
    json rows = json::array();
    rows.push_back(json::array({json{{"text", "📋 群列表"}, {"callback_data", "wd:list:0"}}}));
    rows.push_back(json::array({json{{"text", "🔄 刷新统计"}, {"callback_data", "wd:home"}}}));
    rows.push_back(json::array({json{{"text", "❓ 帮助"}, {"callback_data", "wd:help"}}}));
    return {{"inline_keyboard", rows}};
}

inline std::string listText(Store& store, int page) {
    const int total = store.countChats(true);
    const int pages = std::max(1, (total + kPageSize - 1) / kPageSize);
    page = std::clamp(page, 0, pages - 1);
    return "📋 群列表（" + std::to_string(page + 1) + "/" + std::to_string(pages) + "）\n共 " +
           std::to_string(total) +
           " 个群，点群名查看详情。\n"
           "\n🟢 已启用  ⚪ 未启用  ⚠️ 缺删除权限";
}

inline json listKeyboard(Store& store, int page) {
    const int total = store.countChats(true);
    const int pages = std::max(1, (total + kPageSize - 1) / kPageSize);
    page = std::clamp(page, 0, pages - 1);

    json rows = json::array();
    for (const auto& chat : store.listChats(kPageSize, page * kPageSize, true)) {
        const std::string icon = chat.enabled ? "🟢" : (chat.canDelete ? "⚪" : "⚠️");
        const std::string label = icon + " " + truncateUtf8(chat.title, 24);
        rows.push_back(json::array(
            {json{{"text", label}, {"callback_data", "wd:g:" + std::to_string(chat.chatId)}}}));
    }
    if (rows.empty()) {
        rows.push_back(json::array({json{{"text", "（还没有群）"}, {"callback_data", "wd:home"}}}));
    }

    json nav = json::array();
    if (page > 0) {
        nav.push_back(
            json{{"text", "⬅️ 上一页"}, {"callback_data", "wd:list:" + std::to_string(page - 1)}});
    }
    if (page + 1 < pages) {
        nav.push_back(
            json{{"text", "下一页 ➡️"}, {"callback_data", "wd:list:" + std::to_string(page + 1)}});
    }
    if (!nav.empty()) {
        rows.push_back(nav);
    }
    rows.push_back(json::array({json{{"text", "🏠 控制台"}, {"callback_data", "wd:home"}}}));
    return {{"inline_keyboard", rows}};
}

inline std::string adminDashboardText(Store& store, std::int64_t userId) {
    const auto chats = store.chatsForAdmin(userId);
    int enabled = 0;
    int missing = 0;
    for (const auto& chat : chats) {
        enabled += chat.enabled ? 1 : 0;
        missing += chat.canDelete ? 0 : 1;
    }
    return "🛡 Warden 控制台\n"
           "\n"
           "你管理的群：" +
           std::to_string(chats.size()) +
           " 个（机器人已在群内）\n"
           "其中启用自动清理：" +
           std::to_string(enabled) +
           " 个\n"
           "机器人缺「删除消息」权限：" +
           std::to_string(missing) +
           " 个\n"
           "\n"
           "这里只显示你本人是管理员的群；机器人不在的群不会出现。\n"
           "选择「群列表」查看并修改自己群的设置。";
}

inline std::string adminListText(Store& store, std::int64_t userId, int page) {
    const auto chats = store.chatsForAdmin(userId);
    const int total = static_cast<int>(chats.size());
    const int pages = std::max(1, (total + kPageSize - 1) / kPageSize);
    page = std::clamp(page, 0, pages - 1);
    return "📋 我管理的群（" + std::to_string(page + 1) + "/" + std::to_string(pages) + "）\n共 " +
           std::to_string(total) +
           " 个群，点群名查看详情。\n"
           "\n🟢 已启用  ⚪ 未启用  ⚠️ 机器人缺删除权限";
}

inline json adminListKeyboard(Store& store, std::int64_t userId, int page) {
    const auto chats = store.chatsForAdmin(userId);
    const int total = static_cast<int>(chats.size());
    const int pages = std::max(1, (total + kPageSize - 1) / kPageSize);
    page = std::clamp(page, 0, pages - 1);

    json rows = json::array();
    const int begin = page * kPageSize;
    const int end = std::min(total, begin + kPageSize);
    for (int i = begin; i < end; ++i) {
        const auto& chat = chats[static_cast<std::size_t>(i)];
        const std::string icon = chat.enabled ? "🟢" : (chat.canDelete ? "⚪" : "⚠️");
        const std::string label = icon + " " + truncateUtf8(chat.title, 24);
        rows.push_back(json::array(
            {json{{"text", label}, {"callback_data", "wd:g:" + std::to_string(chat.chatId)}}}));
    }
    if (rows.empty()) {
        rows.push_back(
            json::array({json{{"text", "（还没有你管理的群）"}, {"callback_data", "wd:home"}}}));
    }

    json nav = json::array();
    if (page > 0) {
        nav.push_back(
            json{{"text", "⬅️ 上一页"}, {"callback_data", "wd:list:" + std::to_string(page - 1)}});
    }
    if (page + 1 < pages) {
        nav.push_back(
            json{{"text", "下一页 ➡️"}, {"callback_data", "wd:list:" + std::to_string(page + 1)}});
    }
    if (!nav.empty()) {
        rows.push_back(nav);
    }
    rows.push_back(json::array({json{{"text", "🏠 控制台"}, {"callback_data", "wd:home"}}}));
    return {{"inline_keyboard", rows}};
}

inline std::string detailText(Store& store, const ChatRecord& chat) {
    std::string s = "🛡 " + (chat.title.empty() ? std::string{"（无标题）"} : chat.title) + "\n";
    s += "ID：" + std::to_string(chat.chatId) + " · " + chat.type + "\n";
    s += "我的身份：" + chat.botStatus;
    if (chat.isAdmin()) {
        s += " ✅";
    } else {
        s += " ⚠️（需要管理员）";
    }
    s += "\n\n管理员权限：\n";
    s += "• 删除消息：" + boolMark(chat.canDelete) + "\n";
    s += "• 限制成员：" + boolMark(chat.canRestrict) + "\n";
    s += "• 置顶消息：" + boolMark(chat.canPin) + "\n";
    s += "• 邀请用户：" + boolMark(chat.canInvite) + "\n";
    s += "• 管理群聊：" + boolMark(chat.canManage) + "\n";
    s += "\n自动清理：" + std::string{chat.enabled ? "已启用 🟢" : "未启用 ⚪"} + "\n";
    s += settingsSummary(store, chat.chatId);
    s += "待删除：" + std::to_string(store.pendingDeletionsFor(chat.chatId)) + " 条\n";
    if (chat.leftAt != 0) {
        s += "\n⚠️ 已不在该群\n";
    }
    return s;
}

inline json detailKeyboard(const ChatRecord& chat, bool canLeave = true) {
    json rows = json::array();
    rows.push_back(json::array({json{{"text", chat.enabled ? "⏸ 停用自动清理" : "▶️ 启用自动清理"},
                                     {"callback_data", "wd:t:" + std::to_string(chat.chatId)}}}));
    rows.push_back(json::array(
        {json{{"text", "⚙️ 清理设置"}, {"callback_data", "wd:s:" + std::to_string(chat.chatId)}}}));
    rows.push_back(json::array(
        {json{{"text", "🔄 刷新权限"}, {"callback_data", "wd:r:" + std::to_string(chat.chatId)}},
         json{{"text", "🧹 取消待删"}, {"callback_data", "wd:c:" + std::to_string(chat.chatId)}}}));
    if (canLeave && chat.active()) {
        rows.push_back(json::array({json{
            {"text", "🚪 退出该群"}, {"callback_data", "wd:x:" + std::to_string(chat.chatId)}}}));
    }
    rows.push_back(json::array({json{{"text", "⬅️ 返回列表"}, {"callback_data", "wd:list:0"}}}));
    return {{"inline_keyboard", rows}};
}

inline std::string settingsPageText(Store& store, const ChatRecord& chat) {
    std::string s =
        "⚙️ " + (chat.title.empty() ? std::string{"（无标题）"} : chat.title) + " · 清理设置\n\n";
    s += "状态：" + std::string{chat.enabled ? "已启用 🟢" : "未启用 ⚪"} + "\n";
    s += "点类型按钮切换档位：关闭 → 30秒 → 1分钟 → 5分钟 → 15分钟 → "
         "1小时 → 6小时 → 24小时\n\n";
    for (const auto* kind : kMediaKinds) {
        s += "• " + std::string{kindLabel(kind)} + "：" +
             formatDelay(effectiveDelay(store, chat.chatId, kind)) + "\n";
    }
    s += "\n修改只影响之后的消息；已排程任务保持原到期时间。\n";
    if (!chat.canDelete) {
        s += "⚠️ 我缺少「删除消息」权限，设置保存了也不会真的删。\n";
    }
    return s;
}

inline json settingsPageKeyboard(Store& store, const ChatRecord& chat) {
    json rows = json::array();
    for (const auto* kind : kMediaKinds) {
        const std::string label = std::string{kindLabel(kind)} + "：" +
                                  formatDelay(effectiveDelay(store, chat.chatId, kind)) + " ↻";
        rows.push_back(json::array(
            {json{{"text", label},
                  {"callback_data", "wd:d:" + std::to_string(chat.chatId) + ":" + kind}}}));
    }
    rows.push_back(
        json::array({json{{"text", "全部 5 分钟"},
                          {"callback_data", "wd:a:" + std::to_string(chat.chatId) + ":300"}},
                     json{{"text", "全部关闭"},
                          {"callback_data", "wd:a:" + std::to_string(chat.chatId) + ":0"}}}));
    rows.push_back(json::array({json{{"text", "⬅️ 返回群详情"},
                                     {"callback_data", "wd:g:" + std::to_string(chat.chatId)}}}));
    return {{"inline_keyboard", rows}};
}

// 编辑面板消息；内容没变时 Telegram 会回 400，这里静默吞掉
inline net::awaitable<void> showPanel(BotClient& bot,
                                      std::int64_t chatId,
                                      std::int64_t messageId,
                                      std::string text,
                                      json keyboard,
                                      tgbot::ParseMode parseMode = tgbot::ParseMode::kNone) {
    tgbot::SendOptions options;
    options.parseMode = parseMode;
    options.replyMarkup = keyboard;
    if (messageId != 0) {
        try {
            co_await bot.editMessageText(chatId, messageId, text, options);
            co_return;
        } catch (const tgbot::TelegramApiError& e) {
            if (std::string_view{e.what()}.find("message is not modified") !=
                std::string_view::npos) {
                co_return;
            }
            spdlog::debug("edit panel failed: {}", e.what());
        }
    }
    co_await bot.sendMessage(chatId, text, keyboard);
}

} // namespace warden
