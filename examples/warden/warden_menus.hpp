#pragma once
#include "warden_types.hpp"
#include "commands.hpp"
#include <unordered_set>
#include "store.hpp"
namespace warden {
inline std::vector<tgbot::BotCommand> groupAdminCommands() {
    // 群菜单完全由 commands.hpp 的 groupMenu/ephemeralCommand 决定
    std::vector<tgbot::BotCommand> out;
    for (const auto& policy : kCommandPolicies) {
        if (policy.groupMenu) {
            out.push_back({std::string{policy.name},
                           std::string{policy.description},
                           policy.ephemeralCommand});
        }
    }
    return out;
}

inline std::vector<tgbot::BotCommand> privateCommands() {
    // 私聊菜单由 privateMenu 决定；私聊命令不需要 ephemeral
    std::vector<tgbot::BotCommand> out;
    for (const auto& policy : kCommandPolicies) {
        if (policy.privateMenu) {
            out.push_back({std::string{policy.name},
                           std::string{policy.description}, false});
        }
    }
    return out;
}

// 按 chat_administrators 作用域给单个群发布命令菜单。
// 非管理员在群里看不到任何命令（all_group_chats 被清空），只有群管理员能看到；
// 每个群一次 setMyCommands，进程内去重，失败下次再试。
class GroupMenuPublisher {
public:
    explicit GroupMenuPublisher(std::vector<tgbot::BotCommand> commands,
                                bool enabled = true)
        : commands_(std::move(commands)), enabled_(enabled) {}

    bool enabled() const { return enabled_; }

    // 机器人退群/被踢：允许以后重新进群时再发一次菜单，同时回收 set 条目。
    void forget(std::int64_t chatId) { done_.erase(chatId); }

    net::awaitable<void> publish(BotClient& bot, std::int64_t chatId) {
        if (!enabled_ || done_.find(chatId) != done_.end()) {
            co_return;
        }
        if (done_.size() > 20000) {
            done_.clear();  // 极端多群时允许重发，换取有界内存
        }
        done_.insert(chatId);
        const json scope = {{"type", "chat_administrators"},
                            {"chat_id", chatId}};
        try {
            co_await bot.setMyCommands(commands_, scope);
            spdlog::debug("group admin menu published: chat={}", chatId);
        } catch (const std::exception& e) {
            // 机器人不是管理员 / 网络问题：不标记完成，下次命令或下次启动再试
            done_.erase(chatId);
            spdlog::debug("publish group admin menu failed: chat={}: {}", chatId,
                          e.what());
        }
    }

private:
    std::vector<tgbot::BotCommand> commands_;
    bool enabled_ = true;
    std::unordered_set<std::int64_t> done_;
};

// 启动时给已知的、机器人是管理员的群补发菜单。串行 + 100ms 间隔，
// 避免几百个群同时打 setMyCommands 触发限速。
inline net::awaitable<void> publishAllGroupMenus(BotClient& bot, Store& store,
                                          GroupMenuPublisher& menus) {
    if (!menus.enabled()) {
        spdlog::info("group admin command menus disabled "
                     "(WARDEN_GROUP_MENUS=false)");
        co_return;
    }
    constexpr int kPage = 200;
    for (int offset = 0;; offset += kPage) {
        const auto chats = store.listChats(kPage, offset, true);
        if (chats.empty()) {
            break;
        }
        for (const auto& chat : chats) {
            if (!chat.isAdmin()) {
                continue;
            }
            co_await menus.publish(bot, chat.chatId);
            co_await tgbot::co_sleep(std::chrono::milliseconds{100});
        }
        if (static_cast<int>(chats.size()) < kPage) {
            break;
        }
    }
    spdlog::info("group admin command menus published");
}


}
