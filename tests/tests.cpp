// 最小自测：不引第三方测试框架，失败计数 > 0 即非 0 退出，配合 CTest。
//
// 覆盖三块最容易回归、又不需要网络的行为：
//   1. Dispatcher 的命令解析（含 /cmd@Bot 归一化与其他 bot 的命令过滤）
//   2. callback_data 的"最长前缀优先"匹配
//   3. RateLimiter 的时间槽（同一聊天 ≥1s、全局 ≥~34ms）——这条同时是
//      "先占槽后等待"修复的回归测试：旧实现在这里会测出 0 间隔

#include <tgbot/bot_client.hpp>
#include <tgbot/dispatcher.hpp>
#include <tgbot/rate_limiter.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace net = boost::asio;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const char* expr, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("  FAIL  line %d: %s\n", line, expr);
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

json makeTextMessage(std::string text, std::int64_t chatId = -100123) {
    return json{{"update_id", 1},
                {"message",
                 {{"message_id", 5},
                  {"from", {{"id", 42}, {"is_bot", false}}},
                  {"chat", {{"id", chatId}, {"type", "supergroup"}}},
                  {"text", std::move(text)}}}};
}

json makeCallback(std::string data) {
    return json{
        {"update_id", 2},
        {"callback_query",
         {{"id", "cb1"},
          {"from", {{"id", 7}}},
          {"data", std::move(data)},
          {"message", {{"message_id", 9}, {"chat", {{"id", 7}}}}}}}};
}

void runDispatch(tgbot::Dispatcher& dispatcher, tgbot::BotClient& bot,
                 json update) {
    net::io_context io;
    net::co_spawn(io, dispatcher.dispatch(std::move(update), bot),
                  net::detached);
    io.run();
}

void testCommandParsing() {
    std::printf("test: command parsing\n");
    tgbot::BotClient bot("000000:TEST");
    tgbot::Dispatcher dispatcher;
    dispatcher.setBotUsername("MyBot");

    std::vector<std::string> seen;
    dispatcher.onCommand(
        "start", [&seen](tgbot::BotClient&, json msg) -> net::awaitable<void> {
            seen.push_back(msg.value("text", std::string{}));
            co_return;
        });
    dispatcher.onAnyMessage(
        [&seen](tgbot::BotClient&, json msg) -> net::awaitable<void> {
            seen.push_back("ANY:" + msg.value("text", std::string{}));
            co_return;
        });

    runDispatch(dispatcher, bot, makeTextMessage("/start"));
    runDispatch(dispatcher, bot, makeTextMessage("/start@MyBot a b"));
    runDispatch(dispatcher, bot, makeTextMessage("/start@OtherBot x"));
    runDispatch(dispatcher, bot, makeTextMessage("hello"));

    CHECK(seen.size() == 3);  // 发给 OtherBot 的那条被完全忽略
    CHECK(seen[0] == "/start");
    CHECK(seen[1] == "/start a b");  // @Bot 后缀被归一化掉
    CHECK(seen[2] == "ANY:hello");
}

void testCallbackPrefix() {
    std::printf("test: callback prefix matching\n");
    tgbot::BotClient bot("000000:TEST");
    tgbot::Dispatcher dispatcher;

    std::string hit;
    dispatcher.onCallbackQuery(
        "menu:", [&hit](tgbot::BotClient&, json) -> net::awaitable<void> {
            hit = "menu:";
            co_return;
        });
    dispatcher.onCallbackQuery(
        "menu:sub:", [&hit](tgbot::BotClient&, json) -> net::awaitable<void> {
            hit = "menu:sub:";
            co_return;
        });

    runDispatch(dispatcher, bot, makeCallback("menu:sub:x"));
    CHECK(hit == "menu:sub:");  // 最长前缀优先

    hit.clear();
    runDispatch(dispatcher, bot, makeCallback("other"));
    CHECK(hit.empty());  // 没有匹配不应触发任何 handler
}

std::vector<std::chrono::steady_clock::time_point> runBurst(
    tgbot::RateLimiter& limiter, const std::vector<std::int64_t>& chats) {
    net::io_context io;
    std::vector<std::chrono::steady_clock::time_point> stamps;
    int pending = static_cast<int>(chats.size());

    for (const auto chat : chats) {
        net::co_spawn(
            io,
            [&limiter, chat, &stamps]() -> net::awaitable<void> {
                co_await limiter.beforeSend(chat);
                stamps.push_back(std::chrono::steady_clock::now());
            },
            [&pending, &io](std::exception_ptr e) {
                if (e) {
                    std::printf("  FAIL  beforeSend threw\n");
                    ++g_failed;
                }
                if (--pending == 0) {
                    io.stop();
                }
            });
    }
    io.run();
    std::sort(stamps.begin(), stamps.end());
    return stamps;
}

void testRateLimiterSpacing() {
    std::printf("test: rate limiter slot spacing\n");
    {
        // 同一聊天 3 条突发：应被 1s 间隔拉开（旧实现会同时放行）
        tgbot::RateLimiter limiter;
        const auto stamps = runBurst(limiter, {100, 100, 100});
        CHECK(stamps.size() == 3);
        for (std::size_t i = 1; i < stamps.size(); ++i) {
            const auto gap = stamps[i] - stamps[i - 1];
            CHECK(gap >= 900ms);
        }
    }
    {
        // 不同聊天 5 条突发：受全局 ~34ms 间隔约束
        tgbot::RateLimiter limiter;
        const auto stamps = runBurst(limiter, {1, 2, 3, 4, 5});
        CHECK(stamps.size() == 5);
        for (std::size_t i = 1; i < stamps.size(); ++i) {
            const auto gap = stamps[i] - stamps[i - 1];
            CHECK(gap >= 20ms);
        }
    }
}

void testPermanentErrorClassifier() {
    std::printf("test: permanent error classifier\n");
    CHECK(tgbot::isPermanentError(401));
    CHECK(tgbot::isPermanentError(403));
    CHECK(tgbot::isPermanentError(404));
    CHECK(!tgbot::isPermanentError(409));  // 冲突是"可恢复"的
    CHECK(!tgbot::isPermanentError(429));
    CHECK(!tgbot::isPermanentError(500));
}

}  // namespace

int main() {
    testCommandParsing();
    testCallbackPrefix();
    testRateLimiterSpacing();
    testPermanentErrorClassifier();

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
