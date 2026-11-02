#pragma once

// 媒体类型识别 + 延迟时间解析。刻意做成纯函数，方便单测和复用到别的机器人。

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace warden {

// 支持自动清理的媒体类型（顺序就是面板展示顺序）
inline constexpr std::array<const char*, 7> kMediaKinds{
    "sticker", "photo", "video", "voice", "audio", "animation", "video_note"};

inline std::string_view kindLabel(std::string_view kind) {
    if (kind == "sticker") return "贴纸";
    if (kind == "photo") return "图片";
    if (kind == "video") return "视频";
    if (kind == "voice") return "语音";
    if (kind == "audio") return "音频";
    if (kind == "animation") return "动图";
    if (kind == "video_note") return "圆视频";
    return "未知";
}

// 默认时限（秒）：0 = 默认不删除。
// 只给用户点名要的四类默认值，其余默认关，避免一开机器人就误删。
inline int kindDefaultDelay(std::string_view kind) {
    if (kind == "sticker") return 60;
    if (kind == "photo") return 300;
    if (kind == "video") return 300;
    if (kind == "voice") return 300;
    return 0;
}

// 从 message 里识别媒体类型；不是媒体返回空串。
// 注意顺序：animation 同时带 document，video_note 不是 video。
inline std::string detectMediaKind(const nlohmann::json& msg) {
    if (msg.contains("sticker") && msg["sticker"].is_object()) {
        return "sticker";
    }
    if (msg.contains("photo") && msg["photo"].is_array()) {
        return "photo";
    }
    if (msg.contains("video_note") && msg["video_note"].is_object()) {
        return "video_note";
    }
    if (msg.contains("video") && msg["video"].is_object()) {
        return "video";
    }
    if (msg.contains("voice") && msg["voice"].is_object()) {
        return "voice";
    }
    if (msg.contains("audio") && msg["audio"].is_object()) {
        return "audio";
    }
    if (msg.contains("animation") && msg["animation"].is_object()) {
        return "animation";
    }
    return {};
}

// 解析 "60" / "30s" / "30 秒" / "5m" / "1分钟" / "2h" / "1天" / "off"
// → 秒（off / 关闭 / 关 = 0）。解析失败返回 nullopt；范围检查交给调用方。
// 兼容中文单位、数字与单位之间的空格，以及英文大小写。
inline std::optional<int> parseDelay(std::string_view raw) {
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) {
        raw.remove_prefix(1);
    }
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) {
        raw.remove_suffix(1);
    }
    if (raw.empty()) {
        return std::nullopt;
    }

    auto lowerTrim = [](std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (const unsigned char c : s) {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
        while (!out.empty() &&
               std::isspace(static_cast<unsigned char>(out.front()))) {
            out.erase(out.begin());
        }
        while (!out.empty() &&
               std::isspace(static_cast<unsigned char>(out.back()))) {
            out.pop_back();
        }
        return out;
    };

    const auto lower = lowerTrim(raw);
    if (lower == "off" || lower == "close" || lower == "关闭" ||
        lower == "关" || lower == "0") {
        return 0;
    }

    std::size_t i = 0;
    while (i < lower.size() &&
           std::isspace(static_cast<unsigned char>(lower[i]))) {
        ++i;
    }
    const std::size_t digitStart = i;
    std::int64_t value = 0;
    while (i < lower.size() &&
           std::isdigit(static_cast<unsigned char>(lower[i]))) {
        value = value * 10 + (lower[i] - '0');
        if (value > 10'000'000) {
            return std::nullopt;  // 防溢出，也拦住手滑打出的一长串数字
        }
        ++i;
    }
    if (i == digitStart) {
        return std::nullopt;
    }
    while (i < lower.size() &&
           std::isspace(static_cast<unsigned char>(lower[i]))) {
        ++i;
    }
    const std::string unit = lower.substr(i);

    std::int64_t multiplier = 1;
    if (unit.empty() || unit == "s" || unit == "sec" || unit == "secs" ||
        unit == "second" || unit == "seconds" || unit == "秒" ||
        unit == "秒钟") {
        multiplier = 1;
    } else if (unit == "m" || unit == "min" || unit == "mins" ||
               unit == "minute" || unit == "minutes" || unit == "分" ||
               unit == "分钟") {
        multiplier = 60;
    } else if (unit == "h" || unit == "hr" || unit == "hour" ||
               unit == "hours" || unit == "小时") {
        multiplier = 3600;
    } else if (unit == "d" || unit == "day" || unit == "days" ||
               unit == "天") {
        multiplier = 86400;
    } else {
        return std::nullopt;
    }

    const auto seconds = value * multiplier;
    if (seconds > 2'000'000) {
        return std::nullopt;
    }
    return static_cast<int>(seconds);
}

inline std::string formatDelay(int seconds) {
    if (seconds <= 0) {
        return "关闭";
    }
    if (seconds % 86400 == 0) {
        return std::to_string(seconds / 86400) + " 天";
    }
    if (seconds % 3600 == 0) {
        return std::to_string(seconds / 3600) + " 小时";
    }
    if (seconds % 60 == 0) {
        return std::to_string(seconds / 60) + " 分钟";
    }
    return std::to_string(seconds) + " 秒";
}

}  // namespace warden
