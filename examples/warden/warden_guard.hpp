#pragma once
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include "commands.hpp"
#include "replies.hpp"
#include "scheduler.hpp"
#include "store.hpp"
#include "warden_menus.hpp"
#include "warden_types.hpp"
#include "warden_util.hpp"
namespace warden {
inline net::awaitable<bool> groupCommandGuard(BotClient& bot,
                                              Store& store,
                                              DeleteScheduler& sched,
                                              const std::vector<std::int64_t>& owners,
                                              GroupMenuPublisher& menus,
                                              const json& msg) {
    const auto type = chatTypeOf(msg);
    const auto name = commandNameFromMessage(msg);
    const auto* policy = findCommandPolicy(name);
    if (policy == nullptr) {
        spdlog::warn("no command policy for /{}", name);
        co_return false;
    }

    const auto chatId = chatIdOf(msg);
    const auto userId = senderId(msg);
    const bool owner = isOwnerId(owners, userId);

    // 私聊：只做 privateAllowed / ownerOnly 检查
    if (type == "private") {
        if (!policy->privateAllowed) {
            if (chatId != 0) {
                co_await bot.sendMessage(chatId, "这个命令只能在群里用。");
            }
            co_return false;
        }
        if (policy->ownerOnly && !owner) {
            if (chatId != 0) {
                co_await bot.sendMessage(chatId, "仅机器人拥有者可用。");
            }
            co_return false;
        }
        co_return true;
    }
    if (!isGroupType(type)) {
        co_return false; // 频道等场景不提供命令
    }

    // 明确 @ 本 bot 的注册命令始终删；没写 @ 的看 deleteBareCommand。
    // 这里必须 co_await：detached 协程会排在 handler 后面，出问题很难查。
    if (commandWasMentioned(msg) || policy->deleteBareCommand) {
        co_await sweepGroupCommand(bot, msg);
    }

    if (chatId == 0 || userId == 0) {
        co_return false;
    }

    if (!policy->groupAllowed) {
        if (policy->groupDeny == GroupDeny::kRedirect) {
            co_await replyEphemeral(bot, sched, msg, "请私聊我使用 /" + name);
        }
        co_return false;
    }

    if (policy->ownerOnly && !owner) {
        co_return false;
    }

    // 拥有者是最高管理员：不查 getChatMember，直接放行并补登记群
    if (owner) {
        store.ensureChat(chatId,
                         type,
                         msg.value("chat", json::object()).value("title", std::string{}),
                         nowSec());
        spdlog::debug("owner override in group: chat={} user={}", chatId, userId);
        co_await menus.publish(bot, chatId);
        co_return true;
    }

    if (!policy->groupAdminOnly) {
        co_return true;
    }

    const auto status = co_await fetchMemberStatus(bot, chatId, userId);
    if (!status.has_value()) {
        co_await replyEphemeral(
            bot, sched, msg, "查询你的群身份失败（网络或权限问题），请稍后重试。");
        co_return false;
    }
    if (!isAdminStatus(*status)) {
        spdlog::debug("denied group command from non-admin: chat={} user={}", chatId, userId);
        co_return false;
    }

    store.addAdmin(
        chatId, userId, *status, displayName(msg.value("from", json::object())), nowSec());
    store.ensureChat(
        chatId, type, msg.value("chat", json::object()).value("title", std::string{}), nowSec());
    co_await menus.publish(bot, chatId);
    co_return true;
}

} // namespace warden
