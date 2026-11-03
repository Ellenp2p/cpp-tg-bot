#pragma once

// 回复式媒体豁免：/ignore、/restore。
// 目标通过 reply_to_message.from.id 精确取得，不依赖 @username。

#include "autodel.hpp"
#include "exemptions.hpp"
#include "media.hpp"
#include "replies.hpp"
#include "scheduler.hpp"
#include "store.hpp"
#include "warden_chat_sync.hpp"
#include "warden_types.hpp"
#include "warden_util.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace warden {

enum class IgnoreAction { kAdd, kRemove, kList };
enum class IgnoreScope { kReply, kUser, kUsername, kAdmin };

struct IgnoreRequest {
    IgnoreAction action = IgnoreAction::kAdd;
    IgnoreScope scope = IgnoreScope::kReply;
    std::int64_t userId = 0;
    std::string username;
    std::vector<std::string> kinds; // empty = all media kinds
};

inline std::vector<std::string> allMediaKinds() {
    return std::vector<std::string>(kMediaKinds.begin(), kMediaKinds.end());
}

inline std::string kindListText(const std::vector<std::string>& kinds) {
    if (kinds.size() == kMediaKinds.size()) {
        return "全部媒体";
    }
    std::string out;
    for (const auto& kind : kinds) {
        if (!out.empty()) {
            out += "、";
        }
        out += kindLabel(kind);
    }
    return out;
}

inline bool isAllKindsToken(std::string_view token) {
    const auto t = lower(trim(token));
    return t == "all" || t == "全部" || t == "所有";
}

inline std::optional<IgnoreRequest> parseIgnoreArgs(std::string_view args,
                                                    bool defaultRemove,
                                                    bool hasReply,
                                                    std::string& error) {
    auto tokens = splitTokens(args);
    IgnoreRequest req;
    req.action = defaultRemove ? IgnoreAction::kRemove : IgnoreAction::kAdd;
    if (tokens.empty()) {
        if (!hasReply) {
            if (defaultRemove) {
                error = "请回复某个人的消息，再发 /restore。";
                return std::nullopt;
            }
            req.action = IgnoreAction::kList;
        } else {
            error = "请指定类型；忽略/恢复全部媒体请写「全部」。";
            return std::nullopt;
        }
        return req;
    }

    std::size_t i = 0;
    if (lower(trim(tokens[0])) == "list" || lower(trim(tokens[0])) == "列表") {
        req.action = IgnoreAction::kList;
        return req;
    }
    if (lower(trim(tokens[0])) == "remove" || lower(trim(tokens[0])) == "restore" ||
        lower(trim(tokens[0])) == "恢复") {
        req.action = IgnoreAction::kRemove;
        ++i;
    }

    if (i < tokens.size() &&
        (lower(trim(tokens[i])) == "admin" || lower(trim(tokens[i])) == "管理员" ||
         lower(trim(tokens[i])) == "管理員")) {
        req.scope = IgnoreScope::kAdmin;
        ++i;
        bool sawOn = false;
        bool sawOff = false;
        while (i < tokens.size()) {
            const auto token = lower(trim(tokens[i]));
            if (token == "on" || token == "开启" || token == "打开") {
                sawOn = true;
                ++i;
                continue;
            }
            if (token == "off" || token == "关闭" || token == "关") {
                sawOff = true;
                ++i;
                continue;
            }
            if (isAllKindsToken(token)) {
                req.kinds = allMediaKinds();
                ++i;
                continue;
            }
            const auto kind = kindFromToken(token);
            if (kind.empty() || kind == "all") {
                error = "无法识别「" + token + "」";
                return std::nullopt;
            }
            req.kinds.push_back(kind);
            ++i;
        }
        if (sawOn && sawOff) {
            error = "on 和 off 不能同时出现";
            return std::nullopt;
        }
        if (sawOn) {
            req.action = IgnoreAction::kAdd;
        } else if (sawOff) {
            req.action = IgnoreAction::kRemove;
        }
        if (req.kinds.empty()) {
            if (sawOn || sawOff) {
                req.kinds = allMediaKinds();
            } else {
                error = "请指定类型；全部媒体请写「全部」。";
                return std::nullopt;
            }
        }
        return req;
    }

    if (i < tokens.size()) {
        const auto& token = tokens[i];
        if (!token.empty() && token.front() == '@') {
            req.scope = IgnoreScope::kUsername;
            req.username = lower(trim(std::string_view{token}.substr(1)));
            if (req.username.empty()) {
                error = "用户名不能为空";
                return std::nullopt;
            }
            ++i;
        } else if (lower(trim(token)) == "id") {
            if (i + 1 >= tokens.size() || !allDigits(tokens[i + 1])) {
                error = "id 后面需要数字 user id";
                return std::nullopt;
            }
            req.scope = IgnoreScope::kUser;
            try {
                req.userId = std::stoll(tokens[i + 1]);
            } catch (const std::exception&) {
                error = "user id 太大";
                return std::nullopt;
            }
            i += 2;
        } else if (allDigits(token)) {
            req.scope = IgnoreScope::kUser;
            try {
                req.userId = std::stoll(token);
            } catch (const std::exception&) {
                error = "user id 太大";
                return std::nullopt;
            }
            ++i;
        } else if (hasReply) {
            req.scope = IgnoreScope::kReply;
        } else {
            error = "请回复某个人的消息，或使用 @username / 数字 ID";
            return std::nullopt;
        }
    } else if (!hasReply && req.scope != IgnoreScope::kAdmin) {
        error = "请回复某个人的消息，或使用 @username / 数字 ID";
        return std::nullopt;
    }

    while (i < tokens.size()) {
        const auto token = lower(trim(tokens[i]));
        if (isAllKindsToken(token)) {
            req.kinds = allMediaKinds();
            ++i;
            continue;
        }
        const auto kind = kindFromToken(token);
        if (kind.empty() || kind == "all") {
            error = "无法识别「" + tokens[i] + "」";
            return std::nullopt;
        }
        req.kinds.push_back(kind);
        ++i;
    }
    if (req.kinds.empty()) {
        error = "请指定类型；忽略/恢复全部媒体请写「全部」。";
        return std::nullopt;
    }
    return req;
}

inline std::string exemptionLabel(std::string_view scope,
                                  std::int64_t userId,
                                  std::string_view username) {
    if (scope == "admin") {
        return "所有管理员";
    }
    if (scope == "username") {
        return "@" + std::string{username} + "（用户名规则）";
    }
    return "用户 " + std::to_string(userId) +
           (username.empty() ? std::string{} : " (@" + std::string{username} + ")");
}

inline std::string exemptionListText(const ChatSettings& settings) {
    if (settings.exemptions.empty()) {
        return "本群没有媒体豁免规则。";
    }
    struct Group {
        std::string scope;
        std::int64_t userId = 0;
        std::string username;
        std::vector<std::string> kinds;
    };
    std::vector<Group> groups;
    for (const auto& item : settings.exemptions) {
        auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) {
            return g.scope == item.scope && g.userId == item.userId && g.username == item.username;
        });
        if (it == groups.end()) {
            groups.push_back(Group{item.scope, item.userId, item.username, {}});
            it = std::prev(groups.end());
        }
        if (std::find(it->kinds.begin(), it->kinds.end(), item.kind) == it->kinds.end()) {
            it->kinds.push_back(item.kind);
        }
    }
    std::string text = "🧹 本群媒体豁免：\n";
    for (auto& group : groups) {
        std::sort(group.kinds.begin(), group.kinds.end());
        text += "\n• " + exemptionLabel(group.scope, group.userId, group.username) + "：" +
                kindListText(group.kinds);
    }
    text +=
        "\n\n用法：回复某人的消息，发 /ignore 贴纸 或 /ignore 全部；"
        "恢复用 /restore 贴纸 或 /restore 全部。";
    return text;
}

inline net::awaitable<void> handleIgnore(BotClient& b,
                                         Store& store,
                                         DeleteScheduler& sched,
                                         const json& msg,
                                         std::string args,
                                         bool defaultRemove,
                                         bool alreadyAuthorized,
                                         const Config& cfg,
                                         bool replyToTrigger = false) {
    const auto chatId = chatIdOf(msg);
    if (!isGroupType(chatTypeOf(msg))) {
        co_await b.sendMessage(chatId, "这个命令只能在群里用。");
        co_return;
    }
    auto reply = [&](std::string text,
                     tgbot::ParseMode mode = tgbot::ParseMode::kNone) -> net::awaitable<void> {
        co_await replyEphemeral(b, sched, msg, std::move(text), nullptr, mode, replyToTrigger);
    };

    const auto userId = senderId(msg);
    bool authorized = alreadyAuthorized || isOwner(cfg, userId);
    if (!authorized) {
        const auto status = co_await fetchMemberStatus(b, chatId, userId);
        if (!status.has_value() || !isAdminStatus(*status)) {
            co_return;
        }
        store.addAdmin(chatId,
                       userId,
                       *status,
                       msg.value("from", json::object()).value("username", std::string{}),
                       nowSec());
    }

    store.ensureChat(chatId,
                     chatTypeOf(msg),
                     msg.value("chat", json::object()).value("title", std::string{}),
                     nowSec());

    const auto replyMsg = msg.value("reply_to_message", json::object());
    const auto target = replyMsg.value("from", json::object());
    const auto targetId = target.value("id", std::int64_t{0});
    const auto targetUsername = lower(target.value("username", std::string{}));
    const auto targetName = target.value("first_name", std::string{});
    const auto targetSenderChat =
        replyMsg.value("sender_chat", json::object()).value("id", std::int64_t{0});
    const bool replyAnonymousAdmin = targetSenderChat == chatId;
    const bool hasReply = targetId != 0 || replyAnonymousAdmin;

    std::string error;
    const auto parsed = parseIgnoreArgs(args, defaultRemove, hasReply, error);
    if (!parsed.has_value()) {
        const std::string command = defaultRemove ? "/restore" : "/ignore";
        co_await reply("没看懂：" + error + "\n用法：回复某人的消息后发 " + command +
                       " 贴纸；全部媒体请发 " + command + " 全部。");
        co_return;
    }
    const auto req = *parsed;

    if (req.action == IgnoreAction::kList) {
        co_await reply(exemptionListText(*store.settings(chatId)));
        co_return;
    }

    std::vector<std::string> kinds = req.kinds;
    if (kinds.empty()) {
        kinds = allMediaKinds();
    }
    std::sort(kinds.begin(), kinds.end());
    kinds.erase(std::unique(kinds.begin(), kinds.end()), kinds.end());

    const bool adding = req.action == IgnoreAction::kAdd;
    const std::string actionName = adding ? "忽略" : "恢复";

    if (req.scope == IgnoreScope::kAdmin) {
        if (adding) {
            co_await refreshAdmins(b, store, chatId);
            const auto admins = store.adminIds(chatId);
            for (const auto& kind : kinds) {
                store.addExemption(chatId, "admin", 0, {}, kind, userId, nowSec());
                for (const auto adminId : admins) {
                    sched.cancelSender(chatId, adminId, kind);
                }
            }
        } else {
            for (const auto& kind : kinds) {
                store.removeExemption(chatId, "admin", 0, {}, kind);
            }
        }
        store.audit(chatId,
                    userId,
                    adding ? "ignore.admin.add" : "ignore.admin.remove",
                    kindListText(kinds));
        co_await reply("✅ 已" + actionName + "所有管理员的" + kindListText(kinds) + "。\n" +
                       (adding ? "之后管理员发出的同类媒体不会自动删除。"
                               : "不会补删豁免期间的媒体；之后新消息会重新计时。"));
        co_return;
    }

    if (replyAnonymousAdmin) {
        co_await reply("匿名管理员无法按个人识别。\n请用 /ignore admin 或 /ignore admin 贴纸。");
        co_return;
    }

    std::string scope;
    std::int64_t subjectId = 0;
    std::string username;
    std::string label;
    if (req.scope == IgnoreScope::kReply) {
        if (!hasReply) {
            co_await reply("请回复某个人的消息，再发 /ignore 或 /restore。");
            co_return;
        }
        scope = "user";
        subjectId = targetId;
        username = targetUsername;
        if (!targetUsername.empty()) {
            label = "@" + targetUsername;
        } else {
            label = targetName.empty() ? std::to_string(targetId) : targetName;
        }
    } else if (req.scope == IgnoreScope::kUser) {
        scope = "user";
        subjectId = req.userId;
        label = std::to_string(subjectId);
    } else {
        scope = "username";
        username = req.username;
        label = "@" + username;
    }

    if (adding) {
        for (const auto& kind : kinds) {
            store.addExemption(chatId, scope, subjectId, username, kind, userId, nowSec());
            if (subjectId != 0) {
                sched.cancelSender(chatId, subjectId, kind);
            }
        }
        store.audit(chatId, userId, adding ? "ignore.add" : "ignore.remove", label);
        if (scope == "username") {
            co_await reply("✅ 已记录 " + label + " 的" + kindListText(kinds) +
                           "。\nBot API 无法立即解析普通用户名；之后对方发言时按用户名匹配，"
                           "改名后需要重新设置。");
        } else {
            co_await reply("✅ 已忽略 " + label + " 的" + kindListText(kinds) +
                           "。\n已取消此前待删的同类任务。");
        }
        co_return;
    }

    for (const auto& kind : kinds) {
        store.removeExemption(chatId, scope, subjectId, username, kind);
    }
    if (scope == "user" && !username.empty()) {
        for (const auto& kind : kinds) {
            store.removeExemption(chatId, "username", 0, username, kind);
        }
    }
    store.audit(chatId, userId, "ignore.remove", label);
    co_await reply("✅ 已恢复 " + label + " 的" + kindListText(kinds) +
                   "。\n忽略期间的消息没有删除任务，不会补删；之后的新消息会正常计时。");
}

} // namespace warden
