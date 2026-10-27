#include <tgbot/bot_client.hpp>

#include <boost/asio/this_coro.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <cctype>
#include <utility>

namespace tgbot {

namespace net = boost::asio;
using nlohmann::json;

TelegramApiError::TelegramApiError(long code, const std::string& description)
    : std::runtime_error("Telegram API error " + std::to_string(code) + ": " +
                         description),
      errorCode(code) {}

namespace {

std::string urlEncode(std::string_view in) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (const unsigned char c : in) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

}  // namespace

BotClient::BotClient(std::string token) : token_(std::move(token)) {}

net::awaitable<json> BotClient::apiCall(
    const std::string& method, std::map<std::string, std::string> params) {
    std::string target = "/bot" + token_ + "/" + method;
    if (!params.empty()) {
        target += '?';
        bool first = true;
        for (const auto& [key, value] : params) {
            if (!first) target += '&';
            first = false;
            target += urlEncode(key);
            target += '=';
            target += urlEncode(value);
        }
    }

    constexpr int kMaxRetries = 3;
    for (int attempt = 1;; ++attempt) {
        long retryAfterSec = 0;
        try {
            const auto resp = co_await httpsGet("api.telegram.org", target);
            if (resp.status == 200) {
                const auto j = json::parse(resp.body);
                if (!j.is_object() || !j.value("ok", false)) { spdlog::error("apiCall {} status={} body: {}", method, resp.status, resp.body); }
                if (j.value("ok", false)) {
                    co_return j["result"];
                }
                throw TelegramApiError(j.value("error_code", 0L),
                                       method + ": " +
                                           j.value("description", "unknown"));
            }
            // 429 时优先遵循 Telegram 返回的 retry_after，而不是自己的退避
            if (resp.status == 429) {
                try {
                    const auto j = json::parse(resp.body);
                    retryAfterSec =
                        j.value("parameters", json::object())
                            .value("retry_after", 0L);
                } catch (const std::exception&) {
                    // 响应体不是 JSON，忽略
                }
            }
            const bool retryable = (resp.status == 429 || resp.status >= 500);
            if (!retryable || attempt > kMaxRetries) {
                throw TelegramApiError(
                    resp.status, method + ": HTTP " +
                                     std::to_string(resp.status));
            }
        } catch (const boost::system::system_error& e) {
            if (attempt > kMaxRetries) {
                throw std::runtime_error(method +
                                         ": network error: " + e.what());
            }
        }

        auto delay = std::chrono::seconds{1 << (attempt - 1)};
        if (retryAfterSec > 0) {
            delay = std::chrono::seconds{retryAfterSec + 1};
        }
        spdlog::warn("apiCall {} failed (attempt {}), retry in {}s", method,
                     attempt, delay.count());
        net::steady_timer timer(co_await net::this_coro::executor, delay);
        co_await timer.async_wait(net::use_awaitable);
    }
}

net::awaitable<json> BotClient::getMe() { co_return co_await apiCall("getMe"); }

net::awaitable<json> BotClient::getUpdates(std::int64_t offset, int timeout) {
    co_return co_await apiCall(
        "getUpdates",
        {{"offset", std::to_string(offset)},
         {"timeout", std::to_string(timeout)},
         // 必须显式指定：官方文档说明不传 allowed_updates 会沿用
         // 服务器上一次的设置，残留配置可能把 callback_query 过滤掉，
         // 表现为"按钮点了毫无反应"
         {"allowed_updates", R"(["message","callback_query"])"}});
}

net::awaitable<json> BotClient::sendMessage(std::int64_t chatId,
                                            const std::string& text) {
    co_await limiter_.beforeSend(chatId);
    co_return co_await apiCall(
        "sendMessage", {{"chat_id", std::to_string(chatId)}, {"text", text}});
}

net::awaitable<json> BotClient::sendMessage(std::int64_t chatId,
                                            const std::string& text,
                                            const json& replyMarkup) {
    co_await limiter_.beforeSend(chatId);
    co_return co_await apiCall("sendMessage",
                               {{"chat_id", std::to_string(chatId)},
                                {"text", text},
                                {"reply_markup", replyMarkup.dump()}});
}

net::awaitable<json> BotClient::editMessageText(std::int64_t chatId,
                                                std::int64_t messageId,
                                                const std::string& text) {
    co_await limiter_.beforeSend(chatId);
    co_return co_await apiCall("editMessageText",
                               {{"chat_id", std::to_string(chatId)},
                                {"message_id", std::to_string(messageId)},
                                {"text", text}});
}

net::awaitable<void> BotClient::answerCallbackQuery(
    const std::string& callbackQueryId, const std::string& text) {
    std::map<std::string, std::string> params{
        {"callback_query_id", callbackQueryId}};
    if (!text.empty()) {
        params.emplace("text", text);
    }
    co_await apiCall("answerCallbackQuery", std::move(params));
}


net::awaitable<json> BotClient::deleteMessage(std::int64_t chatId,
                                              std::int64_t messageId) {
    co_await limiter_.beforeSend(chatId);
    co_return co_await apiCall("deleteMessage",
                               {{"chat_id", std::to_string(chatId)},
                                {"message_id", std::to_string(messageId)}});
}

net::awaitable<json> BotClient::getChatMember(std::int64_t chatId,
                                              std::int64_t userId) {
    // 管理类查询不走发送限速
    co_return co_await apiCall("getChatMember",
                               {{"chat_id", std::to_string(chatId)},
                                {"user_id", std::to_string(userId)}});
}

net::awaitable<json> BotClient::setMyCommands(
    std::vector<std::pair<std::string, std::string>> commands) {
    json arr = json::array();
    for (const auto& [cmd, desc] : commands) {
        arr.push_back({{"command", cmd}, {"description", desc}});
    }
    co_return co_await apiCall("setMyCommands", {{"commands", arr.dump()}});
}

}  // namespace tgbot
