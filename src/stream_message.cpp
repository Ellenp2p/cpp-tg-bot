#include <tgbot/stream_message.hpp>

#include <boost/asio/awaitable.hpp>

#include <spdlog/spdlog.h>

#include <exception>
#include <string_view>
#include <utility>
#include <vector>

namespace tgbot {

namespace {

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

}  // namespace

StreamMessage::StreamMessage(BotClient& bot, StreamOptions options)
    : bot_(bot), opts_(std::move(options)) {}

boost::asio::awaitable<void> StreamMessage::begin(std::int64_t chatId) {
    chatId_ = chatId;
    text_.clear();
    sent_.clear();
    sentChars_ = 0;
    frozen_ = false;
    messageId_ = 0;

    SendOptions so;
    so.parseMode = ParseMode::kNone;
    so.disableWebPagePreview = opts_.disableWebPagePreview;

    const auto msg = co_await bot_.sendMessage(chatId_, opts_.cursor, so);
    messageId_ = msg.value("message_id", std::int64_t{0});
    sent_ = opts_.cursor;
    lastEdit_ = std::chrono::steady_clock::now();
}

boost::asio::awaitable<void> StreamMessage::append(std::string_view delta) {
    text_.append(delta.data(), delta.size());

    if (messageId_ == 0) {
        co_return;  // 没起来或已失效
    }
    if (text_.size() > opts_.chunkLimit) {
        frozen_ = true;  // 单条装不下了，收尾时分条发
        co_return;
    }
    if (frozen_) {
        co_return;
    }
    if (text_.size() < sentChars_ + opts_.minChars) {
        co_return;  // 增量太少，攒着
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - lastEdit_ < opts_.minInterval) {
        co_return;  // 还没到下一次编辑的窗口
    }
    co_await flush(false);
}

boost::asio::awaitable<void> StreamMessage::flush(bool isFinal) {
    std::string body = text_;
    if (!isFinal) {
        body += opts_.cursor;
    }
    if (body == sent_) {
        co_return;  // 内容没变：Telegram 会回 400 message is not modified
    }

    SendOptions so;
    so.parseMode = isFinal ? opts_.finalParseMode : ParseMode::kNone;
    so.disableWebPagePreview = opts_.disableWebPagePreview;

    try {
        co_await bot_.editMessageText(chatId_, messageId_, body, so);
        sent_ = body;
        sentChars_ = text_.size();
    } catch (const TelegramApiError& e) {
        const std::string_view what{e.what()};
        if (contains(what, "message is not modified")) {
            sent_ = body;  // 已经是最新内容，没什么可做的
            co_return;
        }
        if (contains(what, "message to edit not found") ||
            contains(what, "message can't be edited")) {
            // 被管理员删了 / 权限变了：停止流式，别打断内容生成
            spdlog::warn("stream: message {} 不可编辑，停止流式输出", messageId_);
            messageId_ = 0;
            co_return;
        }
        // 其余错误（429 等）只记日志：流式是尽力而为，不能因此中断生成
        spdlog::warn("stream: edit failed: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("stream: edit failed: {}", e.what());
    }
    lastEdit_ = std::chrono::steady_clock::now();
}

boost::asio::awaitable<void> StreamMessage::finish() {
    if (messageId_ == 0) {
        co_return;
    }

    auto parts = splitMessage(text_, opts_.chunkLimit, opts_.finalParseMode);
    if (parts.empty()) {
        parts.push_back(opts_.cursor);  // 一个字都没收到：至少把光标留着
    }

    SendOptions so;
    so.parseMode = opts_.finalParseMode;
    so.disableWebPagePreview = opts_.disableWebPagePreview;

    try {
        co_await bot_.editMessageText(chatId_, messageId_, parts.front(), so);
    } catch (const std::exception& e) {
        spdlog::warn("stream: final edit failed: {}", e.what());
    }

    for (std::size_t i = 1; i < parts.size(); ++i) {
        try {
            co_await bot_.sendMessage(chatId_, parts[i], so);
        } catch (const std::exception& e) {
            spdlog::warn("stream: send remainder failed: {}", e.what());
        }
    }

    messageId_ = 0;
}

}  // namespace tgbot
