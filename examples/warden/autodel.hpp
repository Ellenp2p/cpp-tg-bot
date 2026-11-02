#pragma once
#include "warden_types.hpp"
#include "warden_util.hpp"
#include "replies.hpp"
#include "warden_views.hpp"
#include "config.hpp"
#include "media.hpp"
#include "scheduler.hpp"
#include "store.hpp"
#include <algorithm>
#include <cctype>
#include <optional>
#include <vector>
namespace warden {
inline std::string kindFromToken(std::string_view token) {
    const auto t = lower(trim(token));
    if (t == "sticker" || t == "贴纸" || t == "表情" || t == "贴图") {
        return "sticker";
    }
    if (t == "photo" || t == "图片" || t == "照片" || t == "图") {
        return "photo";
    }
    if (t == "video" || t == "视频") {
        return "video";
    }
    if (t == "voice" || t == "语音") {
        return "voice";
    }
    if (t == "audio" || t == "音频") {
        return "audio";
    }
    if (t == "animation" || t == "动图" || t == "gif") {
        return "animation";
    }
    if (t == "video_note" || t == "圆视频" || t == "视频留言") {
        return "video_note";
    }
    if (t == "all" || t == "全部" || t == "所有") {
        return "all";
    }
    return {};
}

inline bool isOnWord(std::string_view token) {
    const auto t = lower(trim(token));
    return t == "on" || t == "开启" || t == "打开" || t == "启用";
}

inline bool isOffWord(std::string_view token) {
    const auto t = lower(trim(token));
    return t == "off" || t == "关闭" || t == "停用" || t == "关";
}

inline bool isCancelWord(std::string_view token) {
    const auto t = lower(trim(token));
    return t == "cancel" || t == "取消";
}

inline bool isResetWord(std::string_view token) {
    const auto t = lower(trim(token));
    return t == "reset" || t == "重置" || t == "恢复默认";
}

inline std::vector<std::string> splitTokens(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    for (const unsigned char c : text) {
        if (std::isspace(c)) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(static_cast<char>(c));
        }
    }
    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

inline bool allDigits(std::string_view token) {
    if (token.empty()) {
        return false;
    }
    return std::all_of(token.begin(), token.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
}

inline bool looksLikeUnit(std::string_view token) {
    const auto t = lower(trim(token));
    return t == "s" || t == "sec" || t == "secs" || t == "second" ||
           t == "seconds" || t == "秒" || t == "秒钟" || t == "m" ||
           t == "min" || t == "mins" || t == "minute" ||
           t == "minutes" || t == "分" || t == "分钟" || t == "h" ||
           t == "hr" || t == "hour" || t == "hours" || t == "小时" ||
           t == "d" || t == "day" || t == "days" || t == "天";
}

enum class AutodelAction { kShow, kOn, kOff, kCancel, kReset, kSet };

struct AutodelRequest {
    AutodelAction action = AutodelAction::kShow;
    std::string kind;  // "" / "all" / media kind
    int seconds = 0;
    bool hasSeconds = false;
};

std::optional<AutodelRequest> parseAutodelArgs(std::string_view args,
                                               std::string& error) {
    const auto tokens = splitTokens(args);
    if (tokens.empty()) {
        return AutodelRequest{};
    }

    AutodelRequest req;
    std::size_t i = 0;
    const auto& first = tokens[0];
    if (isOnWord(first)) {
        req.action = AutodelAction::kOn;
        ++i;
    } else if (isOffWord(first)) {
        req.action = AutodelAction::kOff;
        ++i;
    } else if (isCancelWord(first)) {
        req.action = AutodelAction::kCancel;
        ++i;
    } else if (isResetWord(first)) {
        req.action = AutodelAction::kReset;
        ++i;
    } else {
        auto kind = kindFromToken(first);
        if (kind.empty()) {
            error = "无法识别「" + first + "」";
            return std::nullopt;
        }
        req.kind = std::move(kind);
        ++i;
    }

    while (i < tokens.size()) {
        const auto& token = tokens[i];
        if (req.action == AutodelAction::kShow) {
            if (isOnWord(token)) {
                req.action = AutodelAction::kOn;
                ++i;
                continue;
            }
            if (isOffWord(token)) {
                req.action = AutodelAction::kOff;
                ++i;
                continue;
            }
            if (isCancelWord(token)) {
                req.action = AutodelAction::kCancel;
                ++i;
                continue;
            }
            if (isResetWord(token)) {
                req.action = AutodelAction::kReset;
                ++i;
                continue;
            }
        }
        if (req.kind.empty()) {
            auto kind = kindFromToken(token);
            if (!kind.empty()) {
                req.kind = std::move(kind);
                ++i;
                continue;
            }
        }
        // "20 秒" 这种被空格拆开的写法：把数字和单位拼回去
        std::string duration = token;
        if (allDigits(token) && i + 1 < tokens.size() &&
            looksLikeUnit(tokens[i + 1])) {
            duration += tokens[i + 1];
            ++i;
        }
        const auto seconds = parseDelay(duration);
        if (!seconds.has_value()) {
            error = "无法识别「" + token + "」";
            return std::nullopt;
        }
        req.seconds = *seconds;
        req.hasSeconds = true;
        ++i;
    }

    if (req.action == AutodelAction::kCancel ||
        req.action == AutodelAction::kReset) {
        return req;
    }
    if (req.action == AutodelAction::kShow) {
        if (req.kind.empty() && req.hasSeconds) {
            req.kind = "all";  // "自动删除 10秒" → 全部 10 秒
            req.action = AutodelAction::kSet;
        } else if (!req.kind.empty() && req.hasSeconds) {
            req.action = AutodelAction::kSet;
        } else if (!req.kind.empty() && !req.hasSeconds) {
            error = "缺少时限，例如：贴纸 20秒";
            return std::nullopt;
        }
    }
    if (req.action == AutodelAction::kOn && req.kind.empty() &&
        req.hasSeconds) {
        req.kind = "all";
        req.action = AutodelAction::kSet;
    }
    if (req.action == AutodelAction::kOn && !req.kind.empty() &&
        req.hasSeconds) {
        req.action = AutodelAction::kSet;
    }
    if (req.action == AutodelAction::kOff && req.hasSeconds) {
        req.hasSeconds = false;  // "贴纸 关闭 10秒"：以关闭为准
    }
    return req;
}

inline net::awaitable<void> handleAutodel(BotClient& b, Store& store,
                                   DeleteScheduler& sched, const json& msg,
                                   std::string args, bool alreadyAuthorized,
                                   const Config& cfg,
                                   bool replyToTrigger = false) {
    const auto chatId = chatIdOf(msg);
    if (!isGroupType(chatTypeOf(msg))) {
        co_await b.sendMessage(chatId, "这个命令只能在群里用。");
        co_return;
    }
    // 自然语言命令：回复时引用触发它的那条消息；斜杠命令消息会被立即
    // 删除，所以调用方传 false。
    auto reply = [&](std::string text, json markup = nullptr,
                     tgbot::ParseMode mode = tgbot::ParseMode::kNone)
        -> net::awaitable<void> {
        co_await replyEphemeral(b, sched, msg, std::move(text),
                                std::move(markup), mode, replyToTrigger);
    };

    const auto userId = senderId(msg);
    bool authorized = alreadyAuthorized || isOwner(cfg, userId);
    if (!authorized) {
        const auto status = co_await fetchMemberStatus(b, chatId, userId);
        if (!status.has_value() || !isAdminStatus(*status)) {
            co_return;  // 非管理员：静默丢弃
        }
        store.addAdmin(chatId, userId, *status,
                       msg.value("from", json::object())
                           .value("username", std::string{}),
                       nowSec());
    }

    store.ensureChat(chatId, chatTypeOf(msg),
                     msg.value("chat", json::object())
                         .value("title", std::string{}),
                     nowSec());

    std::string error;
    const auto request = parseAutodelArgs(args, error);
    if (!request.has_value()) {
        co_await reply("没看懂：" + error +
                "\n用法：/autodel on | off | 贴纸 20秒 | 图片 开启 1分钟 | "
                "all 5m\n也可以直接发：自动删除 贴纸 开启 10秒");
        co_return;
    }

    const auto req = *request;
    if (req.hasSeconds && req.seconds != 0 &&
        (req.seconds < cfg.minDelaySec || req.seconds > cfg.maxDelaySec)) {
        co_await reply("时限范围：" + std::to_string(cfg.minDelaySec) + " 秒 – " +
                formatDelay(cfg.maxDelaySec) + "。");
        co_return;
    }

    const bool allKinds = req.kind == "all";
    const auto applyAll = [&](int seconds) {
        for (const auto* kind : kMediaKinds) {
            store.setDelay(chatId, kind, seconds);
            if (seconds == 0) {
                sched.cancelKind(chatId, kind);
            }
        }
        if (seconds > 0) {
            store.setEnabled(chatId, true);
        }
    };
    const auto replyKind = [&](const std::string& kind,
                               int seconds) -> net::awaitable<void> {
        co_await reply(std::string{kindLabel(kind)} + "：" + formatDelay(seconds) +
                (seconds > 0 ? "后自动删除。" : "，不再删除。"));
    };

    switch (req.action) {
        case AutodelAction::kShow: {
            const auto record = store.chat(chatId);
            const auto settings = store.settings(chatId);
            std::string text =
                "🧹 本群自动清理：" +
                std::string{settings->enabled ? "已开启 🟢" : "已关闭 ⚪"} + "\n";
            if (record.has_value() && !record->canDelete) {
                text += "⚠️ 我没有「删除消息」权限，设置不会生效。\n";
            }
            text += "\n" + settingsSummary(store, chatId);
            text += "\n管理员：/autodel on | off | 贴纸 20秒 | 图片 开启 1分钟 "
                    "| all 5m";
            text += "\n也可以直接发：自动删除 贴纸 开启 10秒";
            co_await reply(text);
            co_return;
        }
        case AutodelAction::kCancel:
            sched.cancelChat(chatId);
            store.audit(chatId, userId, "autodel.cancel", "");
            co_await reply("🧹 已取消本群所有待删任务。");
            co_return;
        case AutodelAction::kReset:
            store.clearSettings(chatId);
            store.audit(chatId, userId, "autodel.reset", "");
            co_await reply("已恢复默认时限（贴纸 60 秒，图片/视频/语音 5 分钟，"
                "其余关闭）。");
            co_return;
        case AutodelAction::kOn: {
            if (req.kind.empty() || allKinds) {
                store.setEnabled(chatId, true);
                store.audit(chatId, userId, "autodel.on", "");
                std::string text = "✅ 已开启本群自动清理。\n\n" +
                                   settingsSummary(store, chatId);
                const auto record = store.chat(chatId);
                if (!record.has_value() || !record->canDelete) {
                    text += "\n⚠️ 我没有「删除消息」管理员权限，删除会失败；"
                            "请到群设置里勾选后发 /status 复查。";
                }
                co_await reply(text);
                co_return;
            }
            const int seconds = req.hasSeconds
                                    ? req.seconds
                                    : std::max(kindDefaultDelay(req.kind), 300);
            store.setDelay(chatId, req.kind, seconds);
            if (seconds > 0) {
                store.setEnabled(chatId, true);
            } else {
                sched.cancelKind(chatId, req.kind);
            }
            store.audit(chatId, userId, "autodel." + req.kind,
                        std::to_string(seconds));
            co_await replyKind(req.kind, seconds);
            co_return;
        }
        case AutodelAction::kOff:
            if (req.kind.empty()) {
                store.setEnabled(chatId, false);
                sched.cancelChat(chatId);
                store.audit(chatId, userId, "autodel.off", "");
                co_await reply("⏸ 已关闭自动清理，并取消了本群待删任务。");
                co_return;
            }
            if (allKinds) {
                applyAll(0);
                store.audit(chatId, userId, "autodel.all", "0");
                co_await reply("已把所有支持的类型关闭。");
                co_return;
            }
            store.setDelay(chatId, req.kind, 0);
            sched.cancelKind(chatId, req.kind);
            store.audit(chatId, userId, "autodel." + req.kind, "0");
            co_await replyKind(req.kind, 0);
            co_return;
        case AutodelAction::kSet:
            if (allKinds) {
                applyAll(req.seconds);
                store.audit(chatId, userId, "autodel.all",
                            std::to_string(req.seconds));
                co_await reply(req.seconds > 0
                        ? "✅ 已把所有支持的类型设为：" +
                              formatDelay(req.seconds) + "。"
                        : "已把所有支持的类型关闭。");
                co_return;
            }
            store.setDelay(chatId, req.kind, req.seconds);
            if (req.seconds > 0) {
                store.setEnabled(chatId, true);
            } else {
                sched.cancelKind(chatId, req.kind);
            }
            store.audit(chatId, userId, "autodel." + req.kind,
                        std::to_string(req.seconds));
            co_await replyKind(req.kind, req.seconds);
            co_return;
    }
}


}
