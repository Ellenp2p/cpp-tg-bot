// Warden 持久层 + 纯函数的最小回归测试（不依赖网络/Telegram）。
// 重点验证：删除任务的 claim/finish/崩溃恢复、设置缓存、管理员名册。

#include <tgbot/metrics.hpp>
#include "../src/chat_dispatch_util.hpp"
#include "commands.hpp"
#include "delete_result.hpp"
#include "health.hpp"
#include "media.hpp"
#include "store.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

} // namespace

void testHealthServer() {
    warden::Store store(":memory:");
    std::atomic<bool> draining{false};
    auto server =
        std::make_shared<warden::HealthServer>(store, draining, "test-inst", "0.1.0", "secret");
    boost::asio::io_context io;
    server->start(io.get_executor(), "127.0.0.1", 19090);
    std::thread thread([&io] { io.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    const auto request = [](const std::string& raw) {
        boost::asio::ip::tcp::iostream stream("127.0.0.1", "19090");
        if (!stream) {
            return std::string{};
        }
        stream << raw;
        stream.flush();
        std::ostringstream out;
        out << stream.rdbuf();
        return out.str();
    };
    const auto statusOf = [](const std::string& response) {
        const auto sp = response.find(' ');
        if (sp == std::string::npos || response.size() < sp + 4) {
            return 0;
        }
        try {
            return std::stoi(response.substr(sp + 1, 3));
        } catch (const std::exception&) {
            return 0;
        }
    };
    const auto get = [&](const std::string& path) {
        return request("GET " + path +
                       " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                       "Connection: close\r\n\r\n");
    };
    const auto postDrain = [&](const std::string& token) {
        return request(
            "POST /drain HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "X-Warden-Token: " +
            token + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    };

    check(statusOf(get("/healthz")) == 200, "healthz 200");
    check(statusOf(get("/readyz")) == 200, "readyz 200 before drain");
    check(statusOf(postDrain("wrong")) == 403, "drain wrong token 403");
    check(statusOf(postDrain("secret")) == 202, "drain token 202");
    check(draining.load(), "drain sets flag");
    check(statusOf(get("/readyz")) == 503, "readyz 503 after drain");
    tgbot::metrics::observeUpdateWait(0.1);
    const auto metrics = get("/metrics");
    check(statusOf(metrics) == 200, "metrics 200");
    check(metrics.find("warden_update_queue_depth") != std::string::npos,
          "metrics exposes queue gauge");
    check(metrics.find("warden_telegram_requests_total") != std::string::npos,
          "metrics exposes telegram counter");
    check(metrics.find("# TYPE warden_update_queue_depth gauge") != std::string::npos,
          "metrics has prometheus type");
    check(metrics.find("le=\"+Inf\"") != std::string::npos, "metrics has histogram buckets");

    io.stop();
    thread.join();
}

void testCommandPolicies();
void testUpdateQueue();
void testQueueBackpressure();

int runTests() {
    using namespace warden;

    // ---- 延迟解析 / 媒体识别 ----
    check(parseDelay("30") == 30, "parse 30");
    check(parseDelay("30s") == 30, "parse 30s");
    check(parseDelay("5m") == 300, "parse 5m");
    check(parseDelay("2h") == 7200, "parse 2h");
    check(parseDelay("1d") == 86400, "parse 1d");
    check(parseDelay("off") == 0, "parse off");
    check(parseDelay("20 秒") == 20, "parse 20 秒");
    check(parseDelay("1分钟") == 60, "parse 1分钟");
    check(parseDelay("2 小时") == 7200, "parse 2 小时");
    check(parseDelay("1天") == 86400, "parse 1天");
    check(!parseDelay("abc").has_value(), "parse abc");
    check(!parseDelay("5x").has_value(), "parse 5x");

    check(kindDefaultDelay("sticker") > 0, "sticker default delete");
    check(kindDefaultDelay("photo") > 0, "photo default delete");
    check(kindDefaultDelay("video") > 0, "video default delete");
    check(kindDefaultDelay("voice") > 0, "voice default delete");
    check(kindDefaultDelay("audio") == 0, "audio default keep");

    check(detectMediaKind(nlohmann::json{{"sticker", nlohmann::json::object()}}) == "sticker",
          "detect sticker");
    check(detectMediaKind(nlohmann::json{{"photo", nlohmann::json::array()}}) == "photo",
          "detect photo");
    check(detectMediaKind(nlohmann::json{{"video_note", nlohmann::json::object()}}) == "video_note",
          "detect video_note");
    check(detectMediaKind(nlohmann::json{{"text", "hi"}}).empty(), "detect text");

    // ---- 删除结果分类：手动删除 / 太老 → 视为完成，不重试不告警 ----
    check(deleteErrorMeansGone("Telegram API error 400: deleteMessage: Bad Request: "
                               "message to delete not found"),
          "gone: not found");
    check(deleteErrorMeansGone("Bad Request: message can't be deleted"), "gone: can't be deleted");
    check(deleteErrorMeansGone("Bad Request: message is too old"), "gone: too old");
    check(deleteErrorMeansGone("Bad Request: MESSAGE_ID_INVALID"), "gone: invalid id");
    check(!deleteErrorMeansGone("Bad Request: not enough rights"), "not gone: rights");
    check(!deleteErrorMeansGone("Bad Request: chat not found"), "not gone: chat");
    check(deleteErrorMeansGone("Bad Request: ephemeral message not found"), "gone: ephemeral");
    check(deleteErrorMeansGone("Bad Request: MESSAGE_NOT_FOUND"), "gone: MESSAGE_NOT_FOUND");

    // ---- 群登记 / 设置 ----
    Store store(":memory:");
    store.ensureChat(-100, "supergroup", "Test Group", 1000);
    {
        const auto chat = store.chat(-100);
        check(chat.has_value(), "ensureChat visible");
        check(chat->botStatus == "member", "ensureChat default status");
        check(chat->enabled, "new chat enabled by default");
    }

    ChatRecord record;
    record.chatId = -100;
    record.type = "supergroup";
    record.title = "Test Group";
    record.botStatus = "administrator";
    record.canDelete = true;
    record.canRestrict = true;
    record.enabled = true;
    record.joinedAt = 1000;
    record.lastSeenAt = 1100;
    record.refreshedAt = 1100;
    store.saveChat(record);
    {
        const auto chat = store.chat(-100);
        check(chat->canDelete, "saveChat permissions");
        check(chat->enabled, "saveChat enables chat");
    }

    // 刷新权限时不应把 enabled 反向覆盖成 false
    record.enabled = false;
    record.canDelete = false;
    store.saveChat(record);
    {
        const auto chat = store.chat(-100);
        check(chat->enabled, "refresh keeps enabled");
        check(!chat->canDelete, "refresh updates permissions");
    }

    store.setDelay(-100, "sticker", 30);
    check(store.settings(-100)->delays.at("sticker") == 30, "setDelay visible");
    store.clearSettings(-100);
    check(store.settings(-100)->delays.empty(), "clearSettings");

    // ---- 删除任务 ----
    store.addDeletion(-100, 5, "sticker", 1200, 1000);
    check(!store.claimDeletion(1199).has_value(), "not due yet");
    auto job = store.claimDeletion(1200);
    check(job.has_value() && job->messageId == 5, "claim due job");
    check(store.pendingDeletions() == 1, "inflight still pending");

    // 模拟崩溃：inflight 应被放回 pending，并可再次领取
    check(store.resetInflight() == 1, "reset inflight");
    job = store.claimDeletion(1200);
    check(job.has_value(), "reclaim after crash");
    store.finishDeletion(-100, 5, true, "");
    check(store.pendingDeletions() == 0, "finish removes pending");

    store.addDeletion(-100, 6, "photo", 1300, 1000);
    store.cancelKindDeletions(-100, "photo");
    check(!store.claimDeletion(1300).has_value(), "cancel kind");

    store.addDeletion(-100, 7, "video", 1400, 1000);
    store.cancelChatDeletions(-100);
    check(!store.claimDeletion(1400).has_value(), "cancel chat");

    store.addDeletion(-100, 8, "voice", 1500, 1000);
    const auto retry = store.claimDeletion(1500);
    check(retry.has_value(), "claim before retry");
    store.retryDeletion(-100, 8, 1600, "network");
    check(!store.claimDeletion(1599).has_value(), "retry not due");
    const auto retried = store.claimDeletion(1600);
    check(retried.has_value(), "retry due");
    store.finishDeletion(-100, 8, true, "");

    // ---- 置顶消息：登记 + 取消已排程任务 ----
    store.markPinned(-100, 99, 1000);
    check(store.isPinned(-100, 99), "markPinned visible");
    check(!store.isPinned(-100, 100), "unpinned message not protected");
    store.addDeletion(-100, 99, "photo", 2000, 1000);
    store.cancelMessageDeletion(-100, 99);
    check(!store.claimDeletion(2000).has_value(), "pinned cancel deletion");

    // ---- ephemeral 回复删除：target_user 随任务落库 ----
    store.addDeletion(-100, -777, "ephemeral", 4000, 1000, 42);
    {
        const auto eph = store.claimDeletion(4000);
        check(eph.has_value() && eph->messageId == -777 && eph->targetUserId == 42,
              "ephemeral job target user");
        if (eph.has_value()) {
            store.finishDeletion(-100, -777, true, "");
        }
    }

    // ---- 启动置顶补扫：只查有未完成任务的群，且去重 ----
    store.addDeletion(-100, 70, "sticker", 3000, 1000);
    store.addDeletion(-100, 71, "photo", 3000, 1000);
    {
        const auto pendingChats = store.chatsWithPendingDeletions();
        check(pendingChats.size() == 1 && pendingChats[0] == -100, "pending deletion chats dedup");
    }
    store.cancelMessageDeletion(-100, 70);
    store.cancelMessageDeletion(-100, 71);

    // ---- 管理员名册 ----
    store.replaceAdmins(-100, {{7, "creator", "Owner"}, {8, "administrator", "Admin"}}, 1000);
    {
        const auto groups = store.chatsForAdmin(8);
        check(groups.size() == 1 && groups[0].chatId == -100, "chatsForAdmin");
    }
    store.audit(-100, 8, "test.action", "detail");
    check(store.lastAuditAt(-100, "test.action") > 0, "audit timestamp");

    // ---- 退出后不再出现在活跃列表 ----
    store.markLeft(-100, 2000);
    check(store.listChats(10, 0, true).empty(), "left chat hidden");
    check(store.chatsForAdmin(8).empty(), "left chat hidden from admin list");

    testHealthServer();
    testCommandPolicies();
    testUpdateQueue();
    testQueueBackpressure();

    if (failures == 0) {
        std::cout << "warden_tests: all checks passed\n";
        return 0;
    }
    std::cerr << failures << " check(s) failed\n";
    return 1;
}

void testUpdateQueue() {
    boost::asio::io_context io;
    tgbot::BotClient bot("123:test");
    tgbot::Dispatcher dispatcher;
    std::vector<std::int64_t> seen;
    dispatcher.onAnyMessage(
        [&seen](tgbot::BotClient&, nlohmann::json msg) -> boost::asio::awaitable<void> {
            seen.push_back(msg.value("message_id", std::int64_t{0}));
            co_await tgbot::co_sleep(std::chrono::milliseconds{1});
        });

    tgbot::detail::UpdateWorkers workers(io.get_executor(), 100, 2);
    const auto update = [](std::int64_t chatId, std::int64_t msgId) {
        return nlohmann::json{{"update_id", msgId},
                              {"message", {{"message_id", msgId}, {"chat", {{"id", chatId}}}}}};
    };
    for (std::int64_t i = 1; i <= 3; ++i) {
        check(workers.dispatch(update(-100, i), bot, dispatcher), "queue accepts update");
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (dispatcher.inFlightUpdates() > 0 && std::chrono::steady_clock::now() < deadline) {
        io.restart();
        io.run_for(std::chrono::milliseconds{10});
    }
    check(dispatcher.inFlightUpdates() == 0, "queue drained");
    check((seen == std::vector<std::int64_t>{1, 2, 3}), "same chat update order");
}

void testQueueBackpressure() {
    boost::asio::io_context io;
    tgbot::BotClient bot("123:test");
    tgbot::Dispatcher dispatcher;
    int handled = 0;
    dispatcher.onAnyMessage(
        [&handled](tgbot::BotClient&, nlohmann::json) -> boost::asio::awaitable<void> {
            ++handled;
            co_return;
        });

    // workers 还没开始跑：第一条占满容量，第二条必须被拒绝。
    tgbot::detail::UpdateWorkers workers(io.get_executor(), 1, 1);
    const nlohmann::json update{{"update_id", 1},
                                {"message", {{"message_id", 1}, {"chat", {{"id", -100}}}}}};
    check(workers.dispatch(update, bot, dispatcher), "first update accepted");
    check(!workers.dispatch(update, bot, dispatcher), "second update rejected when full");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (dispatcher.inFlightUpdates() > 0 && std::chrono::steady_clock::now() < deadline) {
        io.restart();
        io.run_for(std::chrono::milliseconds{10});
    }
    check(handled == 1, "only accepted update handled");
    check(dispatcher.inFlightUpdates() == 0, "backpressure queue drained");
}

void testCommandPolicies() {
    const auto* help = warden::findCommandPolicy("help");
    check(help != nullptr, "policy help exists");
    check(help != nullptr && help->privateAllowed && !help->groupAllowed, "help private only");
    check(help != nullptr && !help->groupMenu, "help not in group menu");

    const auto* autodel = warden::findCommandPolicy("autodel");
    check(autodel != nullptr && autodel->groupAllowed && !autodel->privateAllowed &&
              autodel->groupAdminOnly,
          "autodel group admin only");

    const auto* panel = warden::findCommandPolicy("panel");
    check(panel != nullptr && panel->privateAllowed && !panel->groupAllowed, "panel private only");

    const auto* stats = warden::findCommandPolicy("stats");
    check(stats != nullptr && stats->ownerOnly && !stats->privateMenu, "stats owner only hidden");
}

int main() {
    try {
        return runTests();
    } catch (const std::exception& e) {
        std::cerr << "uncaught exception: " << e.what() << "\n";
        return 2;
    }
}
