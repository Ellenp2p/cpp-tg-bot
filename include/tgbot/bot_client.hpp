#pragma once

#include <tgbot/http_client.hpp>
#include <tgbot/rate_limiter.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>

namespace tgbot {

// Telegram 返回 {"ok": false, ...} 或 HTTP 层错误时抛出
struct TelegramApiError : std::runtime_error {
    long errorCode;
    TelegramApiError(long code, const std::string& description);
};

// Bot API 客户端：所有方法可 co_await，统一走 apiCall 入口
class BotClient {
public:
    explicit BotClient(std::string token);

    // 统一入口：method 为 API 方法名，params 作 URL 编码拼到 query。
    // 返回 "result" 字段。ok=false / 不可重试的 HTTP 错误抛 TelegramApiError，
    // 429 与 5xx 自动指数退避重试（最多 3 次）。
    boost::asio::awaitable<nlohmann::json> apiCall(
        const std::string& method,
        std::map<std::string, std::string> params = {});

    boost::asio::awaitable<nlohmann::json> getMe();
    // 长轮询：阻塞至多 timeout 秒，返回 updates 数组（超时为空数组）
    boost::asio::awaitable<nlohmann::json> getUpdates(std::int64_t offset,
                                                      int timeout = 30);
    boost::asio::awaitable<nlohmann::json> sendMessage(
        std::int64_t chatId, const std::string& text);
    // 带 inline/reply 键盘的版本，replyMarkup 形如
    // {{"inline_keyboard", {{{"text", "..."}, {"callback_data", "..."}}}}}
    boost::asio::awaitable<nlohmann::json> sendMessage(
        std::int64_t chatId, const std::string& text,
        const nlohmann::json& replyMarkup);
    boost::asio::awaitable<nlohmann::json> editMessageText(
        std::int64_t chatId, std::int64_t messageId, const std::string& text);
    // 回按钮的"气泡提示"（alert=false 时为轻提示）
    boost::asio::awaitable<void> answerCallbackQuery(
        const std::string& callbackQueryId, const std::string& text = "");
    // 删除消息；失败（已删除/太老/无权限）抛 TelegramApiError
    boost::asio::awaitable<nlohmann::json> deleteMessage(
        std::int64_t chatId, std::int64_t messageId);
    // 查询群成员，返回 ChatMember 对象（"status" 为 creator/administrator/
    // member/restricted/left/kicked）
    boost::asio::awaitable<nlohmann::json> getChatMember(std::int64_t chatId,
                                                         std::int64_t userId);

private:
    std::string token_;
    RateLimiter limiter_;  // 发送限速（单 io_context 线程内无锁）
};

}  // namespace tgbot
