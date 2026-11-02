#include <tgbot/dispatcher.hpp>
#include <tgbot/metrics.hpp>

#include <spdlog/spdlog.h>

#include <cctype>
#include <exception>
#include <string_view>
#include <utility>

namespace tgbot {

using nlohmann::json;

namespace {

// Telegram 的 username 大小写不敏感
bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

} // namespace

void Dispatcher::onCommand(std::string command, MessageHandler handler) {
    const std::string name = command;
    if (!commands_.emplace(std::move(command), std::move(handler)).second) {
        spdlog::warn("onCommand: /{} 重复注册，后注册的处理器被忽略", name);
    }
}

void Dispatcher::onAnyMessage(MessageHandler handler) {
    anyMessage_.push_back(std::move(handler));
}

void Dispatcher::onCallbackQuery(std::string dataPrefix, MessageHandler handler) {
    callbackQueries_.emplace(std::move(dataPrefix), std::move(handler));
}

void Dispatcher::onMyChatMember(MessageHandler handler) {
    myChatMember_ = std::move(handler);
}

void Dispatcher::setCommandGuard(CommandGuard guard) {
    commandGuard_ = std::move(guard);
}

void Dispatcher::beginUpdate() {
    inFlight_.fetch_add(1);
}

void Dispatcher::endUpdate() {
    inFlight_.fetch_sub(1);
}

int Dispatcher::inFlightUpdates() const {
    return inFlight_.load();
}

void Dispatcher::setBotUsername(std::string username) {
    if (!username.empty() && username.front() == '@') {
        username.erase(username.begin());
    }
    botUsername_ = std::move(username);
}

boost::asio::awaitable<void> Dispatcher::dispatch(json update, BotClient& bot) {
    // 机器人自身成员变化（被拉群/被提权/被踢）：与 message 同级，先处理
    if (const auto mcmIt = update.find("my_chat_member"); mcmIt != update.end()) {
        if (myChatMember_) {
            json mcm = *mcmIt;
            try {
                co_await myChatMember_(bot, std::move(mcm));
            } catch (const std::exception& e) {
                spdlog::error("my_chat_member handler failed: {}", e.what());
            }
        }
        co_return;
    }

    // 回调查询：按 callback_data 最长前缀匹配；cq 按值持有，避免悬垂
    if (const auto cqIt = update.find("callback_query"); cqIt != update.end()) {
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
                spdlog::error("callback handler for '{}' failed: {}", bestPrefix, e.what());
            }
        } else {
            spdlog::warn("unhandled callback_query, data='{}'", data);
        }
        co_return;
    }

    const auto msgIt = update.find("message");
    if (msgIt == update.end()) {
        co_return; // 其他 update 类型（edited_message 等）后续按需添加
    }
    json msg = *msgIt; // 按值持有（命令带 @后缀 时需归一化文本）
    const std::string text = msg.value("text", "");

    // 命令消息："/start" 或 "/start@YourBot" 形式
    if (!text.empty() && text.front() == '/') {
        const auto space = text.find(' ');
        std::string command =
            text.substr(1, space == std::string::npos ? std::string::npos : space - 1);
        std::size_t suffixAt = std::string::npos;
        std::string suffix;
        if (const auto at = command.find('@'); at != std::string::npos) {
            suffix = command.substr(at); // 含 '@'
            command.erase(at);
            suffixAt = at;
            // 群里可能有多个 bot："/x@OtherBot" 不是给我们的，直接忽略，
            // 否则每个 bot 都会抢答一遍
            if (!botUsername_.empty() &&
                !iequals(std::string_view{suffix}.substr(1), botUsername_)) {
                co_return;
            }
        }
        if (const auto it = commands_.find(command); it != commands_.end()) {
            metrics::command(command);
            if (suffixAt != std::string::npos) {
                // 保留“这条命令明确 @ 了本 bot”的标记，供 handler/guard
                // 区分“/cmd”和“/cmd@ThisBot”。
                msg["_tgbot_command_mentioned"] = true;
                // 归一化文本：去掉命令里的 @BotName，
                // handler 侧解析参数时无需关心这种形式
                msg["text"].get_ref<std::string&>().erase(1 + suffixAt, suffix.size());
            }
            if (commandGuard_) {
                bool allowed = false;
                try {
                    allowed = co_await commandGuard_(bot, msg);
                } catch (const std::exception& e) {
                    // 守卫自己炸了：按拒绝处理（fail closed），绝不因为
                    // "查不到权限"就把管理命令放进来
                    spdlog::error("command guard for /{} failed: {}", command, e.what());
                    co_return;
                }
                if (!allowed) {
                    co_return;
                }
            }
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

} // namespace tgbot
