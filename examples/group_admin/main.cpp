// 群管理机器人示例（基于 tgbot 框架）
//
// 功能：开启后，群里任何人发的贴纸会在设定时间后自动删除（默认 60 秒）。
// 控制（仅群管理员）：/sticker on 开启，/sticker off 关闭，
//       /sticker delay <秒> 设置删除时限（5–3600 秒）；
//       /sticker 或 /sticker status（任何人）查看状态面板：
//       功能开关、删除时限、机器人在群里的权限。
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

constexpr auto kDefaultStickerDelay = std::chrono::seconds{60};
constexpr long kMinDelaySec = 5;
constexpr long kMaxDelaySec = 3600;
constexpr const char* kStateDb = "group_admin_state.db";

// 每个群的贴纸设置（开关 + 删除时限），基于 StateStore 键值存储
// （SQLite 持久化，重启不丢）
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

    // 删除时限；未设置过返回默认 60 秒，数据异常也回落默认
    std::chrono::seconds stickerDelay(std::int64_t chatId) const {
        const auto v = store_.get(delayKey(chatId));
        if (v) {
            try {
                return std::chrono::seconds{std::stol(*v)};
            } catch (const std::exception&) {
                // 落默认
            }
        }
        return kDefaultStickerDelay;
    }

    void setStickerDelay(std::int64_t chatId, long seconds) {
        store_.set(delayKey(chatId), std::to_string(seconds));
    }

private:
    static std::string key(std::int64_t chatId) {
        return "sticker_expire:" + std::to_string(chatId);
    }
    static std::string delayKey(std::int64_t chatId) {
        return "sticker_delay:" + std::to_string(chatId);
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
                           std::chrono::seconds delay,
                           const net::any_io_executor& executor) {
    net::co_spawn(
        executor,
        [&bot, chatId, messageId, delay]() -> net::awaitable<void> {
            co_await tgbot::co_sleep(delay);
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

    std::int64_t botId = 0;
    try {
        const auto me = co_await bot.getMe();
        botId = me.value("id", std::int64_t{0});
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
            {{"sticker", "贴纸自动删除：on/off/delay/无参数查状态"}});
    } catch (const std::exception& e) {
        spdlog::warn("setMyCommands failed: {}", e.what());
    }

    tgbot::SqliteStateStore store(kStateDb);
    GroupState state(store);

    tgbot::Dispatcher dispatcher;

    // /sticker —— 无参数：状态面板（任何人可用）；on/off/delay：仅群管理员
    dispatcher.onCommand("sticker",
                         [&state, botId](BotClient& bot,
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

        // ---- 无参数 / status：状态面板（任何人可用）----
        if (arg.empty() || arg == "status") {
            const bool enabled = state.stickerExpireEnabled(chatId);
            const auto delay = state.stickerDelay(chatId);

            // 查机器人在本群的权限（能否删除消息的关键）
            std::string botRole = "查询失败";
            try {
                const auto m = co_await bot.getChatMember(chatId, botId);
                const auto st = m.value("status", std::string{});
                if (st == "creator") {
                    botRole = "群主 ✅";
                } else if (st == "administrator") {
                    botRole = "管理员 ✅";
                } else if (st == "member") {
                    botRole = "普通成员 ⚠️（无删除权限，请给我管理员）";
                } else if (st == "left" || st == "kicked") {
                    botRole = "不在群里 ⚠️";
                } else {
                    botRole = st;
                }
            } catch (const std::exception&) {
                // 保持"查询失败"
            }

            co_await reply(
                std::string{enabled ? "已开启，贴纸 " +
                                          std::to_string(delay.count()) +
                                          " 秒后删除"
                                    : "已关闭，贴纸不会自动删除"} +
                "\n我的权限：" + botRole +
                "\n管理员：/sticker on|off|delay <秒>");
            co_return;
        }

        // ---- 以下变更操作仅群管理员可用 ----
        const auto userId = msg["from"].value("id", std::int64_t{0});
        if (!co_await isGroupAdmin(bot, chatId, userId)) {
            co_await reply("只有群管理员可以修改设置。（发 /sticker 可查看状态）");
            co_return;
        }

        if (arg == "on") {
            state.setStickerExpire(chatId, true);
            co_await reply("已开启：本群贴纸将在发送 " +
                           std::to_string(state.stickerDelay(chatId).count()) +
                           " 秒后自动删除。（需要给我删除消息的权限）");
            co_return;
        }
        if (arg == "off") {
            state.setStickerExpire(chatId, false);
            co_await reply("已关闭：贴纸不再自动删除。");
            co_return;
        }

        // /sticker delay <秒>
        if (arg.rfind("delay", 0) == 0) {
            const auto dsp = arg.find(' ');
            long sec = 0;
            if (dsp != std::string::npos) {
                try {
                    sec = std::stol(arg.substr(dsp + 1));
                } catch (const std::exception&) {
                    sec = 0;
                }
            }
            if (sec < kMinDelaySec || sec > kMaxDelaySec) {
                co_await reply("用法：/sticker delay <秒>，时限范围 " +
                               std::to_string(kMinDelaySec) + "–" +
                               std::to_string(kMaxDelaySec) +
                               " 秒，例如 /sticker delay 30");
                co_return;
            }
            state.setStickerDelay(chatId, sec);
            co_await reply("已设置：贴纸将在发送 " + std::to_string(sec) +
                           " 秒后自动删除。");
            co_return;
        }

        co_await reply("不认识的参数。用法：/sticker on|off|delay <秒>，"
                       "或 /sticker 查看状态");
    });

    // 贴纸过期：开启的群里，贴纸发出设定时间后删除
    dispatcher.onAnyMessage(
        [&state](BotClient& bot, const json msg) -> net::awaitable<void> {
            if (!msg.contains("sticker")) {
                co_return;
            }
            const auto chatId = msg["chat"].value("id", std::int64_t{0});
            if (!state.stickerExpireEnabled(chatId)) {
                co_return;
            }
            const auto delay = state.stickerDelay(chatId);
            const auto messageId = msg.value("message_id", std::int64_t{0});
            spdlog::info(
                "sticker received: chat={} message_id={}, will delete in {}s",
                chatId, messageId, delay.count());
            const auto executor = co_await net::this_coro::executor;
            scheduleStickerDelete(bot, chatId, messageId, delay, executor);
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
