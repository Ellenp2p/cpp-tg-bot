// 群管理机器人示例（基于 tgbot 框架）
//
// 功能：开启后，群里任何人发的贴纸会在 60 秒后被自动删除。
// 控制：群管理员在群里发 /sticker on 开启，/sticker off 关闭；
//       发 /sticker（无参数）查看当前状态；
//       设置持久化到 group_admin_state.db（SQLite），重启不丢。
//
// 用法：./group_admin <bot-token>   或   TG_BOT_TOKEN=... ./group_admin
// 注意：机器人需要被设为群管理员（至少有"删除消息"权限），否则删不掉。

#include <tgbot/tg_bot.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace net = boost::asio;
using nlohmann::json;
using tgbot::BotClient;

namespace {

constexpr auto kStickerLifetime = std::chrono::seconds{60};
constexpr const char* kStateDb = "group_admin_state.db";

// 每个群的开关状态，基于 StateStore 键值存储（SQLite 持久化，重启不丢）
class GroupState {
public:
    explicit GroupState(tgbot::StateStore& store) : store_(store) {}

    bool stickerExpireEnabled(std::int64_t chatId) const {
        return store_.get(key(chatId)).has_value();
    }

    void setStickerExpire(std::int64_t chatId, bool enabled) {
        if (enabled) {
            store_.set(key(chatId), "1");
        } else {
            store_.erase(key(chatId));
        }
    }

private:
    static std::string key(std::int64_t chatId) {
        return "sticker_expire:" + std::to_string(chatId);
    }

    tgbot::StateStore& store_;
};

net::awaitable<bool> isGroupAdmin(BotClient& bot, std::int64_t chatId,
                                  std::int64_t userId) {
    try {
        const auto member = co_await bot.getChatMember(chatId, userId);
        const auto status = member.value("status", std::string{});
        co_return status == "creator" || status == "administrator";
    } catch (const std::exception& e) {
        spdlog::warn("getChatMember failed: {}", e.what());
        co_return false;
    }
}

// 延时删除贴纸。独立协程执行（不占 chat strand），到点删除，
// 失败（已被删/太老/无权限）只记日志不打扰聊天。
void scheduleStickerDelete(BotClient& bot, std::int64_t chatId,
                           std::int64_t messageId,
                           const net::any_io_executor& executor) {
    net::co_spawn(
        executor,
        [&bot, chatId, messageId]() -> net::awaitable<void> {
            co_await tgbot::co_sleep(kStickerLifetime);
            try {
                co_await bot.deleteMessage(chatId, messageId);
                spdlog::info("sticker expired: chat={} message_id={}", chatId,
                             messageId);
            } catch (const std::exception& e) {
                spdlog::warn("delete sticker failed: chat={} message_id={}: {}",
                             chatId, messageId, e.what());
            }
        },
        net::detached);
}

}  // namespace

net::awaitable<void> run(const std::string& token) {
    BotClient bot(token);

    try {
        const auto me = co_await bot.getMe();
        spdlog::info("Connected as @{} (id={})",
                     me.value("username", std::string("?")),
                     me.value("id", 0L));
    } catch (const std::exception& e) {
        spdlog::error("getMe failed: {}", e.what());
        co_return;
    }

    // 设置客户端命令菜单，失败不阻断启动
    try {
        co_await bot.setMyCommands(
            {{"sticker", "开关或查询贴纸自动删除（on/off/无参数）"}});
    } catch (const std::exception& e) {
        spdlog::warn("setMyCommands failed: {}", e.what());
    }

    tgbot::SqliteStateStore store(kStateDb);
    GroupState state(store);

    tgbot::Dispatcher dispatcher;

    // /sticker on|off —— 仅群管理员可用
    dispatcher.onCommand("sticker",
                         [&state](BotClient& bot,
                                  const json msg) -> net::awaitable<void> {
        const auto chatId = msg["chat"].value("id", std::int64_t{0});
        const auto chatType =
            msg["chat"].value("type", std::string{});
        const auto reply = [&bot,
                            chatId](const std::string& text) -> net::awaitable<void> {
            co_await bot.sendMessage(chatId, text);
        };

        if (chatType != "group" && chatType != "supergroup") {
            co_await reply("这个命令只能在群里用：把我拉进群，再发 /sticker on");
            co_return;
        }

        const std::string text = msg.value("text", "");
        const auto sp = text.find(' ');
        const std::string arg = sp == std::string::npos ? "" : text.substr(sp + 1);
        const bool enable = arg == "on";
        if (!enable && arg != "off") {
            // 无参数或参数不认识：显示当前状态 + 用法
            const auto current = state.stickerExpireEnabled(chatId);
            co_await reply(std::string{current ? "当前状态：已开启"
                                                "（贴纸发送 1 分钟后自动删除）"
                                              : "当前状态：已关闭"
                                                "（贴纸不会自动删除）"} +
                           "\n\n用法：/sticker on 开启，/sticker off 关闭"
                           "（仅群管理员）");
            co_return;
        }

        const auto userId = msg["from"].value("id", std::int64_t{0});
        if (!co_await isGroupAdmin(bot, chatId, userId)) {
            co_await reply("只有群管理员可以开关这个功能。");
            co_return;
        }

        state.setStickerExpire(chatId, enable);
        co_await reply(enable ? "已开启：本群贴纸将在发送 1 分钟后自动删除。"
                                "（需要给我删除消息的权限）"
                              : "已关闭：贴纸不再自动删除。");
    });

    // 贴纸过期：开启的群里，贴纸发出 60 秒后删除
    dispatcher.onAnyMessage(
        [&state](BotClient& bot, const json msg) -> net::awaitable<void> {
            if (!msg.contains("sticker")) {
                co_return;
            }
            const auto chatId = msg["chat"].value("id", std::int64_t{0});
            if (!state.stickerExpireEnabled(chatId)) {
                co_return;
            }
            const auto messageId = msg.value("message_id", std::int64_t{0});
            const auto executor = co_await net::this_coro::executor;
            scheduleStickerDelete(bot, chatId, messageId, executor);
        });

    spdlog::info("Long polling started, Ctrl+C to stop");
    co_await tgbot::longPollLoop(bot, dispatcher);
}

int main(int argc, char** argv) {
    std::string token;
    if (argc > 1) {
        token = argv[1];
    } else if (const char* env = std::getenv("TG_BOT_TOKEN")) {
        token = env;
    }
    if (token.empty()) {
        std::cerr << "usage: group_admin <bot-token>\n"
                     "   or: set TG_BOT_TOKEN environment variable\n";
        return 1;
    }

    net::io_context io;

    // Ctrl+C / SIGTERM 优雅退出
    net::signal_set signals(io, SIGINT, SIGTERM);
#ifdef SIGBREAK  // Windows 下控制台关闭事件
    signals.add(SIGBREAK);
#endif
    signals.async_wait([&io](const boost::system::error_code& ec, int sig) {
        if (!ec) {
            spdlog::info("received signal {}, shutting down...", sig);
            io.stop();
        }
    });

    net::co_spawn(io, run(token), [&io](std::exception_ptr e) {
        if (e) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& ex) {
                spdlog::error("fatal: {}", ex.what());
            }
        }
        io.stop();
    });
    io.run();
    spdlog::info("bye");
    return 0;
}
