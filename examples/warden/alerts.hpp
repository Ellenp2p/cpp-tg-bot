#pragma once

// 告警出口：同一段文本同时投递到多个渠道（Telegram owner 私聊 / 飞书 /
// Discord）。渠道是"发了就忘"的：任何渠道失败只记日志，不影响删除任务，
// 也不阻塞调用方。
//
// 以后接新渠道只需要加一个 makeXxxSink，不需要改 scheduler/业务代码。

#include <tgbot/bot_client.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace warden {

struct AlertSink {
    std::string name;
    std::function<boost::asio::awaitable<void>(const std::string&)> send;
};

class AlertDispatcher {
public:
    explicit AlertDispatcher(boost::asio::any_io_executor executor);

    void add(AlertSink sink);
    // 异步投递：每个渠道独立 co_spawn，失败只写日志。调用方立即返回。
    void publish(std::string text);

private:
    boost::asio::any_io_executor executor_;
    std::vector<AlertSink> sinks_;
};

// Telegram：逐个 owner 私聊（受 BotClient 发送限速约束）
AlertSink makeOwnerDmSink(tgbot::BotClient& bot,
                          std::vector<std::int64_t> owners);

// 飞书自定义机器人 webhook。secret 非空时按飞书签名规范带 timestamp/sign。
AlertSink makeFeishuSink(std::string webhookUrl, std::string secret);

// Discord webhook。只发 content 文本，超长会自动截断到 1900 字符。
AlertSink makeDiscordSink(std::string webhookUrl);

}  // namespace warden
