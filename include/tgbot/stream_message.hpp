#pragma once

#include <tgbot/bot_client.hpp>
#include <tgbot/rich_text.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace tgbot {

// 流式输出（Telegram 没有"流"，只能 send 一条再反复 editMessageText）
//
// 用法：
//   tgbot::StreamMessage msg(bot);
//   co_await msg.begin(chatId);
//   while (auto delta = provider.next()) co_await msg.append(delta);  // 想多快就多快
//   co_await msg.finish();
//
// 内部会：
//   - 按 minInterval / minChars 节流（Telegram 编辑有频率限制，不能 token 一来就 edit）
//   - 内容没变化时不发（否则 400 message is not modified）
//   - 消息被删/不可编辑时自动停止，不抛异常打断生成
//   - 超过 4096 字符上限时冻结当前消息，收尾时自动分条续发
struct StreamOptions {
    // 两次编辑的最小间隔。官方没给精确值，1 秒上下是安全区
    std::chrono::milliseconds minInterval{1200};
    // 至少新增这么多字符才值得编辑一次（太小会白白消耗配额）
    std::size_t minChars = 8;
    // 单条消息的上限留点余量（Telegram 是 4096）
    std::size_t chunkLimit = 4000;
    // 流式过程中显示的"光标"；收尾时去掉
    std::string cursor = "\xE2\x96\x8D";  // ▍
    // 收尾时用哪种 parse_mode。默认 kNone：
    // 只有在 append 的内容本身是已转义的 HTML 时才设 kHtml，
    // 否则未转义的 '<' 会让整条消息 400 can't parse entities
    ParseMode finalParseMode = ParseMode::kNone;
    bool disableWebPagePreview = true;
};

class StreamMessage {
public:
    explicit StreamMessage(BotClient& bot, StreamOptions options = {});

    // 发一条占位消息（内容就是光标），拿到 message_id
    boost::asio::awaitable<void> begin(std::int64_t chatId);

    // 追加增量。节流不满足时会直接返回，不产生网络请求
    boost::asio::awaitable<void> append(std::string_view delta);

    // 收尾：去掉光标做最后一次编辑；超长则把剩余内容分条发出
    boost::asio::awaitable<void> finish();

    bool active() const { return messageId_ != 0; }
    std::int64_t messageId() const { return messageId_; }
    const std::string& text() const { return text_; }

private:
    boost::asio::awaitable<void> flush(bool isFinal);

    BotClient& bot_;
    StreamOptions opts_;
    std::int64_t chatId_ = 0;
    std::int64_t messageId_ = 0;
    std::string text_;   // 已收到的全部内容
    std::string sent_;   // 最后一次成功发出去的内容（用于去重）
    std::size_t sentChars_ = 0;
    bool frozen_ = false;  // 超过单条上限，后续只累积不再编辑
    std::chrono::steady_clock::time_point lastEdit_{};
};

}  // namespace tgbot
