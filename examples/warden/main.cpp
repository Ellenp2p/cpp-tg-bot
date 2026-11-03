// Warden —— C++ 群务管家（基于 tgbot 框架）
//
// 目标：把"群里不该长期留着的媒体"按类型、按群、按可配置时限自动清掉，
// 并给机器人拥有者一个能看清"我在哪些群、权限够不够、要不要退群"的后台。
//
// 设计要点：
//   1. 删除任务先落 SQLite 再计时（deletions 表），进程崩溃/重启不丢；
//      启动时把上次卡在 inflight 的任务放回 pending，逾期任务立刻执行。
//   2. 删除有独立节流（默认全局 ~30/s、单群 ~4/s），不复用发消息的
//      单群 1 条/秒，否则活跃群里的贴纸会排队到几分钟后。
//   3. 群设置 / 群名册 / 权限快照全部持久化；面板走 Telegram 私聊，
//      群内回复优先 ephemeral（仅操作者可见），不可用时群内普通回复，
//      再不行才私聊发送者；回复按 TTL 自动删除。
//   4. 长轮询与 webhook 共用同一套 Dispatcher：TG_WEBHOOK_URL 存在即
//      webhook，否则长轮询；两种模式都显式订阅 my_chat_member。
//
// 功能边界：只做媒体定时清理 + 群清单/权限后台 + 告警。
// 欢迎语、入群验证、防刷屏、关键词过滤不在本示例范围内（见 README 第 11 节），
// 未来若加，作为独立模块挂到 Dispatcher，不要塞进删除调度器。
//
// 用法：
//   warden <bot-token>
//   TG_BOT_TOKEN=... WARDEN_OWNERS=<你的用户ID> ./warden
//
// 环境变量见 examples/warden/README.md。

#include "alerts.hpp"
#include "autodel.hpp"
#include "commands.hpp"
#include "config.hpp"
#include "health.hpp"
#include "ignore.hpp"
#include "media.hpp"
#include "replies.hpp"
#include "scheduler.hpp"
#include "store.hpp"
#include "warden_chat_sync.hpp"
#include "warden_events.hpp"
#include "warden_guard.hpp"
#include "warden_menus.hpp"
#include "warden_texts.hpp"
#include "warden_util.hpp"
#include "warden_views.hpp"

#include <tgbot/metrics.hpp>
#include <tgbot/tg_bot.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace warden {

namespace {

constexpr const char* kCallbackPrefix = "wd:";

} // namespace

// 命令权限/菜单统一从 commands.hpp 读，不散落在 handler。
void setupLogging(const Config& cfg) {
    try {
        std::filesystem::create_directories(cfg.logDir);
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            cfg.logDir + "/warden.log", 5 * 1024 * 1024, 3);
        auto logger =
            std::make_shared<spdlog::logger>("warden", spdlog::sinks_init_list{console, file});
        logger->set_level(spdlog::level::from_str(cfg.logLevel));
        logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(logger);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "log setup failed: %s\n", e.what());
    }
}

std::atomic<bool> gShuttingDown{false};

net::awaitable<int> run(Config cfg) {
    setReplyTtlSec(cfg.replyTtlSec);
    BotClient bot(cfg.token);

    std::int64_t botId = 0;
    std::string botUsername;
    std::string botName;
    try {
        const auto me = co_await bot.getMe();
        botId = me.value("id", std::int64_t{0});
        botUsername = me.value("username", std::string{});
        botName = me.value("first_name", std::string{});
        if (botName.empty()) {
            botName = botUsername;
        }
        spdlog::info("Warden {} connected as {} (@{}, id={}, instance={})",
                     kWardenVersion,
                     botName,
                     botUsername,
                     botId,
                     cfg.instanceId);
    } catch (const std::exception& e) {
        spdlog::error("getMe failed: {}", e.what());
        co_return 1;
    }

    // 命令菜单分四层，关键是"默认作用域清空"：
    //   - default：空（群成员如果落入 default，也看不到命令）
    //   - all_private_chats：panel/mygroups 等私聊命令
    //   - all_group_chats：空 → 普通群成员看不到任何命令
    //   - chat_administrators（每群）：只有该群管理员能看到 autodel/status
    // 不能只把 default 设成私聊命令再清空 all_group_chats：Telegram 在某个
    // 作用域没有命令时会回落到上一层，群成员可能又看到 default。所以 default
    // 必须是空的，私聊命令单独放 all_private_chats。
    // GCC 14 -O3 对"co_await 操作数里的临时 vector"会 ICE（musl 静态构建
    // 踩过），所以全部先落具名变量
    const auto privateCmds = privateCommands();
    const std::vector<tgbot::BotCommand> noCommands{};
    const json privateScope = {{"type", "all_private_chats"}};
    const json groupScope = {{"type", "all_group_chats"}};
    try {
        co_await bot.setMyCommands(noCommands);
        co_await bot.setMyCommands(privateCmds, privateScope);
        co_await bot.setMyCommands(noCommands, groupScope);
    } catch (const std::exception& e) {
        spdlog::warn("setMyCommands failed: {}", e.what());
    }

    auto store = std::make_shared<Store>(cfg.dbPath);
    const int recovered = store->resetInflight();
    if (recovered > 0) {
        spdlog::info("recovered {} inflight deletion job(s) from last run", recovered);
    }

    const auto executor = co_await net::this_coro::executor;
    tgbot::metrics::setBuildInfo(kWardenVersion, cfg.instanceId);

    // 事件循环延迟：每秒预期触发一次，实际触发晚了多少就是 lag。
    net::co_spawn(
        executor,
        []() -> net::awaitable<void> {
            auto expected = std::chrono::steady_clock::now();
            while (!gShuttingDown.load()) {
                expected += std::chrono::seconds{1};
                co_await tgbot::co_sleep(std::chrono::milliseconds{1000});
                const auto now = std::chrono::steady_clock::now();
                const auto lag = std::chrono::duration<double>(now - expected).count();
                tgbot::metrics::setEventLoopLag(std::max(0.0, lag));
                if (expected < now) {
                    expected = now;
                }
            }
        },
        net::detached);

    // 本地健康/控制端口：长轮询与 webhook 两种模式都能用，蓝绿发布脚本
    // 靠它判断新进程是否 ready、旧进程是否已经 drain。
    std::shared_ptr<HealthServer> health;
    if (cfg.healthPort > 0) {
        health = std::make_shared<HealthServer>(
            *store, gShuttingDown, cfg.instanceId, kWardenVersion, cfg.controlToken);
        health->start(executor, cfg.healthHost, static_cast<unsigned short>(cfg.healthPort));
    }

    // 告警出口：owner 私聊 + 飞书 + Discord 可同时开，互不影响。
    auto alerts = std::make_shared<AlertDispatcher>(executor);
    if (cfg.alertOwnerDm && !cfg.owners.empty()) {
        alerts->add(makeOwnerDmSink(bot, cfg.owners));
    }
    if (!cfg.alertFeishuUrl.empty()) {
        alerts->add(makeFeishuSink(cfg.alertFeishuUrl, cfg.alertFeishuSecret));
        spdlog::info("alert sink enabled: feishu");
    }
    if (!cfg.alertDiscordUrl.empty()) {
        alerts->add(makeDiscordSink(cfg.alertDiscordUrl));
        spdlog::info("alert sink enabled: discord");
    }
    if (cfg.alertOwnerDm && !cfg.owners.empty()) {
        spdlog::info("alert sink enabled: telegram-owner");
    }

    DeleteScheduler::Options sopt;
    sopt.workers = cfg.deleteWorkers;
    sopt.globalIntervalMs = cfg.globalDeleteMs;
    sopt.perChatIntervalMs = cfg.perChatDeleteMs;
    sopt.pinBackfill = cfg.pinBackfill;
    sopt.pinBackfillIntervalMs = cfg.pinBackfillMs;
    sopt.alert = [alerts](std::string text) -> net::awaitable<void> {
        alerts->publish(std::move(text));
        co_return;
    };
    auto sched = std::make_shared<DeleteScheduler>(bot, *store, sopt);
    sched->start(executor);

    const auto startedAt = std::chrono::steady_clock::now();
    auto knownChats = std::make_shared<std::unordered_set<std::int64_t>>();
    auto menus = std::make_shared<GroupMenuPublisher>(groupAdminCommands(), cfg.groupMenus);
    // 后台补发历史群的管理员菜单；不阻塞启动，也不影响收消息
    net::co_spawn(executor, publishAllGroupMenus(bot, *store, *menus), net::detached);

    tgbot::Dispatcher dispatcher;
    dispatcher.setBotUsername(botUsername);

    // 群内所有命令仅群管理员可用；非管理员静默丢弃（连"没权限"都不回，
    // 避免陌生人用 /autodel 试探群设置）。私聊命令不受影响。
    dispatcher.setCommandGuard(
        [store, sched, menus, owners = cfg.owners](BotClient& b, json msg) -> net::awaitable<bool> {
            co_return co_await groupCommandGuard(b, *store, *sched, owners, *menus, msg);
        });

    dispatcher.onMyChatMember([store, sched, menus, botId, knownChats](
                                  BotClient& b, const json mcm) -> net::awaitable<void> {
        co_await handleMyChatMember(b, *store, *sched, *menus, botId, knownChats, mcm);
    });

    // 未注册命令：只有明确 /foo@本bot 才删；/foo 和 /foo@OtherBot 不碰
    dispatcher.onAnyMessage([botUsername](BotClient& b, const json msg) -> net::awaitable<void> {
        if (!isGroupType(chatTypeOf(msg))) {
            co_return;
        }
        if (!isCommandAddressedToBot(msg, botUsername)) {
            co_return;
        }
        co_await sweepGroupCommand(b, msg);
    });

    dispatcher.onAnyMessage(
        [store, sched, knownChats](BotClient& b, const json msg) -> net::awaitable<void> {
            co_await handleMedia(b, *store, *sched, msg, knownChats);
        });

    // 置顶事件：把被置顶的消息登记为"永不自动删除"，并取消已排程的删除。
    // Telegram 在置顶时发一条带 pinned_message 的服务消息；这里只依赖它，
    // 不额外调 getChat，避免高频开销。
    dispatcher.onAnyMessage([store, sched](BotClient&, const json msg) -> net::awaitable<void> {
        const auto it = msg.find("pinned_message");
        if (it == msg.end() || !it->is_object()) {
            co_return;
        }
        const auto chatId = chatIdOf(msg);
        const auto pinnedId = it->value("message_id", std::int64_t{0});
        if (chatId == 0 || pinnedId == 0) {
            co_return;
        }
        store->markPinned(chatId, pinnedId, nowSec());
        sched->cancelMessage(chatId, pinnedId);
        spdlog::info("pinned message protected: chat={} msg={}", chatId, pinnedId);
    });

    // 自然语言别名：群里发“自动删除 贴纸 开启 10秒”等价于
    // /autodel 贴纸 10秒。前缀必须单独成词，避免误伤普通聊天。
    dispatcher.onAnyMessage(
        [store, sched, cfg](BotClient& b, const json msg) -> net::awaitable<void> {
            if (!isGroupType(chatTypeOf(msg))) {
                co_return;
            }
            const auto text = trim(msg.value("text", std::string{}));
            static constexpr std::string_view kPrefixes[] = {"自动删除", "自动清理"};
            for (const auto prefix : kPrefixes) {
                if (std::string_view{text}.substr(0, prefix.size()) != prefix) {
                    continue;
                }
                if (text.size() > prefix.size()) {
                    const auto next = static_cast<unsigned char>(text[prefix.size()]);
                    if (!std::isspace(next) && next != ':' &&
                        text.compare(prefix.size(), 3, "：") != 0) {
                        continue;
                    }
                }
                std::string args = trim(std::string_view{text}.substr(prefix.size()));
                while (!args.empty()) {
                    if (args.front() == ':') {
                        args = trim(std::string_view{args}.substr(1));
                        continue;
                    }
                    if (args.rfind("：", 0) == 0) {
                        args = trim(std::string_view{args}.substr(3));
                        continue;
                    }
                    break;
                }
                // 自然语言命令本体不删；只处理命令、回复按 TTL 清理
                co_await handleAutodel(b, *store, *sched, msg, args, false, cfg, true);
                co_return;
            }
        });

    // 自然语言：回复某人的消息后发“忽略 贴纸”/“恢复 贴纸”。
    dispatcher.onAnyMessage(
        [store, sched, cfg](BotClient& b, const json msg) -> net::awaitable<void> {
            if (!isGroupType(chatTypeOf(msg))) {
                co_return;
            }
            const auto text = trim(msg.value("text", std::string{}));
            for (const auto [prefix, remove] : {std::pair<std::string_view, bool>{"忽略", false},
                                                std::pair<std::string_view, bool>{"恢复", true}}) {
                if (std::string_view{text}.substr(0, prefix.size()) != prefix) {
                    continue;
                }
                if (text.size() > prefix.size()) {
                    const auto next = static_cast<unsigned char>(text[prefix.size()]);
                    if (!std::isspace(next) && next != ':' &&
                        text.compare(prefix.size(), 3, "：") != 0) {
                        continue;
                    }
                }
                std::string args = trim(std::string_view{text}.substr(prefix.size()));
                while (!args.empty()) {
                    if (args.front() == ':') {
                        args = trim(std::string_view{args}.substr(1));
                        continue;
                    }
                    if (args.rfind("：", 0) == 0) {
                        args = trim(std::string_view{args}.substr(3));
                        continue;
                    }
                    break;
                }
                // 只有“忽略”/“恢复”本身：当成普通聊天，不响应，避免误触。
                if (args.empty()) {
                    co_return;
                }
                co_await handleIgnore(b, *store, *sched, msg, args, remove, false, cfg, true);
                co_return;
            }
        });

    // ---- /start /help ----
    dispatcher.onCommand(
        "start",
        [store, sched, cfg, botName, botUsername, startedAt](
            BotClient& b, const json msg) -> net::awaitable<void> {
            const auto chatType = chatTypeOf(msg);
            const auto userId = msg.value("from", json::object()).value("id", std::int64_t{0});
            if (chatType == "private") {
                if (isOwner(cfg, userId)) {
                    co_await b.sendMessage(
                        chatIdOf(msg), dashboardText(*store, startedAt), dashboardKeyboard());
                } else {
                    tgbot::SendOptions options;
                    options.parseMode = tgbot::ParseMode::kHtml;
                    co_await b.sendMessage(chatIdOf(msg), helpText(botName, botUsername), options);
                }
                co_return;
            }
            co_await replyEphemeral(
                b,
                *sched,
                msg,
                isOwner(cfg, userId) ? "私聊我打开控制台：/panel" : "私聊我发 /help 查看用法",
                nullptr,
                tgbot::ParseMode::kHtml);
        });

    dispatcher.onCommand(
        "help", [botName, botUsername](BotClient& b, const json msg) -> net::awaitable<void> {
            // commands.hpp 已把 /help 设为 privateOnly，群里被守卫拦掉
            tgbot::SendOptions options;
            options.parseMode = tgbot::ParseMode::kHtml;
            co_await b.sendMessage(chatIdOf(msg), helpText(botName, botUsername), options);
        });

    // ---- /autodel ----
    dispatcher.onCommand(
        "autodel", [store, sched, cfg](BotClient& b, const json msg) -> net::awaitable<void> {
            co_await handleAutodel(b, *store, *sched, msg, commandArgs(msg), true, cfg, false);
        });

    // ---- /ignore /restore ----
    dispatcher.onCommand(
        "ignore", [store, sched, cfg](BotClient& b, const json msg) -> net::awaitable<void> {
            co_await handleIgnore(
                b, *store, *sched, msg, commandArgs(msg), false, false, cfg, false);
        });
    dispatcher.onCommand("restore",
                         [store, sched, cfg](BotClient& b, const json msg) -> net::awaitable<void> {
                             co_await handleIgnore(
                                 b, *store, *sched, msg, commandArgs(msg), true, false, cfg, false);
                         });

    // ---- /status ----
    dispatcher.onCommand(
        "status", [store, sched, botId](BotClient& b, const json msg) -> net::awaitable<void> {
            const auto chatId = chatIdOf(msg);
            if (!isGroupType(chatTypeOf(msg))) {
                co_await b.sendMessage(chatId, "群内使用：把我设为群管理员后发 /status。");
                co_return;
            }
            const auto record = co_await refreshChat(b, *store, botId, chatId);
            if (!record.has_value()) {
                co_await replyEphemeral(b, *sched, msg, "查询群信息失败，请稍后重试。");
                co_return;
            }
            std::string text = "🛡 我在「" +
                               (record->title.empty() ? std::string{"本群"} : record->title) +
                               "」的身份：" + record->botStatus + "\n";
            text += "删除消息：" + boolMark(record->canDelete) + "\n";
            text += "限制成员：" + boolMark(record->canRestrict) + "\n";
            text += "置顶消息：" + boolMark(record->canPin) + "\n";
            text += "邀请用户：" + boolMark(record->canInvite) + "\n";
            text += "管理群聊：" + boolMark(record->canManage) + "\n\n";
            text += record->canDelete ? "✅ 权限足够，可以执行自动删除。"
                                      : "⚠️ 缺少「删除消息」权限：请到群设置 → 管理员 → 我的权限"
                                        "里勾选，或重新把我设为管理员。";
            co_await replyEphemeral(b, *sched, msg, text);
        });

    // ---- /id ----
    dispatcher.onCommand("id", [sched](BotClient& b, const json msg) -> net::awaitable<void> {
        const auto user = msg.value("from", json::object());
        const std::string text = "🆔 用户 ID：" +
                                 std::to_string(user.value("id", std::int64_t{0})) + "\n群 ID：" +
                                 std::to_string(chatIdOf(msg));
        if (isGroupType(chatTypeOf(msg))) {
            co_await replyEphemeral(b, *sched, msg, text);
            co_return;
        }
        co_await b.sendMessage(chatIdOf(msg), text);
    });

    // ---- /panel /mygroups /stats ----
    dispatcher.onCommand(
        "panel", [store, cfg, startedAt](BotClient& b, const json msg) -> net::awaitable<void> {
            const auto chatId = chatIdOf(msg);
            const auto userId = msg.value("from", json::object()).value("id", std::int64_t{0});
            if (isOwner(cfg, userId)) {
                co_await b.sendMessage(
                    chatId, dashboardText(*store, startedAt), dashboardKeyboard());
                co_return;
            }
            // 普通用户：只显示自己管理的、且机器人还在群里的群。
            auto chats = store->chatsForAdmin(userId);
            if (chats.empty()) {
                // 名册是缓存：先补拉最近 12 个群的管理员列表
                for (const auto& recent : store->listChats(12, 0, true)) {
                    co_await refreshAdmins(b, *store, recent.chatId);
                }
                chats = store->chatsForAdmin(userId);
            }
            if (chats.empty()) {
                co_await b.sendMessage(chatId,
                                       "你还没有管理中的群，或者机器人不在那些群里。\n\n"
                                       "把我拉进群并设为管理员后，再发 /panel。\n"
                                       "如果机器人已经在群里，发一次 /autodel 让我刷新名册。");
                co_return;
            }
            co_await b.sendMessage(chatId, adminDashboardText(*store, userId), dashboardKeyboard());
        });

    dispatcher.onCommand("mygroups", [store](BotClient& b, const json msg) -> net::awaitable<void> {
        const auto chatId = chatIdOf(msg);
        const auto userId = msg.value("from", json::object()).value("id", std::int64_t{0});
        auto chats = store->chatsForAdmin(userId);
        if (chats.empty()) {
            // 名册是缓存：先补拉最近 12 个活跃群的管理员列表，
            // 避免"机器人早就进群了，但名册是空的"看起来像坏了。
            for (const auto& recent : store->listChats(12, 0, true)) {
                co_await refreshAdmins(b, *store, recent.chatId);
            }
            chats = store->chatsForAdmin(userId);
        }
        if (chats.empty()) {
            co_await b.sendMessage(chatId,
                                   "名册里还没有你管理的群。\n"
                                   "把我拉进群并设为管理员，或者在群里发一次 /autodel 后我再查。");
            co_return;
        }
        std::string text = "👥 你管理的群（缓存名册）：\n\n";
        for (const auto& chat : chats) {
            text += std::string{chat.enabled ? "🟢" : "⚪"} + " " +
                    (chat.title.empty() ? std::to_string(chat.chatId) : chat.title);
            text += chat.canDelete ? "" : " ⚠️缺删除权限";
            text += "\n";
        }
        co_await b.sendMessage(chatId, text);
    });

    dispatcher.onCommand(
        "stats", [store, cfg, startedAt](BotClient& b, const json msg) -> net::awaitable<void> {
            const auto chatId = chatIdOf(msg);
            const auto userId = msg.value("from", json::object()).value("id", std::int64_t{0});
            if (!isOwner(cfg, userId)) {
                co_await b.sendMessage(chatId, "仅机器人拥有者可用。");
                co_return;
            }
            co_await b.sendMessage(chatId, dashboardText(*store, startedAt), dashboardKeyboard());
        });

    // 隐藏命令：给 owner 自测飞书/Discord 通道用，不放进 / 菜单
    dispatcher.onCommand(
        "alerttest", [cfg, alerts](BotClient& b, const json msg) -> net::awaitable<void> {
            const auto chatId = chatIdOf(msg);
            if (!isOwner(cfg, senderId(msg))) {
                co_await b.sendMessage(chatId, "仅机器人拥有者可在私聊使用。");
                co_return;
            }
            alerts->publish("🧪 Warden 测试告警 " + std::to_string(nowSec()) +
                            "\n如果你在飞书/Discord 看到这条，说明通道配置正确。");
            co_await b.sendMessage(chatId, "已提交到所有已配置的告警通道；失败原因见日志。");
        });

    // ---- 拥有者后台回调 ----
    dispatcher.onCallbackQuery(
        kCallbackPrefix,
        [store, sched, cfg, botId, botName, botUsername, startedAt](
            BotClient& b, const json cq) -> net::awaitable<void> {
            const auto cqId = cq.value("id", std::string{});
            const auto user = cq.value("from", json::object());
            const auto userId = user.value("id", std::int64_t{0});
            const auto answer = [&](std::string text, bool alert = false) -> net::awaitable<void> {
                try {
                    co_await b.answerCallbackQuery(cqId, text, alert);
                } catch (const std::exception&) {
                    // 回调过期：忽略
                }
            };

            const bool owner = isOwner(cfg, userId);
            // 回调鉴权实时查一次 getChatMember，避免 chat_admins 缓存过期后
            // 被撤销的管理员还能改设置；查询失败按无权限处理（fail closed）。
            const auto canManageChat = [&](std::int64_t chatId) -> net::awaitable<bool> {
                if (owner) {
                    co_return true;
                }
                const auto status = co_await fetchMemberStatus(b, chatId, userId);
                co_return status.has_value() && isAdminStatus(*status);
            };

            const auto msgIt = cq.find("message");
            if (msgIt == cq.end() || !msgIt->is_object()) {
                co_await answer("这个面板已失效，请重新 /panel。", true);
                co_return;
            }
            const auto panelChatId =
                msgIt->value("chat", json::object()).value("id", std::int64_t{0});
            const auto panelMsgId = msgIt->value("message_id", std::int64_t{0});
            if (panelChatId == 0 || panelMsgId == 0) {
                co_await answer("这个面板已失效，请重新 /panel。", true);
                co_return;
            }
            const auto data = cq.value("data", std::string{});
            const auto action = data.substr(std::string{kCallbackPrefix}.size());

            auto show = [&](std::string text,
                            json keyboard,
                            tgbot::ParseMode parseMode =
                                tgbot::ParseMode::kNone) -> net::awaitable<void> {
                co_await showPanel(
                    b, panelChatId, panelMsgId, std::move(text), std::move(keyboard), parseMode);
            };

            if (action == "home") {
                co_await answer("");
                co_await show(
                    owner ? dashboardText(*store, startedAt) : adminDashboardText(*store, userId),
                    dashboardKeyboard());
                co_return;
            }
            if (action == "help") {
                co_await answer("");
                co_await show(
                    helpText(botName, botUsername), dashboardKeyboard(), tgbot::ParseMode::kHtml);
                co_return;
            }
            if (action.rfind("list:", 0) == 0) {
                int page = 0;
                try {
                    page = std::stoi(action.substr(5));
                } catch (const std::exception&) {
                    page = 0;
                }
                co_await answer("");
                if (owner) {
                    co_await show(listText(*store, page), listKeyboard(*store, page));
                } else {
                    co_await show(adminListText(*store, userId, page),
                                  adminListKeyboard(*store, userId, page));
                }
                co_return;
            }

            const auto parseId = [&](std::string_view prefix) -> std::optional<std::int64_t> {
                if (action.rfind(prefix, 0) != 0) {
                    return std::nullopt;
                }
                try {
                    return std::stoll(std::string{action.substr(prefix.size())});
                } catch (const std::exception&) {
                    return std::nullopt;
                }
            };

            const auto parseIdAndTail = [&](std::string_view prefix)
                -> std::optional<std::pair<std::int64_t, std::string>> {
                if (action.rfind(prefix, 0) != 0) {
                    return std::nullopt;
                }
                const auto rest = action.substr(prefix.size());
                const auto colon = rest.find(':');
                if (colon == std::string::npos) {
                    return std::nullopt;
                }
                try {
                    return std::make_pair(std::stoll(rest.substr(0, colon)),
                                          rest.substr(colon + 1));
                } catch (const std::exception&) {
                    return std::nullopt;
                }
            };

            if (const auto id = parseId("s:"); id.has_value()) {
                if (!co_await canManageChat(*id)) {
                    co_await answer("你只能管理自己担任管理员的群。", true);
                    co_return;
                }
                const auto record = store->chat(*id);
                if (!record.has_value()) {
                    co_await answer("这个群不在数据库里。", true);
                    co_return;
                }
                co_await answer("");
                co_await show(settingsPageText(*store, *record),
                              settingsPageKeyboard(*store, *record));
                co_return;
            }

            // 拥有者点某个类型：按预设档位循环（off → 30s → 1m → 5m → ...）
            if (const auto parsed = parseIdAndTail("d:"); parsed.has_value()) {
                const auto [chatId, kind] = *parsed;
                if (!co_await canManageChat(chatId)) {
                    co_await answer("你只能管理自己担任管理员的群。", true);
                    co_return;
                }
                if (!isKnownKind(kind)) {
                    co_await answer("未知类型。", true);
                    co_return;
                }
                auto record = store->chat(chatId);
                if (!record.has_value()) {
                    co_await answer("群不存在。", true);
                    co_return;
                }
                const int next = nextPreset(effectiveDelay(*store, chatId, kind));
                store->setDelay(chatId, kind, next);
                if (next > 0) {
                    store->setEnabled(chatId, true);
                } else {
                    sched->cancelKind(chatId, kind);
                }
                store->audit(chatId, userId, "panel.set." + kind, std::to_string(next));
                record = store->chat(chatId);
                co_await answer(std::string{kindLabel(kind)} + "：" + formatDelay(next));
                if (record.has_value()) {
                    co_await show(settingsPageText(*store, *record),
                                  settingsPageKeyboard(*store, *record));
                }
                co_return;
            }

            if (const auto parsed = parseIdAndTail("a:"); parsed.has_value()) {
                const auto [chatId, rawSeconds] = *parsed;
                if (!co_await canManageChat(chatId)) {
                    co_await answer("你只能管理自己担任管理员的群。", true);
                    co_return;
                }
                int seconds = 0;
                bool parsedSeconds = true;
                try {
                    seconds = std::stoi(rawSeconds);
                } catch (const std::exception&) {
                    parsedSeconds = false;
                }
                if (!parsedSeconds) {
                    co_await answer("时限参数非法。", true);
                    co_return;
                }
                if (seconds != 0 && (seconds < cfg.minDelaySec || seconds > cfg.maxDelaySec)) {
                    co_await answer("时限超出范围。", true);
                    co_return;
                }
                for (const auto* kind : kMediaKinds) {
                    store->setDelay(chatId, kind, seconds);
                    if (seconds == 0) {
                        sched->cancelKind(chatId, kind);
                    }
                }
                if (seconds > 0) {
                    store->setEnabled(chatId, true);
                }
                store->audit(chatId, userId, "panel.set.all", std::to_string(seconds));
                const auto record = store->chat(chatId);
                co_await answer("已更新");
                if (record.has_value()) {
                    co_await show(settingsPageText(*store, *record),
                                  settingsPageKeyboard(*store, *record));
                }
                co_return;
            }

            if (const auto id = parseId("g:"); id.has_value()) {
                if (!co_await canManageChat(*id)) {
                    co_await answer("你只能管理自己担任管理员的群。", true);
                    co_return;
                }
                const auto record = store->chat(*id);
                if (!record.has_value()) {
                    co_await answer("这个群不在数据库里。", true);
                    co_return;
                }
                co_await answer("");
                co_await show(detailText(*store, *record), detailKeyboard(*record, owner));
                co_return;
            }

            if (const auto id = parseId("t:"); id.has_value()) {
                if (!co_await canManageChat(*id)) {
                    co_await answer("你只能管理自己担任管理员的群。", true);
                    co_return;
                }
                auto record = store->chat(*id);
                if (!record.has_value()) {
                    co_await answer("群不存在。", true);
                    co_return;
                }
                const bool next = !record->enabled;
                store->setEnabled(*id, next);
                if (!next) {
                    sched->cancelChat(*id);
                }
                store->audit(*id, userId, next ? "panel.enable" : "panel.disable", "");
                record->enabled = next;
                co_await answer(next ? "已启用" : "已停用");
                co_await show(detailText(*store, *record), detailKeyboard(*record, owner));
                co_return;
            }

            if (const auto id = parseId("r:"); id.has_value()) {
                if (!co_await canManageChat(*id)) {
                    co_await answer("你只能管理自己担任管理员的群。", true);
                    co_return;
                }
                co_await answer("正在刷新…");
                const auto record = co_await refreshChat(b, *store, botId, *id);
                if (!record.has_value()) {
                    co_await answer("刷新失败：可能已被踢出或网络异常。", true);
                    co_return;
                }
                co_await show(detailText(*store, *record), detailKeyboard(*record, owner));
                co_return;
            }

            if (const auto id = parseId("c:"); id.has_value()) {
                if (!co_await canManageChat(*id)) {
                    co_await answer("你只能管理自己担任管理员的群。", true);
                    co_return;
                }
                sched->cancelChat(*id);
                const auto record = store->chat(*id);
                co_await answer("已取消待删任务");
                if (record.has_value()) {
                    co_await show(detailText(*store, *record), detailKeyboard(*record, owner));
                }
                co_return;
            }

            if (const auto id = parseId("x:"); id.has_value()) {
                if (!owner) {
                    co_await answer("只有机器人拥有者可以让我退群。", true);
                    co_return;
                }
                const auto record = store->chat(*id);
                if (!record.has_value()) {
                    co_await answer("群不存在。", true);
                    co_return;
                }
                co_await answer("");
                json kb = {
                    {"inline_keyboard",
                     json::array(
                         {json::array({json{{"text", "确认退出"},
                                            {"callback_data", "wd:xo:" + std::to_string(*id)}}}),
                          json::array({json{{"text", "取消"},
                                            {"callback_data", "wd:g:" + std::to_string(*id)}}})})}};
                co_await show("⚠️ 确认退出「" +
                                  (record->title.empty() ? std::to_string(*id) : record->title) +
                                  "」？\n\n退出后本群所有待删任务会被取消，"
                                  "需要重新邀请我才能继续管理。",
                              kb);
                co_return;
            }

            if (const auto id = parseId("xo:"); id.has_value()) {
                if (!owner) {
                    co_await answer("只有机器人拥有者可以让我退群。", true);
                    co_return;
                }
                std::string leaveError;
                try {
                    co_await b.leaveChat(*id);
                } catch (const std::exception& e) {
                    leaveError = e.what();
                }
                if (!leaveError.empty()) {
                    co_await answer("退出失败：" + leaveError, true);
                    co_return;
                }
                store->markLeft(*id, nowSec());
                sched->cancelChat(*id);
                store->audit(*id, userId, "panel.leave", "");
                co_await answer("已退出");
                co_await show(dashboardText(*store, startedAt), dashboardKeyboard());
                co_return;
            }

            co_await answer("");
        });

    // ---- 接收模式 ----
    spdlog::info("update queue: workers={}, maxPending={}, maxPendingBytes={}MB",
                 cfg.updateWorkers,
                 cfg.maxPendingUpdates,
                 cfg.maxPendingMB);
    if (cfg.useWebhook) {
        spdlog::info("webhook mode: {} (listen {}:{})",
                     cfg.webhook.publicUrl,
                     cfg.webhook.host,
                     cfg.webhook.port);
        co_await tgbot::webhookLoop(bot, dispatcher, cfg.webhook, &gShuttingDown);
    } else {
        tgbot::LongPollOptions opts;
        opts.offsetFile = cfg.offsetFile;
        opts.dropPendingUpdates = cfg.dropPendingUpdates;
        opts.pollTimeoutSeconds = cfg.pollTimeoutSeconds;
        opts.maxPendingUpdates = static_cast<std::size_t>(cfg.maxPendingUpdates);
        opts.maxPendingBytes = static_cast<std::size_t>(cfg.maxPendingMB) * 1024 * 1024;
        opts.updateWorkers = cfg.updateWorkers;
        // 不订阅 my_chat_member 就永远不知道被拉进/踢出了哪些群
        opts.allowedUpdates = R"(["message","callback_query","my_chat_member"])";
        spdlog::info("long polling mode (resume={})", !cfg.dropPendingUpdates ? "yes" : "no");
        co_await tgbot::longPollLoop(bot, dispatcher, opts, &gShuttingDown);
    }

    // 优雅停机：先给 webhook 会话一点时间把 200/503 写完并完成 dispatch，
    // 再等 Dispatcher 的 in-flight update 归零。硬上限由 drainSeconds 控制，
    // 超时也只影响在途 handler，删除任务都已落库。
    if (gShuttingDown.load()) {
        spdlog::info("draining: in-flight updates={}, timeout={}s, health={}",
                     dispatcher.inFlightUpdates(),
                     cfg.drainSeconds,
                     health ? "on" : "off");
        co_await tgbot::co_sleep(std::chrono::milliseconds{200});
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{cfg.drainSeconds};
        while (dispatcher.inFlightUpdates() > 0 && std::chrono::steady_clock::now() < deadline) {
            co_await tgbot::co_sleep(std::chrono::milliseconds{50});
        }
        if (dispatcher.inFlightUpdates() > 0) {
            spdlog::warn("drain timeout: {} update(s) still in flight",
                         dispatcher.inFlightUpdates());
        } else {
            spdlog::info("drain complete: no in-flight updates");
        }
    }

    spdlog::info("stopping: pending deletions={} (kept in database)", store->pendingDeletions());
    store->checkpoint();
    co_return 0;
}

} // namespace warden

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view{argv[1]} == "--version") {
        std::printf("warden %s\n", warden::kWardenVersion);
        return 0;
    }

    warden::Config cfg;
    try {
        cfg = warden::loadConfig(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    warden::setupLogging(cfg);

    boost::asio::io_context io;
    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
#ifdef SIGBREAK
    signals.add(SIGBREAK);
#endif
    signals.async_wait([&io, drainSeconds = cfg.drainSeconds](const boost::system::error_code& ec,
                                                              int sig) {
        if (ec) {
            return;
        }
        if (warden::gShuttingDown.exchange(true)) {
            // 第二次信号：不等人了，立刻停（给运维一个逃生口）
            spdlog::warn("second signal {}, forcing exit", sig);
            io.stop();
            return;
        }
        spdlog::info("received signal {}, draining (timeout {}s + 5s hard)", sig, drainSeconds);
        // 长轮询可能正阻塞在 getUpdates 里；硬超时确保进程不会永远不退。
        auto timer =
            std::make_shared<boost::asio::steady_timer>(io, std::chrono::seconds{drainSeconds + 5});
        timer->async_wait([&io, timer](const boost::system::error_code& ec2) {
            if (!ec2) {
                spdlog::warn("drain hard timeout, forcing exit");
                io.stop();
            }
        });
    });

    int exitCode = 0;
    boost::asio::co_spawn(io, warden::run(cfg), [&io, &exitCode](std::exception_ptr e, int code) {
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
