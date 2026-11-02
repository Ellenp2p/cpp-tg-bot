#pragma once
#include "warden_types.hpp"

namespace warden {
inline std::string introText() {
    return "🛡 我是 Warden，本群的自动清理管家。\n"
           "\n"
           "管理员发 /autodel on 即可启用：贴纸 1 分钟、图片/视频/语音 5 分钟\n"
           "后自动删除（可逐类改时限，也可关掉某一类）。\n"
           "\n"
           "常用命令（管理员）：\n"
           "• /autodel —— 查看本群设置\n"
           "• /autodel 贴纸 20秒 —— 贴纸 20 秒后删\n"
           "• /autodel 图片 开启 1分钟 —— 中文写法\n"
           "• /autodel 视频 关闭 —— 不再删视频\n"
           "• /autodel all 10m —— 所有支持的类型统一 10 分钟\n"
           "• /status —— 检查我的管理员权限\n"
           "\n"
           "也可以直接发（不用 /）：\n"
           "自动删除 贴纸 开启 10秒\n"
           "自动删除 图片 开启 1分钟\n"
           "\n"
           "需要给我「删除消息」管理员权限；回复只发给你。";
}

inline std::string helpText(std::string_view botUsername) {
    // 用 HTML parse_mode 发送，<code> 里的内容在 Telegram 里可以点击复制。
    std::string s =
        "🛡 <b>Warden · 群务管家</b>\n"
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
        "<b>也可以直接发（不用 /）：</b>\n"
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


}
