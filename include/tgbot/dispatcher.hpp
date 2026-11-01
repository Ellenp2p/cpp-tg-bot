#pragma once

#include <tgbot/bot_client.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tgbot {

// 事件分发器：注册 handler，每个 update 由独立的协程处理
class Dispatcher {
public:
    // handler 收到 (BotClient&, message)。json 必须按值传递：
    // dispatch 由 co_spawn 出的协程执行，传引用会在挂起期间悬垂
    // （长轮询循环的 updates 数组销毁后，handler 仍可能活着）
    using MessageHandler = std::function<boost::asio::awaitable<void>(
        BotClient&, nlohmann::json)>;

    void onCommand(std::string command, MessageHandler handler);
    void onAnyMessage(MessageHandler handler);
    // 按 callback_data 前缀注册，最长匹配者优先，如 "menu:"
    void onCallbackQuery(std::string dataPrefix, MessageHandler handler);

    // 本 bot 的 username（getMe 拿到的那个，不带 @）。
    // 设置后，群里 "/sticker@OtherBot on" 这类发给别的 bot 的命令会被忽略；
    // 不设置则保持旧行为（任何 @后缀 都当成本 bot 处理）。
    void setBotUsername(std::string username);

    boost::asio::awaitable<void> dispatch(nlohmann::json update,
                                          BotClient& bot);

private:
    std::map<std::string, MessageHandler> commands_;
    std::vector<MessageHandler> anyMessage_;
    std::map<std::string, MessageHandler> callbackQueries_;
    std::string botUsername_;
};

}  // namespace tgbot
