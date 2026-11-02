// 最小自测：不引第三方测试框架，失败计数 > 0 即非 0 退出，配合 CTest。
//
// 覆盖三块最容易回归、又不需要网络的行为：
//   1. Dispatcher 的命令解析（含 /cmd@Bot 归一化与其他 bot 的命令过滤）
//   2. callback_data 的"最长前缀优先"匹配
//   3. RateLimiter 的时间槽（同一聊天 ≥1s、全局 ≥~34ms）——这条同时是
//      "先占槽后等待"修复的回归测试：旧实现在这里会测出 0 间隔

#include <tgbot/bot_client.hpp>
#include <tgbot/dispatcher.hpp>
#include <tgbot/grab_queue.hpp>
#include <tgbot/job_queue.hpp>
#include <tgbot/lottery_panel.hpp>
#include <tgbot/panel.hpp>
#include <tgbot/rate_limiter.hpp>
#include <tgbot/util.hpp>
#include <tgbot/amount.hpp>
#include <tgbot/chain_health.hpp>
#include <tgbot/reconciliation.hpp>
#include <tgbot/red_packet_batch.hpp>
#include <tgbot/red_packet_ledger.hpp>
#include <tgbot/red_packet_panel.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
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

void testCommandGuard() {
    std::printf("test: command guard\n");
    tgbot::BotClient bot("000000:TEST");
    tgbot::Dispatcher dispatcher;

    int calls = 0;
    dispatcher.onCommand(
        "admin", [&calls](tgbot::BotClient&, json) -> net::awaitable<void> {
            ++calls;
            co_return;
        });

    bool allow = false;
    dispatcher.setCommandGuard(
        [&allow](tgbot::BotClient&, json) -> net::awaitable<bool> {
            co_return allow;
        });

    runDispatch(dispatcher, bot, makeTextMessage("/admin"));
    CHECK(calls == 0);  // 守卫拒绝时 handler 不得执行

    allow = true;
    runDispatch(dispatcher, bot, makeTextMessage("/admin"));
    CHECK(calls == 1);
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

std::string squashSpaces(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
            out += c;
        }
    }
    return out;
}

// 整块必须是合法 UTF-8（块尾出现"半个汉字"就说明切错了位置）
bool validUtf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        if (c < 0x80) {
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        } else {
            return false;
        }
        if (i + len > s.size()) {
            return false;
        }
        for (std::size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += len;
    }
    return true;
}

void testRichText() {
    std::printf("test: rich text escaping / splitting\n");

    CHECK(tgbot::escapeHtml("a<b>&c") == "a&lt;b&gt;&amp;c");
    CHECK(tgbot::escapeHtml("plain") == "plain");
    CHECK(tgbot::escapeMarkdownV2("a_b*c.d!") == "a\\_b\\*c\\.d\\!");
    CHECK(tgbot::escapeMarkdownV2("abc") == "abc");

    CHECK(tgbot::parseModeName(tgbot::ParseMode::kNone) == nullptr);
    CHECK(std::string{tgbot::parseModeName(tgbot::ParseMode::kHtml)} == "HTML");
    CHECK(std::string{tgbot::parseModeName(tgbot::ParseMode::kMarkdownV2)} ==
          "MarkdownV2");

    // 纯文本分块：长度不超限、除空白外内容不丢
    std::string longText;
    for (int i = 0; i < 400; ++i) {
        longText += "第" + std::to_string(i) + "行：这是一段用于测试分块的中文内容\n";
    }
    const auto parts = tgbot::splitMessage(longText, 200);
    CHECK(parts.size() > 1);
    std::string joined;
    for (const auto& p : parts) {
        CHECK(p.size() <= 200);
        CHECK(!p.empty());
        // 整块必须是合法 UTF-8（块尾不能留半个汉字）
        CHECK(validUtf8(p));
        joined += p;
    }
    CHECK(squashSpaces(joined) == squashSpaces(longText));

    // HTML 分块：每段内部标签必须自平衡（<b> 与 </b> 数量相等）
    std::string html = "<b>";
    for (int i = 0; i < 300; ++i) {
        html += "加粗内容";
    }
    html += "</b>";
    const auto htmlParts =
        tgbot::splitMessage(html, 120, tgbot::ParseMode::kHtml);
    CHECK(htmlParts.size() > 1);
    for (const auto& p : htmlParts) {
        const auto opens = std::count(p.begin(), p.end(), '<');
        // <b> 与 </b> 各算一个 '<'，数量应为偶数
        CHECK(opens % 2 == 0);
        CHECK(p.find("<b>") != std::string::npos);
        CHECK(p.find("</b>") != std::string::npos);
    }
}


void testLotteryPanel() {
    std::printf("test: lottery panel state machine\n");

    tgbot::LotteryPanel::Config cfg;
    cfg.creatorId = 1;
    cfg.winnerCount = 1;
    cfg.duration = std::chrono::seconds{60};
    cfg.rewardPrivate = true;  // 这个用例专门验证"私密发码"分支
    tgbot::LotteryPanel panel(cfg);

    tgbot::PanelEvent join;
    join.action = "join";
    join.userId = 10;
    join.userName = "@alice";

    tgbot::PanelEffects fx;
    CHECK(panel.onClick(join, fx));            // 首次参与 → 需要刷新
    CHECK(panel.participantCount() == 1);
    CHECK(!fx.answer.empty());

    fx = {};
    CHECK(!panel.onClick(join, fx));           // 重复参与 → 不刷新
    CHECK(panel.participantCount() == 1);

    auto join2 = join;
    join2.userId = 11;
    join2.userName = "@bob";
    fx = {};
    CHECK(panel.onClick(join2, fx));
    CHECK(panel.participantCount() == 2);

    // 非发起者不能开奖
    auto drawByOther = join;
    drawByOther.action = "draw";
    fx = {};
    CHECK(!panel.onClick(drawByOther, fx));
    CHECK(fx.showAlert);
    CHECK(panel.winners().empty());

    // 发起者开奖 → 产生私密消息
    auto draw = drawByOther;
    draw.userId = 1;
    fx = {};
    CHECK(panel.onClick(draw, fx));
    CHECK(panel.closed());
    CHECK(panel.winners().size() == 1);
    CHECK(fx.privateMessages.size() == 1);
    CHECK(fx.privateMessages[0].userId == panel.winners()[0]);
    CHECK(!fx.privateMessages[0].text.empty());

    // 开奖后不能参与
    auto late = join;
    late.userId = 12;
    fx = {};
    CHECK(!panel.onClick(late, fx));
    CHECK(panel.participantCount() == 2);

    // 结束后渲染：显示已开奖、且不再有按钮
    const auto view = panel.render();
    CHECK(view.text.find("已开奖") != std::string::npos);
    CHECK(view.keyboard.is_null());

    // 倒计时到点自动开奖（只有 1 人参与，中奖数取 min）
    tgbot::LotteryPanel::Config autoCfg;
    autoCfg.duration = std::chrono::seconds{0};
    autoCfg.winnerCount = 3;
    autoCfg.rewardPrivate = true;
    tgbot::LotteryPanel autoPanel(autoCfg);
    tgbot::PanelEffects autoFx;
    CHECK(autoPanel.onClick(join, autoFx));
    autoFx = {};
    autoPanel.onDeadline(autoFx);
    CHECK(autoPanel.closed());
    CHECK(autoPanel.winners().size() == 1);
    CHECK(autoFx.privateMessages.size() == 1);

    // 默认（rewardPrivate=false）只在群里公布名单，不私下发码
    tgbot::LotteryPanel::Config plainCfg;  // creatorId=0 → 谁都能开奖
    tgbot::LotteryPanel plainPanel(plainCfg);
    tgbot::PanelEffects plainFx;
    CHECK(plainPanel.onClick(join, plainFx));
    plainFx = {};
    auto plainDraw = join;
    plainDraw.action = "draw";
    plainDraw.userId = 999;
    CHECK(plainPanel.onClick(plainDraw, plainFx));
    CHECK(!plainPanel.winners().empty());
    CHECK(plainFx.privateMessages.empty());
}

void testRedPacketLedger() {
    std::printf("test: red packet ledger (atomic grab + refund once)\n");

    auto ledger = std::make_shared<tgbot::RedPacketLedger>(":memory:");
    const auto id =
        ledger->create(-100, 1, 10000, 5, std::chrono::seconds{3600});

    auto row = ledger->get(id);
    CHECK(row.has_value());
    CHECK(row->remainingCents == 10000);
    CHECK(row->remainingCount == 5);
    CHECK(row->state == "open");

    // 余额不足：不扣
    CHECK(!ledger->grab(id, 10, "@a", 20000));
    CHECK(ledger->get(id)->remainingCents == 10000);

    CHECK(ledger->grab(id, 10, "@a", 1000));
    CHECK(ledger->get(id)->remainingCents == 9000);
    CHECK(!ledger->grab(id, 10, "@a", 1000));  // 同一人重复领
    CHECK(ledger->get(id)->remainingCents == 9000);

    // 退款权只可能被抢到一次
    CHECK(ledger->beginRefund(id));
    CHECK(!ledger->beginRefund(id));
    ledger->finishRefund(id, "tx-1");
    CHECK(ledger->get(id)->state == "refunded");
    CHECK(ledger->get(id)->txRef == "tx-1");
    CHECK(!ledger->beginRefund(id));

    // 抢完 → exhausted，再抢不到
    const auto id2 = ledger->create(-100, 1, 300, 2, std::chrono::seconds{60});
    CHECK(ledger->grab(id2, 20, "@b", 100));
    CHECK(ledger->grab(id2, 21, "@c", 200));
    CHECK(ledger->get(id2)->state == "exhausted");
    CHECK(!ledger->grab(id2, 22, "@d", 1));
}

void testRedPacketPanel() {
    std::printf("test: red packet panel (群内可见)\n");

    auto ledger = std::make_shared<tgbot::RedPacketLedger>(":memory:");
    const auto id = ledger->create(-100, 1, 10000, 5, std::chrono::seconds{3600});

    tgbot::RedPacketPanel::Options opts;
    opts.seed = 12345;  // 固定随机数，可复现
    tgbot::RedPacketPanel panel(ledger, id, opts);

    tgbot::PanelEvent ev;
    ev.action = "grab";

    // 发起人不能抢自己的红包
    ev.userId = 1;
    tgbot::PanelEffects fx;
    CHECK(!panel.onClick(ev, fx));
    CHECK(fx.showAlert);
    CHECK(ledger->grabs(id).empty());

    // 5 个人抢完：金额守恒
    for (int i = 0; i < 5; ++i) {
        ev.userId = 100 + i;
        ev.userName = "@user" + std::to_string(i);
        fx = {};
        CHECK(panel.onClick(ev, fx));
        CHECK(fx.answer.find("抢到") != std::string::npos);
    }
    std::int64_t sum = 0;
    for (const auto& grab : ledger->grabs(id)) {
        CHECK(grab.cents >= 1);
        sum += grab.cents;
    }
    CHECK(sum == 10000);
    CHECK(ledger->get(id)->state == "exhausted");
    CHECK(panel.finished());

    // 抢完之后再来的人
    ev.userId = 999;
    fx = {};
    CHECK(!panel.onClick(ev, fx));

    // 群里看到名单，且按钮消失
    const auto view = panel.render();
    CHECK(view.text.find("已抢完") != std::string::npos);
    CHECK(view.text.find("@user0") != std::string::npos);
    CHECK(view.keyboard.is_null());

    // 重复抢：金额和人数都不变
    const auto id2 = ledger->create(-100, 1, 500, 2, std::chrono::seconds{60});
    tgbot::RedPacketPanel repeat(ledger, id2, opts);
    ev.userId = 42;
    fx = {};
    CHECK(repeat.onClick(ev, fx));
    const auto afterFirst = ledger->grabs(id2);
    CHECK(afterFirst.size() == 1);
    const auto first = afterFirst.empty() ? -1 : afterFirst.front().cents;
    fx = {};
    CHECK(!repeat.onClick(ev, fx));
    CHECK(ledger->grabs(id2).size() == 1);
    const auto afterRepeat = ledger->grabs(id2);
    CHECK(!afterRepeat.empty() && afterRepeat.front().cents == first);
}

void testJobQueue() {
    std::printf("test: durable job queue (延迟 + 幂等 + 重试)\n");

    tgbot::JobQueue jobs(":memory:");

    tgbot::Job job;
    job.key = "refund:1";
    job.kind = "demo";
    job.payload = "{}";
    job.runAt = std::chrono::system_clock::now() - std::chrono::seconds{1};

    jobs.enqueue(job);
    jobs.enqueue(job);  // 幂等：同一个 key 不会重复入队
    CHECK(jobs.countPending() == 1);

    auto claimed = jobs.claimNext();
    CHECK(claimed.has_value());
    CHECK(claimed->key == "refund:1");
    CHECK(claimed->attempts == 1);
    CHECK(!jobs.claimNext().has_value());  // 已被领走，不会重复领

    // 失败后退避：短期内领不到
    jobs.markRetry(claimed->id, "boom");
    CHECK(jobs.countPending() == 1);
    CHECK(!jobs.claimNext().has_value());

    jobs.markDone(claimed->id);
    CHECK(jobs.countPending() == 0);
}

void testAmount() {
    std::printf("test: Amount（128 位，wei 级金额）\n");

    const auto oneEth = tgbot::Amount::parse("1000000000000000000");
    CHECK(oneEth.has_value());
    CHECK(oneEth->toString() == "1000000000000000000");

    // 1e19 wei 已经超过 int64（9.22e18），但还在 uint64 内
    const auto overInt64 = tgbot::Amount::parse("10000000000000000000");
    CHECK(overInt64.has_value());
    CHECK(overInt64->fitsUint64());

    // 2e19 连 uint64 都装不下，必须靠 128 位
    const auto big = tgbot::Amount::parse("20000000000000000000");
    CHECK(big.has_value());
    CHECK(!big->fitsUint64());
    CHECK(big->toString() == "20000000000000000000");

    CHECK((*oneEth + *oneEth).toString() == "2000000000000000000");
    CHECK((*big - *oneEth).toString() == "19000000000000000000");
    CHECK(oneEth->mulSmall(3).toString() == "3000000000000000000");

    // 拆分要用的除法
    const auto [quotient, remainder] = oneEth->divmod(3);
    CHECK(quotient.toString() == "333333333333333333");
    CHECK(remainder == 1);

    // 分 → 链上单位
    const auto ethWei = tgbot::Amount::fromCents(100, 18);  // ¥1.00
    CHECK(ethWei.has_value());
    CHECK(ethWei->toString() == "1000000000000000000");
    const auto usdt = tgbot::Amount::fromCents(100, 6);  // ¥1.00
    CHECK(usdt.has_value());
    CHECK(usdt->toString() == "1000000");

    // 非法与溢出
    CHECK(!tgbot::Amount::parse("").has_value());
    CHECK(!tgbot::Amount::parse("12a").has_value());
    CHECK(!tgbot::Amount::fromCents(100, 1).has_value());
    // 2^128 正好溢出
    CHECK(!tgbot::Amount::parse("340282366920938463463374607431768211456")
               .has_value());

    CHECK(*oneEth < *big);
    CHECK(*oneEth != *big);
    CHECK(*oneEth == *oneEth);
}

void testBatchRefund() {
    std::printf("test: 批量退款（一批只调一次 handler）\n");

    auto ledger = std::make_shared<tgbot::RedPacketLedger>(":memory:");
    // ttl=0 → 立即到期
    const auto a = ledger->create(-100, 1, 5000, 2, std::chrono::seconds{0});
    const auto b = ledger->create(-100, 2, 3000, 2, std::chrono::seconds{0});
    const auto c = ledger->create(-100, 3, 1000, 2, std::chrono::seconds{0});
    CHECK(ledger->grab(a, 10, "@x", 1000));  // a 被抢走一部分

    tgbot::ChainMonitor monitor;
    tgbot::RefundService service(*ledger, monitor,
                                 tgbot::RefundService::Options{});
    int calls = 0;
    std::vector<std::int64_t> seen;
    std::size_t flushed = 0;
    tgbot::Amount total;

    const tgbot::BatchRefundHandler handler =
        [&](const tgbot::RefundBatch& batch, tgbot::TxRefRecorder)
        -> boost::asio::awaitable<tgbot::RefundBatchResult> {
        ++calls;
        total = batch.total();
        for (const auto& item : batch.items) {
            seen.push_back(item.packetId);
        }
        tgbot::RefundBatchResult result;
        result.ref = "tx-" + batch.id;
        co_return result;
    };

    boost::asio::io_context io;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            flushed = co_await service.flush(handler);
        },
        boost::asio::detached);
    io.run();

    CHECK(calls == 1);  // 三个红包合并成一次
    CHECK(flushed == 3);
    CHECK(seen.size() == 3);
    CHECK(ledger->get(a)->state == "refunded");
    CHECK(ledger->get(b)->state == "refunded");
    CHECK(ledger->get(c)->state == "refunded");
    CHECK(!ledger->get(a)->txRef.empty());
    // 剩余 4000 + 3000 + 1000 = 8000 分 = 8e19 wei
    CHECK(total.toString() == "80000000000000000000");

    // 再冲一次：没有可退的了
    calls = 0;
    boost::asio::io_context io2;
    boost::asio::co_spawn(
        io2,
        [&]() -> boost::asio::awaitable<void> {
            flushed = co_await service.flush(handler);
        },
        boost::asio::detached);
    io2.run();
    CHECK(calls == 0);
    CHECK(flushed == 0);

    // 部分失败：失败那笔退回 expired，下一轮再退
    const auto d = ledger->create(-100, 4, 2000, 2, std::chrono::seconds{0});
    const auto e = ledger->create(-100, 5, 2000, 2, std::chrono::seconds{0});
    const tgbot::BatchRefundHandler failing =
        [&](const tgbot::RefundBatch& batch, tgbot::TxRefRecorder)
        -> boost::asio::awaitable<tgbot::RefundBatchResult> {
        (void)batch;
        tgbot::RefundBatchResult result;
        result.ref = "tx-partial";
        result.failedPacketIds.push_back(e);
        co_return result;
    };
    boost::asio::io_context io3;
    boost::asio::co_spawn(
        io3,
        [&]() -> boost::asio::awaitable<void> {
            co_await service.flush(failing);
        },
        boost::asio::detached);
    io3.run();
    CHECK(ledger->get(d)->state == "refunded");
    CHECK(ledger->get(e)->state == "expired");

    boost::asio::io_context io4;
    boost::asio::co_spawn(
        io4,
        [&]() -> boost::asio::awaitable<void> {
            co_await service.flush(handler);
        },
        boost::asio::detached);
    io4.run();
    CHECK(ledger->get(e)->state == "refunded");
}

void testChainMonitor() {
    std::printf("test: 熔断器（链/RPC 连续失败 → 暂停）\n");

    tgbot::ChainMonitor monitor;
    CHECK(!monitor.paused());

    for (int i = 0; i < 4; ++i) {
        monitor.recordFailure("rpc timeout");
    }
    CHECK(!monitor.paused());  // 还没到阈值
    CHECK(monitor.consecutiveFailures() == 4);

    monitor.recordFailure("rpc timeout");  // 第 5 次触发熔断
    CHECK(monitor.paused());
    CHECK(monitor.lastError() == "rpc timeout");

    monitor.recordSuccess();  // 恢复
    CHECK(!monitor.paused());
    CHECK(monitor.consecutiveFailures() == 0);
}

void testReconciler() {
    std::printf("test: 对账（按 tx hash 回查链上真实状态）\n");

    auto ledger = std::make_shared<tgbot::RedPacketLedger>(":memory:");
    tgbot::ChainMonitor monitor;
    tgbot::Reconciler::Options options;
    options.minAge = std::chrono::seconds{0};
    options.stuckAfterSeconds = 0;  // 立即视为超时
    tgbot::Reconciler reconciler(*ledger, monitor, options);

    // a: 链上已成功 → 应该标记 refunded
    const auto a = ledger->create(-100, 1, 1000, 2, std::chrono::seconds{0});
    CHECK(ledger->beginRefund(a));
    CHECK(ledger->setTxRef(a, "0xaaa"));

    // b: 链上失败 → 应该退回 expired 重试
    const auto b = ledger->create(-100, 2, 1000, 2, std::chrono::seconds{0});
    CHECK(ledger->beginRefund(b));
    CHECK(ledger->setTxRef(b, "0xbbb"));

    // c: 没有 tx hash（广播前崩了）→ 应该 stuck
    const auto c = ledger->create(-100, 3, 1000, 2, std::chrono::seconds{0});
    CHECK(ledger->beginRefund(c));

    const tgbot::ChainProbe probe =
        [](std::string_view ref) -> boost::asio::awaitable<tgbot::ChainTxStatus> {
        if (ref == "0xaaa") {
            co_return tgbot::ChainTxStatus::kConfirmed;
        }
        if (ref == "0xbbb") {
            co_return tgbot::ChainTxStatus::kFailed;
        }
        co_return tgbot::ChainTxStatus::kNotFound;
    };

    tgbot::ReconciliationReport report;
    boost::asio::io_context io;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            report = co_await reconciler.run(probe, {});
        },
        boost::asio::detached);
    io.run();

    CHECK(report.scanned == 3);
    CHECK(report.confirmed == 1);
    CHECK(report.requeued == 1);
    CHECK(report.stuck == 1);
    CHECK(ledger->get(a)->state == "refunded");
    CHECK(ledger->get(b)->state == "expired");
    CHECK(ledger->get(c)->state == "stuck");
}

void testGrabQueue() {
    std::printf("test: 抢红包队列（容量/去重/顺序/关闭）\n");

    tgbot::GrabQueue queue(3);
    CHECK(queue.capacity() == 3);

    CHECK(queue.submit(1, "@a") == tgbot::GrabQueue::Accept::kAccepted);
    CHECK(queue.submit(2, "@b") == tgbot::GrabQueue::Accept::kAccepted);
    CHECK(queue.submit(1, "@a") == tgbot::GrabQueue::Accept::kDuplicate);
    CHECK(queue.submit(3, "@c") == tgbot::GrabQueue::Accept::kAccepted);
    // 第 4 个人：名额已满，当场拒绝（不让他白花手续费）
    CHECK(queue.submit(4, "@d") == tgbot::GrabQueue::Accept::kFull);
    CHECK(queue.accepted() == 3);

    // FIFO，seq 递增
    const auto first = queue.pop();
    CHECK(first.has_value() && first->userId == 1 && first->seq == 1);
    const auto second = queue.pop();
    CHECK(second.has_value() && second->userId == 2 && second->seq == 2);
    const auto third = queue.pop();
    CHECK(third.has_value() && third->userId == 3 && third->seq == 3);
    CHECK(!queue.pop().has_value());

    // 关闭后一律拒绝
    tgbot::GrabQueue closed(5);
    closed.close();
    CHECK(closed.submit(9) == tgbot::GrabQueue::Accept::kClosed);
}

void testGrabRunner() {
    std::printf("test: 抢红包批量执行（并发上限 + 顺序渲染）\n");

    tgbot::GrabQueue queue(10);
    for (std::int64_t i = 0; i < 10; ++i) {
        CHECK(queue.submit(i + 100, "@u" + std::to_string(i)) ==
              tgbot::GrabQueue::Accept::kAccepted);
    }

    int inFlight = 0;
    int maxInFlight = 0;
    std::vector<std::uint64_t> renderedSeq;

    tgbot::GrabRunner runner(tgbot::GrabRunner::Options{5});
    const auto submit =
        [&](tgbot::GrabRequest request) -> boost::asio::awaitable<tgbot::GrabResult> {
        ++inFlight;
        maxInFlight = std::max(maxInFlight, inFlight);
        // 让后提交的反而先完成，验证"按 seq 渲染"
        co_await tgbot::co_sleep(std::chrono::milliseconds{20 - request.seq * 2});
        --inFlight;

        tgbot::GrabResult result;
        result.seq = request.seq;
        result.userId = request.userId;
        result.ok = true;
        result.detail = "10";
        result.fromNetwork = true;
        co_return result;
    };
    const auto render = [&](std::vector<tgbot::GrabResult> results)
        -> boost::asio::awaitable<void> {
        for (const auto& result : results) {
            renderedSeq.push_back(result.seq);
            CHECK(result.ok);
        }
        co_return;
    };

    std::size_t processed = 0;
    boost::asio::io_context io;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            processed = co_await runner.drain(queue, submit, render);
        },
        boost::asio::detached);
    io.run();

    CHECK(processed == 10);
    CHECK(renderedSeq.size() == 10);
    // 渲染顺序必须严格递增（等于点击顺序）
    for (std::size_t i = 1; i < renderedSeq.size(); ++i) {
        CHECK(renderedSeq[i] > renderedSeq[i - 1]);
    }
    // 并发不超过配置上限
    CHECK(maxInFlight <= 5);
    CHECK(maxInFlight >= 2);  // 确实并发了
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testCommandParsing();
    testCommandGuard();
    testCallbackPrefix();
    testRateLimiterSpacing();
    testPermanentErrorClassifier();
    testRichText();
    testLotteryPanel();
    testRedPacketLedger();
    testRedPacketPanel();
    testJobQueue();
    testAmount();
    testBatchRefund();
    testChainMonitor();
    testReconciler();
    testGrabQueue();
    testGrabRunner();

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
