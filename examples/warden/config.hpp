#pragma once

// Warden 运行配置：全部来自环境变量 / 命令行，不引入配置文件格式。
// 部署侧（systemd / docker compose）本来就擅长注入环境变量。

#include <tgbot/webhook.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace warden {

#ifndef WARDEN_VERSION
#define WARDEN_VERSION "0.2.2"
#endif
inline constexpr char kWardenVersion[] = WARDEN_VERSION;

inline std::string envOr(const char* name, std::string fallback = {}) {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::string{v} : std::move(fallback);
}

inline int envInt(const char* name, int fallback) {
    const std::string raw = envOr(name);
    if (raw.empty()) {
        return fallback;
    }
    try {
        return std::stoi(raw);
    } catch (const std::exception&) {
        return fallback;
    }
}

// 布尔环境变量：true/false 是推荐写法；兼容 1/0、yes/no、on/off，
// 且大小写不敏感（Windows 用户很容易写成 True/False）。
inline bool envFlag(const char* name, bool fallback) {
    std::string raw = envOr(name);
    if (raw.empty()) {
        return fallback;
    }
    std::transform(raw.begin(), raw.end(), raw.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (raw == "1" || raw == "true" || raw == "yes" || raw == "on") {
        return true;
    }
    if (raw == "0" || raw == "false" || raw == "no" || raw == "off") {
        return false;
    }
    return fallback;
}

struct Config {
    std::string token;
    // 机器人拥有者（私聊后台的唯一身份来源）。空 = 只能改单群设置，
    // 没有全局后台。不要做成"第一个 /start 的人就是 owner"——那等于把
    // 机器人送人。
    std::vector<std::int64_t> owners;

    std::string dbPath = "warden.db";
    std::string offsetFile = "warden.offset";
    std::string logDir = "logs";
    std::string logLevel = "info";
    std::string instanceId;

    // 健康检查/控制端口（默认只监听回环）
    std::string healthHost = "127.0.0.1";
    int healthPort = 9090;
    std::string controlToken;    // POST /drain 需要；空 = 禁用控制
    int drainSeconds = 10;       // 优雅停机最多等多久
    int pollTimeoutSeconds = 10; // 长轮询单次阻塞时长，停机响应更快

    bool useWebhook = false;
    tgbot::WebhookConfig webhook;

    // false（默认）：从持久化 offset 续传，停机期间的消息重启后仍会被补上，
    // 从而补排删除任务。Telegram 只保留 24h，最多补这么久。
    bool dropPendingUpdates = false;

    int deleteWorkers = 4;     // 并发删除协程数
    int globalDeleteMs = 34;   // 全局删除节流（~30 次/秒）
    int perChatDeleteMs = 250; // 单群删除节流（~4 次/秒）
    bool pinBackfill = true;   // 启动时对有任务未完成的群做 getChat 置顶补扫
    int pinBackfillMs = 50;    // 补扫间隔，避免启动瞬间打爆 getChat
    int maxDelaySec = 86400;   // 单条消息最长保留 24h（Telegram 48h 硬上限）
    int minDelaySec = 5;

    // 有界 update 队列 + 固定 worker 协程；满了长轮询暂停 / webhook 回 503
    int maxPendingUpdates = 2000;
    int maxPendingMB = 32; // update 队列的字节上限，防止大 JSON 撑爆内存
    int updateWorkers = 8;

    // 机器人回复的自动清理时间（秒）；0 = 不自动删
    int replyTtlSec = 20; // ephemeral / 群内普通兜底 / 私聊兜底

    // 是否给每个群单独发 chat_administrators 命令菜单。
    // 关闭后群管理员菜单为空，但命令仍然可以手动输入使用。
    bool groupMenus = true;

    // ---- 告警出口 ----
    // 三者可以同时配置：同一条告警分别投递，互不影响。
    bool alertOwnerDm = true;      // Telegram 私聊 owner
    std::string alertFeishuUrl;    // 飞书自定义机器人 webhook
    std::string alertFeishuSecret; // 可选；开了签名校验才需要
    std::string alertDiscordUrl;   // Discord webhook
};

inline void parseOwners(const std::string& raw, std::vector<std::int64_t>& out) {
    std::size_t start = 0;
    while (start <= raw.size()) {
        const auto comma = raw.find(',', start);
        const auto piece =
            raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!piece.empty()) {
            try {
                out.push_back(std::stoll(piece));
            } catch (const std::exception&) {
                // 忽略非法项，不让一个手滑的逗号害得整个进程起不来
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
}

inline Config loadConfig(int argc, char** argv) {
    Config cfg;

    if (argc > 1) {
        cfg.token = argv[1];
    } else {
        cfg.token = envOr("TG_BOT_TOKEN");
    }
    if (cfg.token.empty()) {
        throw std::runtime_error(
            "usage: warden <bot-token>\n"
            "   or: set TG_BOT_TOKEN environment variable");
    }

    parseOwners(envOr("WARDEN_OWNERS"), cfg.owners);

    cfg.dbPath = envOr("WARDEN_DB", cfg.dbPath);
    cfg.offsetFile = envOr("WARDEN_OFFSET_FILE", cfg.offsetFile);
    cfg.logDir = envOr("WARDEN_LOG_DIR", cfg.logDir);
    cfg.logLevel = envOr("WARDEN_LOG_LEVEL", cfg.logLevel);
    cfg.dropPendingUpdates = envFlag("WARDEN_DROP_PENDING", false);

    cfg.instanceId = envOr("WARDEN_INSTANCE");
    if (cfg.instanceId.empty()) {
        cfg.instanceId =
            "warden-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    }
    cfg.healthHost = envOr("WARDEN_HEALTH_HOST", cfg.healthHost);
    cfg.healthPort = std::clamp(envInt("WARDEN_HEALTH_PORT", cfg.healthPort), 0, 65535);
    cfg.controlToken = envOr("WARDEN_CONTROL_TOKEN");
    cfg.drainSeconds = std::clamp(envInt("WARDEN_DRAIN_SECONDS", cfg.drainSeconds), 1, 120);
    cfg.pollTimeoutSeconds =
        std::clamp(envInt("WARDEN_POLL_TIMEOUT", cfg.pollTimeoutSeconds), 5, 30);

    cfg.alertOwnerDm = envFlag("WARDEN_ALERT_OWNER_DM", true);
    cfg.alertFeishuUrl = envOr("WARDEN_ALERT_FEISHU_URL");
    cfg.alertFeishuSecret = envOr("WARDEN_ALERT_FEISHU_SECRET");
    cfg.alertDiscordUrl = envOr("WARDEN_ALERT_DISCORD_URL");

    cfg.deleteWorkers = std::clamp(envInt("WARDEN_DELETE_WORKERS", cfg.deleteWorkers), 1, 32);
    cfg.globalDeleteMs =
        std::clamp(envInt("WARDEN_DELETE_GLOBAL_MS", cfg.globalDeleteMs), 10, 1000);
    cfg.perChatDeleteMs =
        std::clamp(envInt("WARDEN_DELETE_CHAT_MS", cfg.perChatDeleteMs), 10, 5000);
    cfg.pinBackfill = envFlag("WARDEN_PIN_BACKFILL", true);
    cfg.pinBackfillMs = std::clamp(envInt("WARDEN_PIN_BACKFILL_MS", cfg.pinBackfillMs), 0, 5000);
    cfg.replyTtlSec = std::clamp(envInt("WARDEN_REPLY_TTL", cfg.replyTtlSec), 0, 3600);
    cfg.maxPendingUpdates =
        std::clamp(envInt("WARDEN_MAX_PENDING", cfg.maxPendingUpdates), 100, 100000);
    cfg.maxPendingMB = std::clamp(envInt("WARDEN_MAX_PENDING_MB", cfg.maxPendingMB), 1, 512);
    cfg.updateWorkers = std::clamp(envInt("WARDEN_UPDATE_WORKERS", cfg.updateWorkers), 1, 64);
    cfg.groupMenus = envFlag("WARDEN_GROUP_MENUS", true);

    const std::string mode = envOr("WARDEN_MODE", "auto");
    const std::string url = envOr("TG_WEBHOOK_URL");

    if (mode == "webhook" && url.empty()) {
        throw std::runtime_error("WARDEN_MODE=webhook 但 TG_WEBHOOK_URL 为空");
    }
    cfg.useWebhook = (mode == "webhook") || (mode == "auto" && !url.empty());

    if (cfg.useWebhook) {
        cfg.webhook.publicUrl = url;
        cfg.webhook.secretToken = envOr("TG_WEBHOOK_SECRET");
        cfg.webhook.host = envOr("WEBHOOK_HOST", "127.0.0.1");
        const int port = envInt("WEBHOOK_PORT", 8080);
        if (port <= 0 || port > 65535) {
            throw std::runtime_error("WEBHOOK_PORT 非法");
        }
        cfg.webhook.port = static_cast<unsigned short>(port);
        cfg.webhook.tlsCert = envOr("WEBHOOK_TLS_CERT");
        cfg.webhook.tlsKey = envOr("WEBHOOK_TLS_KEY");
        cfg.webhook.maxConnections = std::clamp(envInt("WEBHOOK_MAX_CONNECTIONS", 40), 1, 100);
        cfg.webhook.maxPendingUpdates = static_cast<std::size_t>(cfg.maxPendingUpdates);
        cfg.webhook.maxPendingBytes = static_cast<std::size_t>(cfg.maxPendingMB) * 1024 * 1024;
        cfg.webhook.updateWorkers = cfg.updateWorkers;
        cfg.webhook.dropPendingUpdates = cfg.dropPendingUpdates;
        cfg.webhook.registerOnStart = !envFlag("TG_WEBHOOK_SKIP_REGISTER", false);
        // 显式订阅 my_chat_member：后台的"机器人在哪些群"完全依赖它。
        // 不传的话可能沿用服务端旧设置，把成员变化静默过滤掉。
        cfg.webhook.allowedUpdates = R"(["message","callback_query","my_chat_member"])";
    }

    return cfg;
}

} // namespace warden
