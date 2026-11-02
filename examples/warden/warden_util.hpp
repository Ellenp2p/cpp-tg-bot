#pragma once
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "warden_types.hpp"
namespace warden {
inline std::int64_t nowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return std::string{s};
}

inline std::string lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

inline bool isGroupType(std::string_view type) {
    return type == "group" || type == "supergroup";
}

inline std::int64_t chatIdOf(const json& msg) {
    const auto it = msg.find("chat");
    return it == msg.end() ? 0 : it->value("id", std::int64_t{0});
}

inline std::int64_t senderId(const json& msg) {
    return msg.value("from", json::object()).value("id", std::int64_t{0});
}

inline std::string chatTypeOf(const json& msg) {
    const auto it = msg.find("chat");
    return it == msg.end() ? std::string{} : it->value("type", std::string{});
}

inline std::string displayName(const json& user) {
    std::string name = user.value("first_name", std::string{});
    const auto last = user.value("last_name", std::string{});
    if (!last.empty()) {
        if (!name.empty()) {
            name += ' ';
        }
        name += last;
    }
    if (name.empty()) {
        name = user.value("username", std::string{});
    }
    return name.empty() ? std::to_string(user.value("id", std::int64_t{0})) : name;
}

// 命令行参数：去掉命令本身和 @Bot 后缀后的剩余文本
inline std::string commandArgs(const json& msg) {
    const std::string text = msg.value("text", std::string{});
    const auto space = text.find(' ');
    return space == std::string::npos ? std::string{}
                                      : trim(std::string_view{text}.substr(space + 1));
}

std::pair<std::string, std::string> splitFirst(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    const auto space = s.find(' ');
    if (space == std::string_view::npos) {
        return {std::string{s}, std::string{}};
    }
    return {std::string{s.substr(0, space)}, trim(s.substr(space + 1))};
}

// UTF-8 安全截断（按钮标题用）
inline std::string truncateUtf8(std::string_view s, std::size_t maxChars) {
    std::string out;
    std::size_t chars = 0;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        if (i + len > s.size()) {
            break;
        }
        if (chars == maxChars) {
            out += "…";
            break;
        }
        out.append(s.substr(i, len));
        i += len;
        ++chars;
    }
    return out;
}

inline bool isAdminStatus(std::string_view status) {
    return status == "creator" || status == "administrator";
}

inline std::string boolMark(bool ok) {
    return ok ? "✅" : "❌";
}

net::awaitable<std::optional<std::string>> fetchMemberStatus(BotClient& bot,
                                                             std::int64_t chatId,
                                                             std::int64_t userId) {
    try {
        const auto member = co_await bot.getChatMember(chatId, userId);
        co_return member.value("status", std::string{});
    } catch (const std::exception& e) {
        spdlog::warn("getChatMember failed: chat={} user={}: {}", chatId, userId, e.what());
        co_return std::nullopt;
    }
}

// 群内回复：优先 ephemeral（仅操作者可见）。ephemeral 不可用（机器人不是
// 管理员 / 客户端不支持）时，降级成群内普通回复，并在 reply TTL 后删除；
// 普通回复也发不出去时，再尝试私聊发送者（同样按 TTL 删除）。

inline bool isOwnerId(const std::vector<std::int64_t>& owners, std::int64_t userId) {
    return std::find(owners.begin(), owners.end(), userId) != owners.end();
}

// 群里的 `/命令` 立即删除。必须用普通 deleteMessage；ephemeral command

inline std::string commandNameFromMessage(const json& msg) {
    const auto text = msg.value("text", std::string{});
    if (text.size() < 2 || text.front() != '/') {
        return {};
    }
    const auto end = text.find(' ');
    return text.substr(1, end == std::string::npos ? std::string::npos : end - 1);
}

inline bool commandWasMentioned(const json& msg) {
    return msg.value("_tgbot_command_mentioned", false);
}

// 未注册命令是否明确写给了本 bot：/foo@OurBot。没有 @ 的 /foo 不动，
// /foo@OtherBot 也不动，避免误删别的 bot 的命令。
inline bool isCommandAddressedToBot(const json& msg, std::string_view botUsername) {
    if (botUsername.empty()) {
        return false;
    }
    const auto text = msg.value("text", std::string{});
    if (text.size() < 2 || text.front() != '/') {
        return false;
    }
    const auto end = text.find(' ');
    const auto token =
        std::string_view{text}.substr(1, end == std::string::npos ? std::string::npos : end - 1);
    const auto at = token.find('@');
    if (at == std::string_view::npos || at + 1 >= token.size()) {
        return false;
    }
    return lower(std::string{token.substr(at + 1)}) == lower(std::string{botUsername});
}

// 命令总守卫：所有“私聊/群、管理员/owner、群里拒绝后是忽略还是提示”

} // namespace warden
