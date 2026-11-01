#include <tgbot/bot_client.hpp>
#include <tgbot/dispatcher.hpp>
#include <tgbot/long_poll.hpp>

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

net::awaitable<int> run(const std::string& token) {
    tgbot::BotClient bot(token);

    std::string botUsername;
    try {
        const auto me = co_await bot.getMe();
        botUsername = me.value("username", std::string{});
        spdlog::info("Connected as @{} (id={})",
                     me.value("username", std::string("?")),
                     me.value("id", 0L));
    } catch (const std::exception& e) {
        // 错 token / 网络不可达：必须让进程以非 0 退出，
        // 否则 systemd/容器的重启策略永远不会触发
        spdlog::error("getMe failed: {}", e.what());
        co_return 1;
    }

    // 设置客户端命令菜单，失败不阻断启动
    //
    // 注意：不要把 std::vector 临时对象直接塞进 co_await 的操作数
    // （co_await bot.setMyCommands(std::vector<...>{...})）——GCC 14 在 -O3 下
    // 会对"co_await 操作数里的临时 initializer_list 容器"报
    // internal compiler error: in gimple_add_tmp_var, at gimplify.cc:802，
    // musl 静态构建（CI linux-static）就是这么挂的。先落具名变量即可。
    const std::vector<tgbot::BotCommand> commands{
        {"start", "开始使用", false},
        {"inline", "inline 键盘演示", false},
    };
    try {
        co_await bot.setMyCommands(commands);
    } catch (const std::exception& e) {
        spdlog::warn("setMyCommands failed: {}", e.what());
    }

    tgbot::Dispatcher dispatcher;
    // 群里多个 bot 时，"/inline@OtherBot" 不归我们处理
    dispatcher.setBotUsername(botUsername);

    dispatcher.onCommand("start", [](tgbot::BotClient& bot,
                                     const json msg) -> net::awaitable<void> {
        const auto chatId = msg["chat"].value("id", std::int64_t{0});
        co_await bot.sendMessage(chatId,
                                 "Hi! I'm an async C++ bot built on "
                                 "Boost.Asio coroutines.");
    });

    // echobot：原样回复任何非命令消息
    dispatcher.onAnyMessage([](tgbot::BotClient& bot,
                               const json msg) -> net::awaitable<void> {
        const auto chatId = msg["chat"].value("id", std::int64_t{0});
        const std::string text = msg.value("text", "");
        if (!text.empty()) {
            co_await bot.sendMessage(chatId, "You said: " + text);
        }
    });

    // inline 键盘演示：/inline 出按钮，点击回 pong 气泡
    dispatcher.onCommand("inline", [](tgbot::BotClient& bot,
                                      const json msg) -> net::awaitable<void> {
        const auto chatId = msg["chat"].value("id", std::int64_t{0});
        const json markup = {
            {"inline_keyboard",
             json::array({{{{"text", "点我试试"},
                            {"callback_data", "btn:ping"}}}})}};
        co_await bot.sendMessage(chatId, "按下面的按钮：", markup);
    });
        dispatcher.onCallbackQuery(
        "btn:", [](tgbot::BotClient& bot,
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
    co_await tgbot::longPollLoop(bot, dispatcher);
    co_return 0;
}

int main(int argc, char** argv) {
    std::string token;
    if (argc > 1) {
        token = argv[1];
    } else if (const char* env = std::getenv("TG_BOT_TOKEN")) {
        token = env;
    }
    if (token.empty()) {
        std::cerr << "usage: echo_bot <bot-token>\n"
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

    int exitCode = 0;
    net::co_spawn(io, run(token),
                  [&io, &exitCode](std::exception_ptr e, int code) {
                      // 主协程结束（启动失败等）时退出事件循环；否则
                      // signal_set 的挂起等待会让 io.run() 永远不返回
                      if (e) {
                          try {
                              std::rethrow_exception(e);
                          } catch (const std::exception& ex) {
                              spdlog::error("fatal: {}", ex.what());
                          }
                          exitCode = 1;
                      } else {
                          exitCode = code;
                      }
                      io.stop();
                  });
    io.run();
    spdlog::info("bye");
    return exitCode;
}
