#pragma once

#include <tgbot/bot_client.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tgbot {

// 事件分发器：注册 handler，由有界队列 + per-chat lane + worker 协程处理
class Dispatcher {
public:
    // handler 收到 (BotClient&, message)。json 必须按值传递：
    // dispatch 在 worker 协程里执行，传引用会在入队/挂起期间悬垂。
    using MessageHandler = std::function<boost::asio::awaitable<void>(BotClient&, nlohmann::json)>;

    void onCommand(std::string command, MessageHandler handler);
    void onAnyMessage(MessageHandler handler);
    // 按 callback_data 前缀注册，最长匹配者优先，如 "menu:"
    void onCallbackQuery(std::string dataPrefix, MessageHandler handler);

    // 机器人自身群成员身份变化（被拉进群 / 被设为管理员 / 被踢 / 主动退出）。
    // handler 收到的是 update 里的 my_chat_member 对象（含 chat / old_chat_member
    // / new_chat_member）。需要在 allowed_updates 里显式订阅 my_chat_member。
    void onMyChatMember(MessageHandler handler);
    // Telegram Stars 支付：pre_checkout_query 顶层 update；successful_payment
    // 在 message 里。两者都需要显式订阅 allowed_updates。
    void onPreCheckout(MessageHandler handler);
    void onSuccessfulPayment(MessageHandler handler);

    // 命令前置守卫：返回 false 时该命令**不会**进入任何 handler（静默丢弃）。
    // 典型用法是"群里只允许管理员用命令"——Telegram 没有服务端权限，必须在
    // 机器人这一层挡；放在这里可以保证不会漏掉某个新注册的命令。
    // msg 按值传入，守卫可以安全地 co_await（不要把它存起来跨调用使用）。
    using CommandGuard = std::function<boost::asio::awaitable<bool>(BotClient&, nlohmann::json)>;
    void setCommandGuard(CommandGuard guard);

    // 本 bot 的 username（getMe 拿到的那个，不带 @）。
    // 设置后，群里 "/sticker@OtherBot on" 这类发给别的 bot 的命令会被忽略；
    // 不设置则保持旧行为（任何 @后缀 都当成本 bot 处理）。
    void setBotUsername(std::string username);

    boost::asio::awaitable<void> dispatch(nlohmann::json update, BotClient& bot);

    // 已入队但尚未处理完的 update 数（含队列中 + 正在执行）。
    // 优雅停机时用它判断"手里还有没有活"；入队 +1、handler 完成 -1。
    void beginUpdate();
    void endUpdate();
    int inFlightUpdates() const;

private:
    std::map<std::string, MessageHandler> commands_;
    std::vector<MessageHandler> anyMessage_;
    std::map<std::string, MessageHandler> callbackQueries_;
    MessageHandler myChatMember_;
    MessageHandler preCheckout_;
    MessageHandler successfulPayment_;
    CommandGuard commandGuard_;
    std::string botUsername_;
    std::atomic<int> inFlight_{0};
};

} // namespace tgbot
