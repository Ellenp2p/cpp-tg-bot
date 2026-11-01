#include <tgbot/panel.hpp>

#include <tgbot/util.hpp>

#include <spdlog/spdlog.h>

#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tgbot {

namespace {

std::string displayName(const nlohmann::json& from) {
    if (from.contains("username") && from["username"].is_string()) {
        const auto name = from["username"].get<std::string>();
        if (!name.empty()) {
            return "@" + name;
        }
    }
    if (from.contains("first_name") && from["first_name"].is_string()) {
        return from["first_name"].get<std::string>();
    }
    return {};
}

bool isNotModified(std::string_view what) {
    return what.find("message is not modified") != std::string_view::npos;
}

}  // namespace

PanelManager::PanelManager(BotClient& bot) : bot_(bot) {}

boost::asio::awaitable<void> PanelManager::edit(std::int64_t chatId,
                                                std::int64_t messageId,
                                                const Panel& panel) {
    const auto view = panel.render();
    SendOptions send;
    send.parseMode = view.parseMode;
    send.disableWebPagePreview = true;
    send.replyMarkup = view.keyboard;
    try {
        co_await bot_.editMessageText(chatId, messageId, view.text, send);
    } catch (const std::exception& e) {
        if (!isNotModified(e.what())) {
            spdlog::warn("panel[{}:{}] 编辑失败: {}", chatId, messageId, e.what());
        }
    }
}

boost::asio::awaitable<std::int64_t> PanelManager::open(
    std::int64_t chatId, std::shared_ptr<Panel> panel) {
    const auto view = panel->render();
    SendOptions send;
    send.parseMode = view.parseMode;
    send.disableWebPagePreview = true;
    send.replyMarkup = view.keyboard;

    const auto msg = co_await bot_.sendMessage(chatId, view.text, send);
    const auto messageId = msg.value("message_id", std::int64_t{0});
    if (messageId != 0) {
        panels_[{chatId, messageId}] = std::move(panel);
    }
    co_return messageId;
}

void PanelManager::attach(std::int64_t chatId, std::int64_t messageId,
                          std::shared_ptr<Panel> panel) {
    if (panel) {
        panels_[{chatId, messageId}] = std::move(panel);
    }
}

boost::asio::awaitable<void> PanelManager::refresh(std::int64_t chatId,
                                                   std::int64_t messageId) {
    const auto it = panels_.find({chatId, messageId});
    if (it != panels_.end()) {
        co_await edit(chatId, messageId, *it->second);
    }
}

boost::asio::awaitable<void> PanelManager::applyEffects(
    const PanelEffects& effects, std::int64_t chatId,
    std::string_view callbackId) {
    for (const auto& pm : effects.privateMessages) {
        EphemeralMessageParameters eph;
        eph.receiverUserId = pm.userId;
        if (!callbackId.empty()) {
            // 有 callback_query_id 时走"被动回复"，只投递到点击的那台设备，
            // 而且不受"bot 必须是群管理员"的限制
            eph.callbackQueryId = std::string{callbackId};
        }
        try {
            co_await bot_.sendEphemeralMessage(chatId, eph, pm.text,
                                               pm.keyboard);
        } catch (const std::exception& e) {
            spdlog::warn("panel: 给 {} 发私密消息失败: {}", pm.userId, e.what());
        }
    }
    for (const auto& text : effects.publicMessages) {
        try {
            co_await bot_.sendMessage(chatId, text);
        } catch (const std::exception& e) {
            spdlog::warn("panel: 发公开消息失败: {}", e.what());
        }
    }
}

boost::asio::awaitable<void> PanelManager::handleCallback(
    nlohmann::json callbackQuery) {
    const auto cqId = callbackQuery.value("id", std::string{});
    const auto data = callbackQuery.value("data", std::string{});

    const auto msgIt = callbackQuery.find("message");
    if (msgIt == callbackQuery.end() || !msgIt->is_object()) {
        // inline 模式来源的回调没有 message，定位不到聊天
        co_await bot_.answerCallbackQuery(cqId, "这个按钮只在群里有效");
        co_return;
    }
    const auto chatId = (*msgIt)["chat"].value("id", std::int64_t{0});
    const auto messageId = msgIt->value("message_id", std::int64_t{0});

    PanelEvent event;
    event.chatId = chatId;
    event.messageId = messageId;
    event.action = std::string{std::string_view{data}.substr(2)};
    event.callback = &callbackQuery;
    if (const auto from = callbackQuery.find("from");
        from != callbackQuery.end() && from->is_object()) {
        event.userId = from->value("id", std::int64_t{0});
        event.userName = displayName(*from);
    }

    const auto it = panels_.find({chatId, messageId});
    if (it == panels_.end()) {
        co_await bot_.answerCallbackQuery(cqId, "这个操作台已失效");
        co_return;
    }

    PanelEffects effects;
    const bool redraw = it->second->onClick(event, effects);

    // 顺序很重要：先应答（客户端立刻停掉转圈），再编辑消息，最后发副作用。
    // 反过来的话，用户会盯着一个还在转的按钮等上整整一个编辑周期。
    co_await bot_.answerCallbackQuery(cqId, effects.answer, effects.showAlert);
    if (redraw) {
        co_await edit(chatId, messageId, *it->second);
    }
    co_await applyEffects(effects, chatId, cqId);

    if (it->second->finished()) {
        panels_.erase(it);
    }
}

boost::asio::awaitable<void> PanelManager::runTicker(
    std::chrono::milliseconds interval) {
    while (true) {
        co_await co_sleep(interval);

        const auto now = std::chrono::system_clock::now();
        std::vector<Key> due;
        for (const auto& [key, panel] : panels_) {
            const auto deadline = panel->deadline();
            if (deadline.has_value() && *deadline <= now && !panel->finished()) {
                due.push_back(key);
            }
        }
        for (const auto& key : due) {
            const auto it = panels_.find(key);
            if (it == panels_.end()) {
                continue;
            }
            PanelEffects effects;
            it->second->onDeadline(effects);
            co_await edit(key.first, key.second, *it->second);
            co_await applyEffects(effects, key.first, {});
            if (it->second->finished()) {
                panels_.erase(it);
            }
        }
    }
}

}  // namespace tgbot
