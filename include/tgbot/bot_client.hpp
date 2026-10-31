#pragma once

#include <tgbot/http_client.hpp>
#include <tgbot/rate_limiter.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tgbot {

// Telegram 返回 {"ok": false, ...} 或 HTTP 层错误时抛出
struct TelegramApiError : std::runtime_error {
    long errorCode;
    TelegramApiError(long code, const std::string& description);
};

// Ephemeral Message 参数（Bot API 10.2+）：发送"群里只有指定用户可见"的消息。
// 两种合法用法：
// - 被动回复（所有 bot）：触发动作后 15 秒窗口内提供 callbackQueryId
//   （用户点了 bot 的按钮），消息只投递到触发的那台设备
// - 主动推送（bot 必须是群管理员）：只填 receiverUserId，任意时刻可发给
//   群里任意非 bot 成员，可能投递到其多个在线客户端
// 注意：官方明确不保证送达（离线/切设备/重启可能丢），不能当通知渠道。
// replaceCallbackQueryMessage=true 表示用这条 ephemeral 消息原地替换
// 触发它的那条公开消息（仅对触发者生效；对 ephemeral 消息上的按钮回调
// 必须为 false，那种回调要用 editEphemeralMessage* 系列）
struct EphemeralMessageParameters {
    std::int64_t receiverUserId = 0;
    std::optional<std::string> callbackQueryId;
    std::optional<bool> replaceCallbackQueryMessage;
};

// 命令菜单项。isEphemeral=true 表示 ephemeral command（Bot API 10.2+）：
// 用户把这个命令发到群里时，其他成员（和其他 bot）完全看不到这条命令消息
struct BotCommand {
    std::string command;
    std::string description;
    bool isEphemeral = false;
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
    // 带 inline 键盘的版本，replyMarkup 形如
    // {{"inline_keyboard", json::array({{{{"text", "..."}, {"callback_data", "..."}}}})}}
    // （inline_keyboard 是"行数组的数组"，最内层是按钮对象；
    //   务必用 json::array 明确层级，裸写嵌套大括号极易少一层导致 API 400）
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
    // 发送 ephemeral 消息（群内仅 receiver 可见）。返回 sent Message：
    // 其 message_id 恒为 0，后续编辑/删除用 ephemeral_message_id 标识
    boost::asio::awaitable<nlohmann::json> sendEphemeralMessage(
        std::int64_t chatId, const EphemeralMessageParameters& ephemeral,
        const std::string& text, const nlohmann::json& replyMarkup = nullptr);
    // 编辑 ephemeral 消息的文本（和按钮）。成功返回 true
    boost::asio::awaitable<nlohmann::json> editEphemeralMessageText(
        std::int64_t chatId, std::int64_t receiverUserId,
        std::int64_t ephemeralMessageId, const std::string& text,
        const nlohmann::json& replyMarkup = nullptr);
    // 只改 ephemeral 消息的按钮。成功返回 true
    boost::asio::awaitable<nlohmann::json> editEphemeralMessageReplyMarkup(
        std::int64_t chatId, std::int64_t receiverUserId,
        std::int64_t ephemeralMessageId, const nlohmann::json& replyMarkup);
    // 删除 ephemeral 消息。成功返回 true
    boost::asio::awaitable<nlohmann::json> deleteEphemeralMessage(
        std::int64_t chatId, std::int64_t receiverUserId,
        std::int64_t ephemeralMessageId);
    // 群信息（ChatFullInfo，含默认权限 "permissions"）；静默模式开启前
    // 快照原权限用
    boost::asio::awaitable<nlohmann::json> getChat(std::int64_t chatId);
    // 设置群默认权限（对所有非管理员生效）。需要 bot 有"限制成员"管理员
    // 权限，成功返回 true。permissions 形如 {{"can_send_messages", false}}
    boost::asio::awaitable<nlohmann::json> setChatPermissions(
        std::int64_t chatId, const nlohmann::json& permissions);
    // 成员级权限（覆盖群默认权限）：静默白名单放行/收回。
    // untilDate > 0 为临时限制（Unix 时间），0 为永久
    boost::asio::awaitable<nlohmann::json> restrictChatMember(
        std::int64_t chatId, std::int64_t userId,
        const nlohmann::json& permissions, std::int64_t untilDate = 0);
    // 设置客户端命令菜单（输入框旁 "/" 自动补全列表），启动时调用一次即可。
    // isEphemeral=true 的命令是 ephemeral command（Bot API 10.2+）：用户把它
    // 发到群里时，其他成员（和其他 bot）完全看不到这条命令消息
    boost::asio::awaitable<nlohmann::json> setMyCommands(
        const std::vector<BotCommand>& commands);
    // 注册 webhook 投递地址。secretToken 非空时 Telegram 会带
    // X-Telegram-Bot-Api-Secret-Token 头投递，接收端必须校验。
    // 与 getUpdates 互斥；dropPendingUpdates=true 清空积压 updates
    boost::asio::awaitable<nlohmann::json> setWebhook(
        const std::string& url, const std::string& secretToken = "",
        int maxConnections = 40, bool dropPendingUpdates = true);
    // 取消 webhook，恢复可用 getUpdates 的状态
    boost::asio::awaitable<nlohmann::json> deleteWebhook(
        bool dropPendingUpdates = true);

private:
    std::string token_;
    RateLimiter limiter_;  // 发送限速（单 io_context 线程内无锁）
};

}  // namespace tgbot
