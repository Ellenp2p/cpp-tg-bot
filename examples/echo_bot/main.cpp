#include <tgbot/bot_client.hpp>
#include <tgbot/dispatcher.hpp>
#include <tgbot/job_queue.hpp>
#include <tgbot/long_poll.hpp>
#include <tgbot/lottery_panel.hpp>
#include <tgbot/panel.hpp>
#include <tgbot/reconciliation.hpp>
#include <tgbot/red_packet_batch.hpp>
#include <tgbot/red_packet_ledger.hpp>
#include <tgbot/red_packet_panel.hpp>
#include <tgbot/red_packet_service.hpp>
#include <tgbot/rich_text.hpp>
#include <tgbot/stream_message.hpp>
#include <tgbot/util.hpp>

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
        spdlog::info(
            "Connected as @{} (id={})", me.value("username", std::string("?")), me.value("id", 0L));
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
        {"rich", "富文本渲染演示", false},
        {"stream", "流式输出演示", false},
        {"lottery", "发一个抽奖操作台（红包）", false},
        {"redpacket", "在群里发一个红包，大家点按钮抢", false},
    };
    try {
        co_await bot.setMyCommands(commands);
    } catch (const std::exception& e) {
        spdlog::warn("setMyCommands failed: {}", e.what());
    }

    tgbot::Dispatcher dispatcher;
    // 群里多个 bot 时，"/inline@OtherBot" 不归我们处理
    dispatcher.setBotUsername(botUsername);

    // 操作台：一条消息 = 一块可交互面板（抽奖/红包等）。ticker 负责倒计时开奖
    tgbot::PanelManager panels(bot);
    net::co_spawn(co_await net::this_coro::executor, panels.runTicker(), net::detached);

    // 红包账本 + 持久化任务队列：重启不丢，没领完的到期批量退款
    auto ledger = std::make_shared<tgbot::RedPacketLedger>("red_packets.db");
    tgbot::JobQueue jobs("jobs.db");
    tgbot::ChainMonitor chain; // 连续失败就熔断，暂停自动退款

    const tgbot::AlertSink alert = [](std::string_view level,
                                      std::string_view message) -> net::awaitable<void> {
        // 生产环境这里改成给管理员发消息（例如固定 chat_id 的 sendMessage）
        spdlog::error("[告警/{}] {}", level, message);
        co_return;
    };

    tgbot::RefundService refunds(*ledger, chain, tgbot::RefundService::Options{}, alert);
    tgbot::Reconciler reconciler(*ledger, chain, tgbot::Reconciler::Options{});

    const tgbot::BatchRefundHandler refund =
        [](const tgbot::RefundBatch& batch,
           tgbot::TxRefRecorder recordRef) -> net::awaitable<tgbot::RefundBatchResult> {
        // 一次调用处理一整批：链上就是一次批量转账，省 gas。
        // 拿到 tx hash 的第一时间就落库，否则崩溃后没法对账。
        const auto txHash = "local-batch:" + batch.id;
        recordRef(txHash);
        for (const auto& item : batch.items) {
            spdlog::info("退款：红包 {} → 用户 {}，金额 {} wei",
                         item.packetId,
                         item.creatorId,
                         item.amount.toString());
        }
        tgbot::RefundBatchResult result;
        result.ref = txHash;
        co_return result;
    };

    net::co_spawn(
        co_await net::this_coro::executor,
        jobs.runWorker([&refunds, refund](const tgbot::JobRecord& job) -> net::awaitable<void> {
            co_await refunds.handleJob(job, refund);
        }),
        net::detached);

    // 对账：补上"广播成功但进程崩了"的窗口。没有真实链时 probe 一律 not_found
    const tgbot::ChainProbe probe =
        [](std::string_view txRef) -> net::awaitable<tgbot::ChainTxStatus> {
        spdlog::debug("对账探测: {}", txRef);
        co_return tgbot::ChainTxStatus::kNotFound;
    };
    net::co_spawn(
        co_await net::this_coro::executor,
        [&reconciler, probe, alert]() -> net::awaitable<void> {
            while (true) {
                co_await tgbot::co_sleep(std::chrono::seconds{60});
                const auto report = co_await reconciler.run(probe, alert);
                if (report.scanned > 0) {
                    spdlog::info(
                        "对账：扫描 {} 笔，确认 {}，重排 {}，stuck {}，"
                        "待确认 {}",
                        report.scanned,
                        report.confirmed,
                        report.requeued,
                        report.stuck,
                        report.stillPending);
                }
            }
        },
        net::detached);

    dispatcher.onCommand("start",
                         [](tgbot::BotClient& bot, const json msg) -> net::awaitable<void> {
                             const auto chatId = msg["chat"].value("id", std::int64_t{0});
                             co_await bot.sendMessage(chatId,
                                                      "Hi! I'm an async C++ bot built on "
                                                      "Boost.Asio coroutines.");
                         });

    // echobot：原样回复任何非命令消息
    dispatcher.onAnyMessage([](tgbot::BotClient& bot, const json msg) -> net::awaitable<void> {
        const auto chatId = msg["chat"].value("id", std::int64_t{0});
        const std::string text = msg.value("text", "");
        if (!text.empty()) {
            co_await bot.sendMessage(chatId, "You said: " + text);
        }
    });

    // 富文本：Bot API 的 parse_mode（HTML 最省心，只需转义 & < >）
    dispatcher.onCommand("rich", [](tgbot::BotClient& bot, const json msg) -> net::awaitable<void> {
        const auto chatId = msg["chat"].value("id", std::int64_t{0});
        tgbot::SendOptions send;
        send.parseMode = tgbot::ParseMode::kHtml;
        send.disableWebPagePreview = true;
        const std::string text =
            "<b>粗体</b> / <i>斜体</i> / <u>下划线</u> / <s>删除线</s>\n"
            "<code>inline code</code>\n"
            "<pre><code class=\"language-cpp\">int main() { return 0; }</code></pre>\n"
            "<blockquote>引用块</blockquote>\n"
            "<span class=\"tg-spoiler\">剧透（点一下才显示）</span>\n"
            "链接：<a href=\"https://core.telegram.org/bots/api\">Bot API</a>\n"
            "转义示例：&lt;tag&gt; &amp; 100%";
        co_await bot.sendMessage(chatId, text, send);
    });

    // 流式输出：先发占位，再按节流反复 editMessageText
    dispatcher.onCommand("stream",
                         [](tgbot::BotClient& bot, const json msg) -> net::awaitable<void> {
                             const auto chatId = msg["chat"].value("id", std::int64_t{0});
                             tgbot::StreamMessage stream(bot);
                             co_await stream.begin(chatId);
                             const char* chunks[] = {"正在",
                                                     "生成",
                                                     "一段",
                                                     "流式",
                                                     "文本",
                                                     "……",
                                                     "每",
                                                     "200ms",
                                                     "推送",
                                                     "一次，",
                                                     "但只有",
                                                     "满足节流",
                                                     "才会",
                                                     "真正",
                                                     "编辑消息。"};
                             for (const char* c : chunks) {
                                 co_await stream.append(c);
                                 co_await tgbot::co_sleep(std::chrono::milliseconds{200});
                             }
                             co_await stream.finish();
                         });

    // 抽奖操作台：/lottery 发一条可交互消息，点按钮参与，到点自动开奖
    dispatcher.onCommand(
        "lottery", [&panels](tgbot::BotClient&, const json msg) -> net::awaitable<void> {
            const auto chatId = msg["chat"].value("id", std::int64_t{0});
            tgbot::LotteryPanel::Config cfg;
            cfg.title = "新年红包";
            cfg.prize = "专属红包";
            cfg.winnerCount = 1;
            cfg.duration = std::chrono::seconds{120};
            cfg.creatorId = msg["from"].value("id", std::int64_t{0});
            co_await panels.open(chatId, std::make_shared<tgbot::LotteryPanel>(cfg));
        });

    // 群红包：消息本身就是红包，抢到多少、谁抢的都在群里可见
    dispatcher.onCommand(
        "redpacket",
        [&panels, ledger, &jobs](tgbot::BotClient&, const json msg) -> net::awaitable<void> {
            const auto chatId = msg["chat"].value("id", std::int64_t{0});
            tgbot::RedPacketSpec spec;
            spec.chatId = chatId;
            spec.creatorId = msg["from"].value("id", std::int64_t{0});
            spec.totalCents = 10000; // ¥100.00
            spec.count = 5;
            spec.ttl = std::chrono::seconds{3600};
            // 落账 + 排一条"到期退款"任务（进程重启也还在）
            const auto packetId = tgbot::createRedPacket(*ledger, jobs, spec);
            co_await panels.open(chatId, std::make_shared<tgbot::RedPacketPanel>(ledger, packetId));
        });

    // 所有操作台按钮统一走 "p:" 前缀
    dispatcher.onCallbackQuery(
        "p:", [&panels](tgbot::BotClient&, const json cq) -> net::awaitable<void> {
            co_await panels.handleCallback(cq);
        });

    // inline 键盘演示：/inline 出按钮，点击回 pong 气泡
    dispatcher.onCommand(
        "inline", [](tgbot::BotClient& bot, const json msg) -> net::awaitable<void> {
            const auto chatId = msg["chat"].value("id", std::int64_t{0});
            const json markup = {
                {"inline_keyboard",
                 json::array({{{{"text", "点我试试"}, {"callback_data", "btn:ping"}}}})}};
            co_await bot.sendMessage(chatId, "按下面的按钮：", markup);
        });
    dispatcher.onCallbackQuery(
        "btn:", [](tgbot::BotClient& bot, const json cq) -> net::awaitable<void> {
            co_await bot.answerCallbackQuery(cq["id"].get<std::string>(), "pong!");
            // 气泡提示一闪而过，再编辑消息留下持久可见的效果
            const auto it = cq.find("message");
            if (it != cq.end() && it->is_object()) {
                const json m = *it;
                const auto cid = m["chat"].value("id", std::int64_t{0});
                const auto mid = m.value("message_id", std::int64_t{0});
                co_await bot.editMessageText(cid, mid, "你点了按钮：pong！（这条消息已被编辑）");
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
#ifdef SIGBREAK // Windows 下控制台关闭事件
    signals.add(SIGBREAK);
#endif
    signals.async_wait([&io](const boost::system::error_code& ec, int sig) {
        if (!ec) {
            spdlog::info("received signal {}, shutting down...", sig);
            io.stop();
        }
    });

    int exitCode = 0;
    net::co_spawn(io, run(token), [&io, &exitCode](std::exception_ptr e, int code) {
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
