#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tgbot {

// Telegram Bot API 的 parse_mode。
// 经验：程序拼接的内容优先用 kHtml（只需转义 & < >）；
// kMarkdownV2 有 18 个元字符必须转义，漏一个整条消息就 400。
enum class ParseMode {
    kNone,       // 纯文本，不解析
    kHtml,       // <b> <i> <u> <s> <code> <pre> <a href> <blockquote> ...
    kMarkdownV2, // *bold* _italic_ __underline__ ~strike~ ||spoiler|| `code`
};

// kNone 返回 nullptr（调用方据此决定是否传 parse_mode 参数）
const char* parseModeName(ParseMode mode);

// HTML 模式转义：& < >
std::string escapeHtml(std::string_view text);

// MarkdownV2 转义：_ * [ ] ( ) ~ ` > # + - = | { } . !
std::string escapeMarkdownV2(std::string_view text);

// 按单条消息上限切分（Telegram 上限 4096 个字符，解析后计）。
// - 优先在换行处断开，其次空格，实在没有才硬切
// - 不会切断 UTF-8 多字节字符
// - mode == kHtml 时不会把 HTML 标签切成两半
// 返回的每一段都已去掉首尾空白；原文为空时返回空 vector。
std::vector<std::string> splitMessage(std::string_view text,
                                      std::size_t limit = 4096,
                                      ParseMode mode = ParseMode::kNone);

} // namespace tgbot
