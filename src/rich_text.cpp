#include <tgbot/rich_text.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace tgbot {

const char* parseModeName(ParseMode mode) {
    switch (mode) {
        case ParseMode::kHtml:
            return "HTML";
        case ParseMode::kMarkdownV2:
            return "MarkdownV2";
        case ParseMode::kNone:
            break;
    }
    return nullptr;
}

std::string escapeHtml(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
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

std::string escapeMarkdownV2(std::string_view text) {
    static constexpr std::string_view kSpecial = R"(_*[]()~`>#+-=|{}.!)";
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        if (kSpecial.find(c) != std::string_view::npos) {
            out += '\\';
        }
        out += c;
    }
    return out;
}

namespace {

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void trimTrailingSpace(std::string& s) {
    while (!s.empty() && isSpace(s.back())) {
        s.pop_back();
    }
}

// cut 落在 UTF-8 续字节上时，往回退到字符边界
std::size_t backOffToCharBoundary(std::string_view text, std::size_t pos, std::size_t cut) {
    while (cut > pos + 1 && cut < text.size() &&
           (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return cut;
}

// cut 落在一个 HTML 标签中间时，退到这个标签的 '<' 之前
std::size_t backOffOutOfTag(std::string_view text, std::size_t pos, std::size_t cut) {
    const std::size_t lt = text.rfind('<', cut - 1);
    if (lt == std::string_view::npos || lt < pos) {
        return cut;
    }
    const std::size_t gt = text.find('>', lt);
    if (gt == std::string_view::npos || gt >= cut) {
        return lt > pos ? lt : cut;
    }
    return cut;
}

struct OpenTag {
    std::string open; // 完整的开标签文本（含属性），重开时原样拼回去
    std::string name;
};

// 扫描 [begin,end) 内的标签，返回结束时仍未闭合的标签栈（按出现顺序）
std::vector<OpenTag> scanUnclosedTags(std::string_view text, std::size_t begin, std::size_t end) {
    std::vector<OpenTag> stack;
    std::size_t i = begin;
    while (i < end) {
        const std::size_t lt = text.find('<', i);
        if (lt == std::string_view::npos || lt >= end) {
            break;
        }
        const std::size_t gt = text.find('>', lt);
        if (gt == std::string_view::npos || gt >= end) {
            break; // 标签没闭合，交给外层处理
        }
        std::string_view tag = text.substr(lt + 1, gt - lt - 1);
        if (!tag.empty() && tag.back() == '/') {
            i = gt + 1; // 自闭合标签，忽略
            continue;
        }
        if (!tag.empty() && tag.front() == '/') {
            std::string_view name = tag.substr(1);
            while (!name.empty() && isSpace(name.back())) {
                name.remove_suffix(1);
            }
            for (std::size_t k = stack.size(); k > 0; --k) {
                if (stack[k - 1].name == name) {
                    stack.resize(k - 1);
                    break;
                }
            }
        } else {
            std::string_view name = tag.substr(0, tag.find_first_of(" \t\r\n"));
            if (!name.empty()) {
                stack.push_back(
                    OpenTag{std::string{text.substr(lt, gt - lt + 1)}, std::string{name}});
            }
        }
        i = gt + 1;
    }
    return stack;
}

} // namespace

std::vector<std::string> splitMessage(std::string_view text, std::size_t limit, ParseMode mode) {
    std::vector<std::string> parts;
    if (limit == 0) {
        limit = 1;
    }

    // HTML 模式下跨段需要补的"重开前缀"（上一段结束时仍未闭合的标签）
    std::string reopen;

    std::size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && isSpace(text[pos])) {
            ++pos; // 段首空白丢掉（换行/缩进不进下一条消息）
        }
        if (pos >= text.size()) {
            break;
        }

        const std::size_t take = std::min(limit, text.size() - pos);
        std::size_t cut = pos + take;

        if (cut < text.size()) {
            // 1) 换行处断开最自然，2) 其次空格，3) HTML 别切在标签中间
            const std::size_t nl = text.rfind('\n', cut - 1);
            const std::size_t sp = text.rfind(' ', cut - 1);
            if (nl != std::string_view::npos && nl > pos + take / 2) {
                cut = nl + 1;
            } else if (sp != std::string_view::npos && sp > pos + take / 2) {
                cut = sp + 1;
            } else if (mode == ParseMode::kHtml) {
                cut = backOffOutOfTag(text, pos, cut);
            }
        }

        cut = backOffToCharBoundary(text, pos, cut);
        if (cut <= pos) {
            // 极端情况（比如一个超长标签）：放弃美化，硬切
            cut = backOffToCharBoundary(text, pos, pos + take);
        }

        std::string chunk = reopen;
        chunk.append(text.substr(pos, cut - pos));
        reopen.clear();

        if (mode == ParseMode::kHtml) {
            // 本段结束时仍未闭合的标签：在这里补闭合，下一段开头重新打开
            const auto open = scanUnclosedTags(chunk, 0, chunk.size());
            for (auto it = open.rbegin(); it != open.rend(); ++it) {
                chunk += "</" + it->name + ">";
                reopen = it->open + reopen; // 保持嵌套顺序
            }
        }

        trimTrailingSpace(chunk);
        if (!chunk.empty()) {
            parts.push_back(std::move(chunk));
        }
        pos = cut;
    }
    return parts;
}

} // namespace tgbot
