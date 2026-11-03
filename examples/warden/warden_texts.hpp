#pragma once
#include "warden_types.hpp"

namespace warden {
inline std::string htmlEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

inline std::string helpText(std::string_view botName, std::string_view botUsername) {
    // 用 HTML parse_mode 发送，<code> 里的内容在 Telegram 里可以点击复制。
    const std::string title = botName.empty() ? "群务管家" : htmlEscape(botName);
    std::string s = "🛡 <b>" + title +
                    " · 群务管家</b>\n"
                    "\n"
                    "<b>【群管理员】</b>\n"
                    "<code>/autodel</code> —— 查看本群设置\n"
                    "<code>/autodel on</code> / <code>/autodel off</code> —— 开启 / 关闭"
                    "（关闭会取消待删任务）\n"
                    "<code>/autodel 贴纸 20秒</code> —— 单类型时限\n"
                    "<code>/autodel 图片 开启 1分钟</code> —— 中文 + 自然写法\n"
                    "<code>/autodel all 5m</code> —— 所有类型统一时限\n"
                    "<code>/autodel cancel</code> —— 取消待删任务\n"
                    "<code>/autodel reset</code> —— 恢复默认时限\n"
                    "<code>/status</code> —— 我的身份与管理员权限\n"
                    "\n"
                    "<b>【媒体豁免】</b>\n"
                    "回复某人的消息后：\n"
                    "<code>/ignore 贴纸</code> —— 只忽略他的贴纸\n"
                    "<code>/ignore 贴纸 图片</code> —— 忽略多种类型\n"
                    "<code>/ignore 全部</code> —— 忽略他的全部媒体\n"
                    "<code>/restore 贴纸</code> / <code>/restore 全部</code> —— 恢复\n"
                    "<code>/ignore list</code> —— 查看当前豁免规则\n"
                    "管理员：<code>/ignore admin on|off</code>，"
                    "<code>/ignore admin 贴纸 on|off</code>\n"
                    "\n"
                    "<b>也可以直接发（不用 /，用户目标要回复消息）：</b>\n"
                    "<code>忽略 贴纸</code> / <code>忽略 全部</code>\n"
                    "<code>恢复 贴纸</code> / <code>恢复 全部</code>\n"
                    "<code>忽略管理员 全部</code> / <code>恢复管理员 全部</code>\n"
                    "只发“忽略”或“恢复”不会执行；全部媒体必须写“全部”。\n"
                    "<code>@username</code> 只能按用户名匹配，回复或数字 ID 才精确。\n"
                    "\n"
                    "<b>自动清理也可以直接发：</b>\n"
                    "<code>自动删除 贴纸 开启 10秒</code>\n"
                    "<code>自动删除 图片 开启 1分钟</code>\n"
                    "<code>自动删除 视频 关闭</code>\n"
                    "<code>自动删除 全部 开启 5分钟</code>\n"
                    "\n"
                    "<b>类型：</b>贴纸 / 图片 / 视频 / 语音 / 音频 / 动图 / 圆视频 / 全部\n"
                    "sticker / photo / video / voice / audio / animation / video_note / all\n"
                    "<b>时限：</b>10秒、20 秒、1分钟、2小时、1天、30s、5m、2h、1d、"
                    "off/关闭\n"
                    "范围 5 秒–24 小时；off = 不删除\n"
                    "\n"
                    "<b>【私聊】</b>\n"
                    "<code>/panel</code> —— 控制台：owner 看全部，群管理员看自己的群\n"
                    "<code>/mygroups</code> —— 我担任管理员的群（缓存名册）\n"
                    "<code>/id</code> —— 我的用户 ID\n"
                    "\n";
    if (!botUsername.empty()) {
        s += "把我拉进群并设为管理员（勾选「删除消息」）后使用。\n";
        s += "私聊入口：https://t.me/" + std::string{botUsername};
    }
    return s;
}

} // namespace warden
