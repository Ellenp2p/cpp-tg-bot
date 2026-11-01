#pragma once

#include <tgbot/bot_client.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tgbot {

// 一次按钮点击
struct PanelEvent {
    std::int64_t chatId = 0;
    std::int64_t messageId = 0;
    std::int64_t userId = 0;
    std::string userName;  // @username 或显示名，可能为空
    std::string action;    // callback_data 里 "p:" 之后的部分
    const nlohmann::json* callback = nullptr;  // 原始 callback_query
};

// 面板处理完一次点击后要产生的副作用。
// 面板本身不碰网络，只往这里"下单"，由 PanelManager 统一执行——
// 这样业务逻辑可以被单测，也保证"先应答、再编辑"的顺序不会写错。
struct PanelEffects {
    std::string answer;      // answerCallbackQuery 的提示气泡
    bool showAlert = false;  // true = 弹窗，false = 轻提示

    struct Private {
        std::int64_t userId = 0;
        std::string text;                  // 建议用 HTML（随 PanelView 的 parse_mode）
        nlohmann::json keyboard = nullptr;
    };
    std::vector<Private> privateMessages;    // 只有该用户可见（ephemeral）
    std::vector<std::string> publicMessages; // 额外发到群里的公开消息
};

// 一帧渲染结果
struct PanelView {
    std::string text;
    ParseMode parseMode = ParseMode::kHtml;
    nlohmann::json keyboard = nullptr;  // inline_keyboard
};

// 操作台抽象：一条消息 + 一个键盘 + 一份状态。
// 交互模型固定为：点击 → onClick 改状态 → 返回 true 则重渲染并编辑同一条消息。
// 没有 I/O、没有协程，纯逻辑，方便测试和复用。
class Panel {
public:
    virtual ~Panel() = default;

    // 类型名（日志/未来持久化用）
    virtual std::string_view type() const = 0;

    // 当前状态对应的渲染结果
    virtual PanelView render() const = 0;

    // 处理按钮点击。返回 true 表示状态已变、需要重渲染
    virtual bool onClick(const PanelEvent& event, PanelEffects& effects) = 0;

    // 可选：定时到期（抽奖倒计时这类）
    virtual std::optional<std::chrono::system_clock::time_point> deadline() const {
        return std::nullopt;
    }
    virtual void onDeadline(PanelEffects& effects) { (void)effects; }

    // 结束后是否可以从管理表里回收
    virtual bool finished() const { return false; }
};

// 操作台管理器：把 Panel 挂到一条消息上，路由回调、刷新消息、跑定时器。
// callback_data 约定：以 "p:" 开头，后面是 action（不含 ':'）
class PanelManager {
public:
    explicit PanelManager(BotClient& bot);

    // 发一条新的操作台消息，返回 message_id
    boost::asio::awaitable<std::int64_t> open(std::int64_t chatId,
                                              std::shared_ptr<Panel> panel);

    // 把已经存在的消息挂到操作台上
    void attach(std::int64_t chatId, std::int64_t messageId,
                std::shared_ptr<Panel> panel);

    // 处理 callback_query（由 Dispatcher 按 "p:" 前缀转进来）
    boost::asio::awaitable<void> handleCallback(nlohmann::json callbackQuery);

    static bool matches(std::string_view data) {
        return data.size() > 2 && data.rfind("p:", 0) == 0;
    }
    static std::string callbackData(std::string_view action) {
        return "p:" + std::string{action};
    }

    // 后台定时器：检查 deadline 并触发 onDeadline，然后刷新消息
    boost::asio::awaitable<void> runTicker(
        std::chrono::milliseconds interval = std::chrono::seconds{1});

    // 主动刷新某条面板消息
    boost::asio::awaitable<void> refresh(std::int64_t chatId,
                                         std::int64_t messageId);

    std::size_t size() const { return panels_.size(); }

private:
    using Key = std::pair<std::int64_t, std::int64_t>;  // chatId, messageId

    boost::asio::awaitable<void> edit(std::int64_t chatId, std::int64_t messageId,
                                      const Panel& panel);
    boost::asio::awaitable<void> applyEffects(const PanelEffects& effects,
                                              std::int64_t chatId,
                                              std::string_view callbackId);

    BotClient& bot_;
    std::map<Key, std::shared_ptr<Panel>> panels_;
};

}  // namespace tgbot
