#include <tgbot/dispatcher.hpp>

#include <spdlog/spdlog.h>

#include <exception>
#include <utility>

namespace tgbot {

using nlohmann::json;

void Dispatcher::onCommand(std::string command, MessageHandler handler) {
    commands_.emplace(std::move(command), std::move(handler));
}

void Dispatcher::onAnyMessage(MessageHandler handler) {
    anyMessage_.push_back(std::move(handler));
}

void Dispatcher::onCallbackQuery(std::string dataPrefix,
                                 MessageHandler handler) {
    callbackQueries_.emplace(std::move(dataPrefix), std::move(handler));
}

boost::asio::awaitable<void> Dispatcher::dispatch(json update, BotClient& bot) {
    // 回调查询：按 callback_data 最长前缀匹配；cq 按值持有，避免悬垂
    if (const auto cqIt = update.find("callback_query");
        cqIt != update.end()) {
        const json cq = *cqIt;
        const std::string data = cq.value("data", "");
        const MessageHandler* best = nullptr;
        std::string bestPrefix;
        for (const auto& [prefix, handler] : callbackQueries_) {
            if (data.rfind(prefix, 0) == 0 && prefix.size() > bestPrefix.size()) {
                bestPrefix = prefix;
                best = &handler;
            }
        }
        if (best != nullptr) {
            try {
                co_await (*best)(bot, cq);
            } catch (const std::exception& e) {
                spdlog::error("callback handler for '{}' failed: {}",
                              bestPrefix, e.what());
            }
        } else {
            spdlog::warn("unhandled callback_query, data='{}'", data);
        }
        co_return;
    }

    const auto msgIt = update.find("message");
    if (msgIt == update.end()) {
        co_return;  // 其他 update 类型（edited_message 等）后续按需添加
    }
    const json msg = *msgIt;  // 按值持有
    const std::string text = msg.value("text", "");

    // 命令消息："/start" 或 "/start@YourBot" 形式
    if (!text.empty() && text.front() == '/') {
        const auto space = text.find(' ');
        std::string command =
            text.substr(1, space == std::string::npos ? std::string::npos
                                                      : space - 1);
        if (const auto at = command.find('@');
            at != std::string::npos) {
            command.erase(at);
        }
        if (const auto it = commands_.find(command); it != commands_.end()) {
            try {
                co_await it->second(bot, msg);
            } catch (const std::exception& e) {
                spdlog::error("handler for /{} failed: {}", command, e.what());
            }
            co_return;
        }
    }

    for (const auto& handler : anyMessage_) {
        try {
            co_await handler(bot, msg);
        } catch (const std::exception& e) {
            spdlog::error("message handler failed: {}", e.what());
        }
    }
}

}  // namespace tgbot
