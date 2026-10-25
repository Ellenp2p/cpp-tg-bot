#include "bot_client.hpp"
#include "dispatcher.hpp"
#include "long_poll.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <spdlog/spdlog.h>

#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace net = boost::asio;
using nlohmann::json;

net::awaitable<void> run(const std::string& token) {
    tb::BotClient bot(token);

    try {
        const auto me = co_await bot.getMe();
        spdlog::info("Connected as @{} (id={})",
                     me.value("username", std::string("?")),
                     me.value("id", 0L));
    } catch (const std::exception& e) {
        spdlog::error("getMe failed: {}", e.what());
        co_return;
    }

    tb::Dispatcher dispatcher;

    dispatcher.onCommand("start", [](tb::BotClient& bot,
                                     const json msg) -> net::awaitable<void> {
        const auto chatId = msg["chat"].value("id", std::int64_t{0});
        co_await bot.sendMessage(chatId,
                                 "Hi! I'm an async C++ bot built on "
                                 "Boost.Asio coroutines.");
    });

    // echobot：原样回复任何非命令消息
    dispatcher.onAnyMessage([](tb::BotClient& bot,
                               const json msg) -> net::awaitable<void> {
        const auto chatId = msg["chat"].value("id", std::int64_t{0});
        const std::string text = msg.value("text", "");
        if (!text.empty()) {
            co_await bot.sendMessage(chatId, "You said: " + text);
        }
    });

    // inline 键盘演示：/inline 出按钮，点击回 pong 气泡
    dispatcher.onCommand("inline", [](tb::BotClient& bot,
                                      const json msg) -> net::awaitable<void> {
        const auto chatId = msg["chat"].value("id", std::int64_t{0});
        const json markup = {
            {"inline_keyboard",
             json::array({{{{"text", "点我试试"},
                            {"callback_data", "btn:ping"}}}})}};
        co_await bot.sendMessage(chatId, "按下面的按钮：", markup);
    });
        dispatcher.onCallbackQuery(
        "btn:", [](tb::BotClient& bot,
                   const json cq) -> net::awaitable<void> {
            co_await bot.answerCallbackQuery(cq["id"].get<std::string>(),
                                             "pong!");
            // 气泡提示一闪而过，再编辑消息留下持久可见的效果
            const auto it = cq.find("message");
            if (it != cq.end() && it->is_object()) {
                const json m = *it;
                const auto cid = m["chat"].value("id", std::int64_t{0});
                const auto mid = m.value("message_id", std::int64_t{0});
                co_await bot.editMessageText(
                    cid, mid, "你点了按钮：pong！（这条消息已被编辑）");
            }
        });

    spdlog::info("Long polling started, Ctrl+C to stop");
    co_await tb::longPollLoop(bot, dispatcher);
}

int main(int argc, char** argv) {
    std::string token;
    if (argc > 1) {
        token = argv[1];
    } else if (const char* env = std::getenv("TG_BOT_TOKEN")) {
        token = env;
    }
    if (token.empty()) {
        std::cerr << "usage: tg_bot <bot-token>\n"
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
        // 主协程结束（启动失败等）时退出事件循环；否则 signal_set 的
        // 挂起等待会让 io.run() 永远不返回
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
