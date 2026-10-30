// 群管理机器人示例（基于 tgbot 框架）
//
// 功能：
// 🧹 贴纸自动删除：开启后，群里任何人发的贴纸会在设定时间后自动删除
//    （默认 60 秒）。/sticker on|off|delay <秒>|查状态（变更仅群管理员）。
// 🙋 新成员欢迎：/welcome on 后以 ephemeral 私密消息欢迎新成员（仅本人可见，
//    不刷屏）；/welcome set <文案> 自定义（{name}/{group} 占位符）。
// 🤐 静默模式：/silent on 修改群默认权限实现全员禁言（非管理员输入框被禁用），
//    /silent allow 回复某人放行、deny 收回、list 查白名单；开启前自动快照
//    原权限，/silent off 精确恢复。需要 bot 有"限制成员"管理员权限。
// 🎁 ephemeral 演示：/card 公开卡片 + 私人结果、/eph、/myid。
//    设置持久化到 group_admin_state.db（SQLite），重启不丢。
// 清理：群里的命令消息立即删除，机器人的回复在 20 秒后删除，避免刷屏。
// 私聊：/start、/help 查看功能介绍。
//
// 用法：./group_admin <bot-token>   或   TG_BOT_TOKEN=... ./group_admin
// 注意：机器人需要被设为群管理员（"删除消息"，静默模式还需要"限制成员"），
//       否则对应功能会失败并在日志中给出具体原因。

#include <tgbot/tg_bot.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace net = boost::asio;
using nlohmann::json;
using tgbot::BotClient;

namespace {

constexpr auto kDefaultStickerDelay = std::chrono::seconds{60};
constexpr long kMinDelaySec = 5;
constexpr long kMaxDelaySec = 3600;
constexpr auto kGroupMsgTtl = std::chrono::seconds{20};  // 群内命令消息/回复的存活时间
constexpr const char* kStateDb = "group_admin_state.db";

// 默认欢迎词模板；{name} = 新成员称呼（@用户名或名字），{group} = 群名
constexpr const char* kDefaultWelcome =
    "欢迎 {name} 加入「{group}」🎉\n"
    "本群由我协助管理：贴纸超时会自动清理，福利卡片点按钮领取"
    "（仅本人可见）。\n"
    "私聊我发 /help 可以看我的全部功能～";

// 每个群的功能开关（贴纸删除 + 新成员欢迎），基于 StateStore 键值存储
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

    // 新成员欢迎语开关（默认关闭，管理员 /welcome on 开启）
    bool welcomeEnabled(std::int64_t chatId) const {
        return store_.get(welcomeKey(chatId)).has_value();
    }

    void setWelcome(std::int64_t chatId, bool enabled) {
        if (enabled) {
            store_.set(welcomeKey(chatId), "1");
        } else {
            store_.erase(welcomeKey(chatId));
        }
    }

    // 自定义欢迎词；未设置返回 nullopt（用默认模板）
    std::optional<std::string> welcomeText(std::int64_t chatId) const {
        return store_.get(welcomeTextKey(chatId));
    }

    void setWelcomeText(std::int64_t chatId, std::string text) {
        store_.set(welcomeTextKey(chatId), std::move(text));
    }

    void resetWelcomeText(std::int64_t chatId) {
        store_.erase(welcomeTextKey(chatId));
    }

    // ---- 静默模式（全员禁言）----
    bool silentEnabled(std::int64_t chatId) const {
        return store_.get(silentKey(chatId)).has_value();
    }

    void setSilent(std::int64_t chatId, bool enabled) {
        if (enabled) {
            store_.set(silentKey(chatId), "1");
        } else {
            store_.erase(silentKey(chatId));
        }
    }

    // 白名单：静默模式下仍被允许发言的用户 id 列表
    std::vector<std::int64_t> silentAllowlist(std::int64_t chatId) const {
        const auto v = store_.get(silentAllowKey(chatId));
        if (!v) {
            return {};
        }
        std::vector<std::int64_t> ids;
        try {
            for (const auto& e : json::parse(*v)) {
                if (e.is_number_integer()) {
                    ids.push_back(e.get<std::int64_t>());
                }
            }
        } catch (const std::exception&) {
            // 数据异常按空名单处理
        }
        return ids;
    }

    void addSilentAllow(std::int64_t chatId, std::int64_t userId) {
        auto ids = silentAllowlist(chatId);
        if (std::find(ids.begin(), ids.end(), userId) == ids.end()) {
            ids.push_back(userId);
            store_.set(silentAllowKey(chatId), json(ids).dump());
        }
    }

    void removeSilentAllow(std::int64_t chatId, std::int64_t userId) {
        auto ids = silentAllowlist(chatId);
        const auto it = std::find(ids.begin(), ids.end(), userId);
        if (it != ids.end()) {
            ids.erase(it);
            store_.set(silentAllowKey(chatId), json(ids).dump());
        }
    }

    // 静默开启前的群默认权限快照（关闭时原样恢复）
    std::optional<std::string> silentSnapshot(std::int64_t chatId) const {
        return store_.get(silentSnapKey(chatId));
    }

    void setSilentSnapshot(std::int64_t chatId, std::string snapshot) {
        store_.set(silentSnapKey(chatId), std::move(snapshot));
    }

    void clearSilentSnapshot(std::int64_t chatId) {
        store_.erase(silentSnapKey(chatId));
    }

private:
    static std::string key(std::int64_t chatId) {
        return "sticker_expire:" + std::to_string(chatId);
    }
    static std::string delayKey(std::int64_t chatId) {
        return "sticker_delay:" + std::to_string(chatId);
    }
    static std::string welcomeKey(std::int64_t chatId) {
        return "welcome_enabled:" + std::to_string(chatId);
    }
    static std::string welcomeTextKey(std::int64_t chatId) {
        return "welcome_text:" + std::to_string(chatId);
    }
    static std::string silentKey(std::int64_t chatId) {
        return "silent:" + std::to_string(chatId);
    }
    static std::string silentAllowKey(std::int64_t chatId) {
        return "silent_allow:" + std::to_string(chatId);
    }
    static std::string silentSnapKey(std::int64_t chatId) {
        return "silent_snap:" + std::to_string(chatId);
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

// 延时删除一条消息。独立协程执行（不占 chat strand），到点删除，
// 失败（已被删/太老/无权限）只记日志不打扰聊天。
void scheduleDelete(BotClient& bot, std::int64_t chatId,
                    std::int64_t messageId, std::chrono::seconds delay,
                    const net::any_io_executor& executor,
                    const char* what) {
    net::co_spawn(
        executor,
        [&bot, chatId, messageId, delay, what]() -> net::awaitable<void> {
            co_await tgbot::co_sleep(delay);
            try {
                co_await bot.deleteMessage(chatId, messageId);
                spdlog::info("{} expired: chat={} message_id={}", what, chatId,
                             messageId);
            } catch (const std::exception& e) {
                spdlog::warn("delete {} failed: chat={} message_id={}: {}", what,
                             chatId, messageId, e.what());
            }
        },
        net::detached);
}

// 回复命令。群里：用户命令消息立即删除，bot 回复在 kGroupMsgTtl 后删除，
// 避免群被命令刷屏。私聊不清理。
net::awaitable<void> replyAndSweep(BotClient& bot, const json& msg,
                                   const std::string& text) {
    const auto chatId = msg["chat"].value("id", std::int64_t{0});
    const auto chatType = msg["chat"].value("type", std::string{});
    const auto sent = co_await bot.sendMessage(chatId, text);
    if (chatType != "group" && chatType != "supergroup") {
        co_return;
    }
    // 先删用户的命令消息（立刻）
    const auto cmdMsgId = msg.value("message_id", std::int64_t{0});
    try {
        co_await bot.deleteMessage(chatId, cmdMsgId);
        spdlog::info("command swept: chat={} message_id={}", chatId, cmdMsgId);
    } catch (const std::exception& e) {
        spdlog::warn("delete command failed: chat={} message_id={}: {}", chatId,
                     cmdMsgId, e.what());
    }
    // bot 的回复 20 秒后再删
    const auto executor = co_await net::this_coro::executor;
    const auto replyMsgId = sent.value("message_id", std::int64_t{0});
    if (replyMsgId != 0) {
        scheduleDelete(bot, chatId, replyMsgId, kGroupMsgTtl, executor, "reply");
    }
}

// ---- Ephemeral 演示辅助（Bot API 10.2+）----

// 每人每天一个确定性专属码（演示用，同一人当天刷新码不变）
std::string dailyCode(std::int64_t userId) {
    const auto days = std::chrono::duration_cast<std::chrono::hours>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count() /
                      24;
    auto h = static_cast<unsigned long long>(userId) * 2654435761ULL;
    h ^= static_cast<unsigned long long>(days);
    return std::to_string(static_cast<long>(h % 900000ULL) + 100000L);
}

// ephemeral 消息上的操作键盘：刷新 = 编辑文本，销毁 = 删除 ephemeral
json ephActionKb() {
    return {{"inline_keyboard",
             json::array({{{{"text", "🔄 刷新"},
                             {"callback_data", "eph:refresh"}},
                            {{"text", "🗑 销毁"},
                             {"callback_data", "eph:destroy"}}}})}};
}

void replaceAllInplace(std::string& s, const std::string& from,
                       const std::string& to) {
    if (from.empty()) {
        return;
    }
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// 渲染欢迎词模板：{name} = 新成员称呼，{group} = 群名
std::string renderWelcome(std::string tpl, const std::string& name,
                          const std::string& group) {
    replaceAllInplace(tpl, "{name}", name);
    replaceAllInplace(tpl, "{group}", group);
    return tpl;
}

// ---- 静默模式（硬禁言）----

// 静默开启后的群默认权限：全员禁言，只保留邀请（can_send_messages=false
// 会连带关闭所有发送类权限）
json mutedPerms() {
    return {{"can_send_messages", false}, {"can_invite_users", true}};
}

// 白名单成员权限：完整放行发言
json allowedPerms() {
    return {{"can_send_messages", true}};
}

// 无快照可用时的兜底恢复权限（正常路径用开启前快照，不会走到这里）
json defaultOpenPerms() {
    return {{"can_send_messages", true},
            {"can_send_polls", true},
            {"can_invite_users", true},
            {"can_pin_messages", true}};
}

// 从"回复某人"的消息里取目标用户 id（bot 不算）；取不到返回 0
std::int64_t targetFromReply(const json& msg) {
    const auto rt = msg.find("reply_to_message");
    if (rt == msg.end() || !rt->is_object()) {
        return 0;
    }
    const auto from = rt->find("from");
    if (from == rt->end() || !from->is_object() ||
        from->value("is_bot", false)) {
        return 0;
    }
    return from->value("id", std::int64_t{0});
}

std::string joinIds(const std::vector<std::int64_t>& ids) {
    std::string s;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) {
            s += ", ";
        }
        s += std::to_string(ids[i]);
    }
    return s;
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
        co_await bot.setMyCommands(std::vector<tgbot::BotCommand>{
            {"start", "开始使用 / 功能介绍", false},
            {"help", "这个机器人能做什么", false},
            {"sticker", "贴纸自动删除：on/off/delay/无参数查状态", false},
            {"welcome", "新成员欢迎：on/off/set <文案>/reset/查状态（群管理员）", false},
            {"silent", "静默模式：全员禁言 on/off/allow/deny/list（群管理员）", false},
            {"card", "发福利卡片，点按钮领你的专属码（仅你可见）", false},
            {"eph", "给自己发一条仅你可见的消息", false},
            {"myid", "查你的用户 ID（隐身命令，别人看不到）", true},
        });
    } catch (const std::exception& e) {
        spdlog::warn("setMyCommands failed: {}", e.what());
    }

    tgbot::SqliteStateStore store(kStateDb);
    GroupState state(store);

    tgbot::Dispatcher dispatcher;

    // /start /help —— 欢迎语 + 功能总览（群里同样可用，但会被 20 秒清理）
    const auto helpText =
        std::string{
            "你好，我是群管理机器人 🤖\n"
            "把我拉进群、设为管理员（勾选“删除消息”）后，我可以帮你：\n"
            "\n"
            "🧹 群内容管理\n"
            "• /sticker on / off —— 开关贴纸自动删除（群管理员）\n"
            "• /sticker delay <秒> —— 贴纸删除时限，5–3600 秒（群管理员）\n"
            "• /sticker —— 查看本群状态（开关/时限/我的权限）\n"
            "\n"
            "🙋 新成员欢迎（私密送达，仅新成员本人可见，不刷屏）\n"
            "• /welcome on / off —— 开关欢迎语（群管理员，默认关闭）\n"
            "• /welcome set <文案> —— 自定义欢迎词（{name}=对方，{group}=群名）\n"
            "• /welcome —— 查看欢迎语状态\n"
            "\n"
            "🤐 静默模式（全员禁言，需要我有\"限制成员\"权限）\n"
            "• /silent on / off —— 只留管理员发言（群管理员）\n"
            "• /silent allow / deny —— 回复某人放行或收回发言\n"
            "• /silent —— 查看静默状态和白名单\n"
            "\n"
            "🎁 群内私密消息（ephemeral，只有你自己能看到）\n"
            "• /card —— 发公开福利卡片，点按钮领取你的专属码\n"
            "• /eph <文字> —— 给自己发一条私密消息\n"
            "• /myid —— 隐身命令：查你的用户 ID，群里其他人看不到\n"
            "\n"
            "小规则：群里的命令消息会立即清理，我的回复 20 秒后自动删除。"};

    // bot 被拉进群时的自我介绍：让管理员一眼知道怎么开功能。这条不清理。
    const auto introText =
        std::string{
            "大家好，我是群管理机器人 🤖\n"
            "🧹 群内容管理：管理员发 /sticker on 开启贴纸自动删除"
            "（默认 60 秒，/sticker delay <秒> 可改），/sticker 查看状态\n"
            "🙋 新成员欢迎：/welcome on 开启（私密送达，仅本人可见），"
            "/welcome set 定制文案\n"
            "🤐 静默模式：/silent on 全员禁言（需给我\"限制成员\"权限），"
            "回复某人 /silent allow 放行\n"
            "🎁 群内私密消息：/card 发福利卡片（点按钮只有本人可见）、"
            "/myid 隐身查 ID\n"
            "私聊我发 /help 看全部功能。需要给我“删除消息”权限才能干活～"};
    dispatcher.onCommand("start",
                         [helpText](BotClient& bot,
                                    const json msg) -> net::awaitable<void> {
                             co_await replyAndSweep(bot, msg, helpText);
                         });
    dispatcher.onCommand("help",
                         [helpText](BotClient& bot,
                                    const json msg) -> net::awaitable<void> {
                             co_await replyAndSweep(bot, msg, helpText);
                         });

    // /sticker —— 无参数：状态面板（任何人可用）；on/off/delay：仅群管理员
    dispatcher.onCommand("sticker",
                         [&state, botId](BotClient& bot,
                                  const json msg) -> net::awaitable<void> {
        const auto chatId = msg["chat"].value("id", std::int64_t{0});
        const auto chatType =
            msg["chat"].value("type", std::string{});
        const auto reply = [&bot,
                            msg](const std::string& text) -> net::awaitable<void> {
            co_await replyAndSweep(bot, msg, text);
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

    // /welcome —— 无参数：欢迎语状态（任何人）；on/off/set/reset：仅群管理员
    // 欢迎消息以 ephemeral 私密投递给新成员本人，不刷屏；支持自定义文案。
    dispatcher.onCommand("welcome",
                         [&state](BotClient& bot,
                                  const json msg) -> net::awaitable<void> {
        const auto chatId = msg["chat"].value("id", std::int64_t{0});
        const auto chatType =
            msg["chat"].value("type", std::string{});

        if (chatType != "group" && chatType != "supergroup") {
            co_await replyAndSweep(bot, msg, "这个命令只能在群里用。");
            co_return;
        }

        const std::string text = msg.value("text", "");
        const auto sp = text.find(' ');
        const std::string arg = sp == std::string::npos ? "" : text.substr(sp + 1);
        const auto groupTitle =
            msg["chat"].value("title", std::string{"本群"});

        // 无参数：状态面板（任何人可用）
        if (arg.empty()) {
            const auto custom = state.welcomeText(chatId);
            co_await replyAndSweep(
                bot, msg,
                std::string{"新成员欢迎语：" +
                            (state.welcomeEnabled(chatId)
                                 ? std::string{"已开启 ✅（私密送达，仅新成员本人可见）"}
                                 : std::string{"已关闭（默认）"}) +
                            "\n文案：" +
                            (custom ? std::string{"自定义（/welcome reset 恢复默认）"}
                                    : std::string{"默认模板"}) +
                            "\n管理员：/welcome on|off|set <文案>|reset"});
            co_return;
        }

        // 以下变更操作仅群管理员可用
        const auto userId = msg["from"].value("id", std::int64_t{0});
        if (!co_await isGroupAdmin(bot, chatId, userId)) {
            co_await replyAndSweep(bot, msg, "只有群管理员可以修改设置。");
            co_return;
        }

        if (arg == "on") {
            state.setWelcome(chatId, true);
            co_await replyAndSweep(bot, msg,
                                   "已开启：新成员进群时我会私密发欢迎消息"
                                   "（仅本人可见，不刷屏）。");
            co_return;
        }
        if (arg == "off") {
            state.setWelcome(chatId, false);
            co_await replyAndSweep(bot, msg, "已关闭：不再发欢迎消息。");
            co_return;
        }
        if (arg == "reset") {
            state.resetWelcomeText(chatId);
            co_await replyAndSweep(bot, msg, "已恢复默认欢迎词。");
            co_return;
        }
        // /welcome set <文案> —— 自定义欢迎词，{name}/{group} 为占位符
        if (arg.rfind("set", 0) == 0 && arg.size() > 3 && arg[3] == ' ') {
            auto tpl = arg.substr(4);
            // 去掉文案前多余的空格
            while (!tpl.empty() && tpl.front() == ' ') {
                tpl.erase(tpl.begin());
            }
            constexpr std::size_t kMaxTpl = 1000;
            if (tpl.empty() || tpl.size() > kMaxTpl) {
                co_await replyAndSweep(
                    bot, msg,
                    "用法：/welcome set <欢迎词>，1–1000 字，"
                    "可用占位符：{name}=新成员称呼，{group}=群名");
                co_return;
            }
            state.setWelcomeText(chatId, tpl);
            co_await replyAndSweep(
                bot, msg,
                "已设置自定义欢迎词 ✅（仅新成员本人可见）\n预览：\n" +
                    renderWelcome(tpl, "@示例用户", groupTitle));
            co_return;
        }
        co_await replyAndSweep(
            bot, msg,
            "用法：/welcome on|off|set <文案>|reset，或 /welcome 查看状态");
    });

    // /silent —— 静默模式（硬禁言：改群默认权限，非管理员输入框直接被禁用）
    // 无参数/list：查状态（任何人）；on/off/allow/deny：仅群管理员
    dispatcher.onCommand(
        "silent", [&state](BotClient& bot,
                           const json msg) -> net::awaitable<void> {
            const auto chatId = msg["chat"].value("id", std::int64_t{0});
            const auto chatType =
                msg["chat"].value("type", std::string{});
            if (chatType != "group" && chatType != "supergroup") {
                co_await replyAndSweep(bot, msg, "这个命令只能在群里用。");
                co_return;
            }

            const std::string text = msg.value("text", "");
            const auto sp = text.find(' ');
            const std::string arg =
                sp == std::string::npos ? "" : text.substr(sp + 1);
            // 二级参数（allow/deny 后面的用户 ID）
            const auto sp2 = arg.find(' ');
            const std::string sub =
                sp2 == std::string::npos ? "" : arg.substr(sp2 + 1);

            // 无参数：状态面板（任何人可用）
            if (arg.empty()) {
                const auto ids = state.silentAllowlist(chatId);
                co_await replyAndSweep(
                    bot, msg,
                    std::string{"静默模式：" +
                                (state.silentEnabled(chatId)
                                     ? std::string{"已开启 🔇 只有管理员和白名单成员能发言"}
                                     : std::string{"已关闭"}) +
                                "\n发言放行：" +
                                std::to_string(ids.size()) + " 人" +
                                (ids.empty() ? "" : "（" + joinIds(ids) + "）") +
                                "\n管理员：/silent on|off|allow|deny"});
                co_return;
            }

            if (arg == "list") {
                const auto ids = state.silentAllowlist(chatId);
                co_await replyAndSweep(
                    bot, msg, ids.empty()
                                  ? "静默白名单为空。"
                                  : "静默白名单（" + std::to_string(ids.size()) +
                                        " 人）：" + joinIds(ids));
                co_return;
            }

            // 以下变更操作仅群管理员可用
            const auto userId = msg["from"].value("id", std::int64_t{0});
            if (!co_await isGroupAdmin(bot, chatId, userId)) {
                co_await replyAndSweep(bot, msg, "只有群管理员可以修改设置。");
                co_return;
            }

            if (arg == "on") {
                if (state.silentEnabled(chatId)) {
                    co_await replyAndSweep(bot, msg,
                                           "静默模式已经是开启状态。");
                    co_return;
                }
                // 1) 快照当前群默认权限（拿不到就不继续，避免无法恢复）
                std::string snap;
                std::string failMsg;
                try {
                    const auto chat = co_await bot.getChat(chatId);
                    snap =
                        chat.value("permissions", json::object()).dump();
                } catch (const std::exception& e) {
                    failMsg = e.what();
                }
                if (!failMsg.empty()) {
                    co_await replyAndSweep(
                        bot, msg,
                        std::string{"开启失败：读取群权限出错（需要我是群"
                                    "管理员）："} +
                            failMsg);
                    co_return;
                }
                // 2) 全员禁言
                try {
                    co_await bot.setChatPermissions(chatId, mutedPerms());
                } catch (const std::exception& e) {
                    failMsg = e.what();
                }
                if (!failMsg.empty()) {
                    co_await replyAndSweep(
                        bot, msg,
                        std::string{"开启失败（需要我有“限制成员”权限）："} +
                            failMsg);
                    co_return;
                }
                state.setSilentSnapshot(chatId, snap);
                state.setSilent(chatId, true);
                // 3) 对白名单成员恢复个人放行（覆盖群默认禁言）
                for (const auto id : state.silentAllowlist(chatId)) {
                    try {
                        co_await bot.restrictChatMember(chatId, id,
                                                        allowedPerms());
                    } catch (const std::exception& e) {
                        spdlog::warn("whitelist re-apply {} in {} failed: {}",
                                     id, chatId, e.what());
                    }
                }
                co_await replyAndSweep(
                    bot, msg,
                    "已开启静默 🔇 现在只有管理员能发言。\n"
                    "放行指定成员：回复 TA 的消息发 /silent allow；"
                    "恢复：/silent off");
                co_return;
            }

            if (arg == "off") {
                if (!state.silentEnabled(chatId)) {
                    co_await replyAndSweep(bot, msg,
                                           "静默模式当前没有开启。");
                    co_return;
                }
                // 1) 恢复原群默认权限（快照缺失时用兜底默认）
                json restore = defaultOpenPerms();
                const auto snap = state.silentSnapshot(chatId);
                if (snap) {
                    try {
                        const auto parsed = json::parse(*snap);
                        if (parsed.is_object()) {
                            restore = parsed;
                        }
                    } catch (const std::exception&) {
                        // 快照损坏，用兜底默认
                    }
                }
                std::string failMsg;
                try {
                    co_await bot.setChatPermissions(chatId, restore);
                } catch (const std::exception& e) {
                    failMsg = e.what();
                }
                // 2) 解除白名单成员的个人放行
                for (const auto id : state.silentAllowlist(chatId)) {
                    try {
                        co_await bot.restrictChatMember(chatId, id,
                                                        defaultOpenPerms());
                    } catch (const std::exception& e) {
                        spdlog::warn("unrestrict {} in {} failed: {}", id,
                                     chatId, e.what());
                    }
                }
                if (!failMsg.empty()) {
                    co_await replyAndSweep(
                        bot, msg,
                        std::string{"恢复原权限失败："} + failMsg +
                            "\n请管理员手动在群设置里调整权限。");
                    co_return;
                }
                state.setSilent(chatId, false);
                state.clearSilentSnapshot(chatId);
                co_await replyAndSweep(
                    bot, msg, "已关闭静默，群权限已恢复，所有人可正常发言。");
                co_return;
            }

            // allow / deny：目标优先取"回复某人"，其次数字用户 ID
            const bool isAllow =
                arg == "allow" || arg.rfind("allow ", 0) == 0;
            const bool isDeny = arg == "deny" || arg.rfind("deny ", 0) == 0;
            if (isAllow || isDeny) {
                std::int64_t target = targetFromReply(msg);
                if (target == 0 && !sub.empty()) {
                    try {
                        target = std::stoll(sub);
                    } catch (const std::exception&) {
                        target = 0;
                    }
                }
                if (target == 0) {
                    co_await replyAndSweep(
                        bot, msg,
                        "用法：回复某人的消息发 /silent " +
                            std::string{isAllow ? "allow" : "deny"} +
                            "，或 /silent " +
                            std::string{isAllow ? "allow" : "deny"} +
                            " <用户ID>");
                    co_return;
                }
                std::string failMsg;
                try {
                    if (isAllow) {
                        co_await bot.restrictChatMember(chatId, target,
                                                        allowedPerms());
                    } else if (state.silentEnabled(chatId)) {
                        // 静默中收回：恢复禁言；未静默只需移出名单
                        co_await bot.restrictChatMember(chatId, target,
                                                        mutedPerms());
                    }
                } catch (const std::exception& e) {
                    failMsg = e.what();
                }
                if (!failMsg.empty()) {
                    co_await replyAndSweep(
                        bot, msg,
                        std::string{"操作失败（需要我有“限制成员”权限）："} +
                            failMsg);
                    co_return;
                }
                if (isAllow) {
                    state.addSilentAllow(chatId, target);
                    co_await replyAndSweep(
                        bot, msg, "已放行 " + std::to_string(target) +
                                      "：静默模式下 TA 可以发言。");
                } else {
                    state.removeSilentAllow(chatId, target);
                    co_await replyAndSweep(
                        bot, msg, "已移除 " + std::to_string(target) +
                                      " 的发言放行。");
                }
                co_return;
            }

            co_await replyAndSweep(
                bot, msg,
                "用法：/silent on|off|allow|deny|list，或 /silent 查看状态");
        });

    // ---- Ephemeral 演示（Bot API 10.2+）：公开入口 + 私人结果 ----
    // 群里发一条所有人可见的卡片；点按钮的人各自收到一条"只有他能看到"的
    // ephemeral 消息，可原地替换卡片、可编辑、可删除，群聊记录干干净净。

    // /card —— 发公开福利卡片（任何人的入口；卡片本身保留不被清理）
    dispatcher.onCommand("card",
                         [](BotClient& bot,
                            const json msg) -> net::awaitable<void> {
                             const auto chatId =
                                 msg["chat"].value("id", std::int64_t{0});
                             const auto chatType =
                                 msg["chat"].value("type", std::string{});
                             if (chatType != "group" &&
                                 chatType != "supergroup") {
                                 co_await replyAndSweep(
                                     bot, msg, "这个命令只能在群里用。");
                                 co_return;
                             }
                             co_await bot.sendMessage(
                                 chatId, "🎁 每日福利 · 点击领取",
                                 {{"inline_keyboard",
                                   json::array({{{{"text", "🎟 领取我的专属码"},
                                                  {"callback_data",
                                                   "eph:claim"}}}})}});
                             // 与全群命令一致：命令消息立即清理
                             try {
                                 co_await bot.deleteMessage(
                                     chatId,
                                     msg.value("message_id", std::int64_t{0}));
                             } catch (const std::exception&) {
                                 // 已被删/无权限，忽略
                             }
                         });

    // 点按钮：用 ephemeral 消息原地替换卡片（仅对点击者生效），
    // 15 秒窗口内凭 callback_query_id 发送，只投递到点击的那台设备
    dispatcher.onCallbackQuery(
        "eph:claim",
        [](BotClient& bot, const json cq) -> net::awaitable<void> {
            const auto userId = cq["from"].value("id", std::int64_t{0});
            const auto chatId =
                cq["message"]["chat"].value("id", std::int64_t{0});
            tgbot::EphemeralMessageParameters eph;
            eph.receiverUserId = userId;
            eph.callbackQueryId = cq.value("id", std::string{});
            eph.replaceCallbackQueryMessage = true;
            co_await bot.sendEphemeralMessage(
                chatId, eph,
                "🎉 你的专属码：" + dailyCode(userId) +
                    "\n这条消息只有你能看到 👀",
                ephActionKb());
        });

    // ephemeral 消息上的"刷新"：编辑文本（对 ephemeral 上的按钮回调
    // 不能 replace，必须用 editEphemeralMessage*）
    dispatcher.onCallbackQuery(
        "eph:refresh",
        [](BotClient& bot, const json cq) -> net::awaitable<void> {
            const auto userId = cq["from"].value("id", std::int64_t{0});
            const auto& m = cq["message"];
            const auto chatId = m["chat"].value("id", std::int64_t{0});
            const auto ephId =
                m.value("ephemeral_message_id", std::int64_t{0});
            if (ephId == 0) {
                co_return;  // 不是 ephemeral 消息上的按钮
            }
            co_await bot.editEphemeralMessageText(
                chatId, userId, ephId,
                "🎉 你的专属码：" + dailyCode(userId) + "\n（刚刚刷新过 ✨）",
                ephActionKb());
        });

    // ephemeral 消息上的"销毁"
    dispatcher.onCallbackQuery(
        "eph:destroy",
        [](BotClient& bot, const json cq) -> net::awaitable<void> {
            const auto userId = cq["from"].value("id", std::int64_t{0});
            const auto& m = cq["message"];
            const auto chatId = m["chat"].value("id", std::int64_t{0});
            const auto ephId =
                m.value("ephemeral_message_id", std::int64_t{0});
            if (ephId == 0) {
                co_return;
            }
            co_await bot.deleteEphemeralMessage(chatId, userId, ephId);
        });

    // /eph <文字> —— 模式 B：bot 是群管理员时可主动推送（无 15 秒限制）
    dispatcher.onCommand(
        "eph", [](BotClient& bot,
                  const json msg) -> net::awaitable<void> {
            const auto chatId = msg["chat"].value("id", std::int64_t{0});
            const auto chatType =
                msg["chat"].value("type", std::string{});
            const auto userId = msg["from"].value("id", std::int64_t{0});
            if (chatType != "group" && chatType != "supergroup") {
                co_await replyAndSweep(bot, msg, "这个命令只能在群里用。");
                co_return;
            }
            const std::string text = msg.value("text", "");
            const auto sp = text.find(' ');
            const std::string arg =
                sp == std::string::npos ? "" : text.substr(sp + 1);
            if (arg.empty()) {
                co_await replyAndSweep(
                    bot, msg, "用法：/eph <文字>，给自己发一条只有你能看到的消息。");
                co_return;
            }
            tgbot::EphemeralMessageParameters eph;
            eph.receiverUserId = userId;
            std::string failMsg;
            try {
                co_await bot.sendEphemeralMessage(chatId, eph,
                                                  "📩 只有你能看到：" + arg);
            } catch (const std::exception& e) {
                failMsg = std::string{"ephemeral 发送失败（需要我是群管理员）："} +
                          e.what();
            }
            if (!failMsg.empty()) {
                co_await replyAndSweep(bot, msg, failMsg);
                co_return;
            }
            try {
                co_await bot.deleteMessage(
                    chatId, msg.value("message_id", std::int64_t{0}));
            } catch (const std::exception&) {
                // 忽略
            }
        });

    // /myid —— ephemeral command：命令本身群里就没人能看到（is_ephemeral），
    // bot 再回一条仅本人可见的 ID
    dispatcher.onCommand(
        "myid", [](BotClient& bot,
                   const json msg) -> net::awaitable<void> {
            const auto chatId = msg["chat"].value("id", std::int64_t{0});
            const auto chatType =
                msg["chat"].value("type", std::string{});
            const auto userId = msg["from"].value("id", std::int64_t{0});
            tgbot::EphemeralMessageParameters eph;
            eph.receiverUserId = userId;
            std::string failMsg;
            try {
                co_await bot.sendEphemeralMessage(
                    chatId, eph, "🆔 你的用户 ID：" + std::to_string(userId));
            } catch (const std::exception& e) {
                failMsg = std::string{"ephemeral 发送失败（需要我是群管理员）："} +
                          e.what();
            }
            if (!failMsg.empty()) {
                co_await replyAndSweep(bot, msg, failMsg);
                co_return;
            }
            if (chatType == "group" || chatType == "supergroup") {
                try {
                    co_await bot.deleteMessage(
                        chatId, msg.value("message_id", std::int64_t{0}));
                } catch (const std::exception&) {
                    // 忽略
                }
            }
        });

    // 成员入群：bot 自己进群时发自我介绍（常驻说明书，不参与清理）；
    // 开启 /welcome 后，给每位新成员单独私密投递（ephemeral）欢迎消息，
    // 群里其他人完全无感，不刷屏。
    dispatcher.onAnyMessage(
        [&state, botId, introText](BotClient& bot,
                                   const json msg) -> net::awaitable<void> {
            const auto it = msg.find("new_chat_members");
            if (it == msg.end() || !it->is_array()) {
                co_return;
            }
            const auto chatId = msg["chat"].value("id", std::int64_t{0});
            const auto groupTitle =
                msg["chat"].value("title", std::string{"本群"});

            bool meJoined = false;
            // (user id, 展示名)：ephemeral 按人投递，需要各自的 id
            std::vector<std::pair<std::int64_t, std::string>> joiners;
            for (const auto& u : *it) {
                if (u.value("id", std::int64_t{0}) == botId) {
                    meJoined = true;
                    continue;
                }
                if (u.value("is_bot", false)) {
                    continue;  // 不欢迎其他 bot
                }
                const auto username = u.value("username", std::string{});
                joiners.emplace_back(
                    u.value("id", std::int64_t{0}),
                    username.empty()
                        ? u.value("first_name", std::string{"新朋友"})
                        : "@" + username);
            }

            if (meJoined) {
                spdlog::info("joined group {}, sending intro", chatId);
                co_await bot.sendMessage(chatId, introText);
            }
            if (joiners.empty() || !state.welcomeEnabled(chatId)) {
                co_return;
            }

            const auto tpl = state.welcomeText(chatId).value_or(kDefaultWelcome);
            for (const auto& [userId, name] : joiners) {
                const auto welcome = renderWelcome(tpl, name, groupTitle);
                tgbot::EphemeralMessageParameters eph;
                eph.receiverUserId = userId;
                std::string failMsg;
                try {
                    co_await bot.sendEphemeralMessage(chatId, eph, welcome);
                    spdlog::info("ephemeral welcome sent: chat={} user={}",
                                 chatId, userId);
                } catch (const std::exception& e) {
                    failMsg = e.what();
                }
                if (!failMsg.empty()) {
                    // bot 非管理员时 mode B 不可用：降级为公开普通消息兜底
                    spdlog::warn(
                        "ephemeral welcome failed (chat={} user={}): {}; "
                        "fallback to public message",
                        chatId, userId, failMsg);
                    co_await bot.sendMessage(chatId, welcome);
                }
            }
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
            scheduleDelete(bot, chatId, messageId, delay, executor, "sticker");
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
