#include "alerts.hpp"

#include <tgbot/http_client.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <stdexcept>
#include <string>
#include <utility>

namespace warden {

namespace net = boost::asio;
using nlohmann::json;

namespace {

// Discord 单条 content 上限 2000；留点余量，且不能切断 UTF-8
std::string truncateUtf8(std::string_view s, std::size_t maxChars) {
    std::string out;
    std::size_t chars = 0;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        if (i + len > s.size()) {
            break;
        }
        if (chars == maxChars) {
            out += "…";
            break;
        }
        out.append(s.substr(i, len));
        i += len;
        ++chars;
    }
    return out;
}

// 目前只支持标准 https://host/path（Feishu/Discord 的 webhook 都是 443）
bool splitHttpsUrl(const std::string& url, std::string& host, std::string& target) {
    static constexpr std::string_view kPrefix = "https://";
    if (url.rfind(kPrefix, 0) != 0) {
        return false;
    }
    const std::string_view rest{url.data() + kPrefix.size(), url.size() - kPrefix.size()};
    const auto slash = rest.find('/');
    host = std::string{rest.substr(0, slash)};
    target = slash == std::string_view::npos ? std::string{"/"} : std::string{rest.substr(slash)};
    if (host.empty() || host.find(':') != std::string::npos ||
        host.find('@') != std::string::npos) {
        return false;
    }
    return true;
}

net::awaitable<tgbot::HttpResponse> postJson(const std::string& url, const json& body) {
    std::string host;
    std::string target;
    if (!splitHttpsUrl(url, host, target)) {
        throw std::runtime_error(
            "alert webhook 必须是 https://host/path（不支持端口/IP 字面量）: " + url);
    }
    tgbot::HttpRequest request;
    request.method = tgbot::HttpMethod::kPost;
    request.target = std::move(target);
    request.body = body.dump();
    request.contentType = "application/json";
    request.readTimeout = std::chrono::seconds{10};
    co_return co_await tgbot::httpsRequest(host, request);
}

std::string feishuSign(const std::string& timestamp, const std::string& secret) {
    // 飞书签名：key = timestamp + "\n" + secret，data 为空，HMAC-SHA256 后 Base64
    const std::string key = timestamp + "\n" + secret;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    HMAC(EVP_sha256(),
         key.data(),
         static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(""),
         0,
         digest,
         &digestLen);

    const int encodedLen = 4 * ((static_cast<int>(digestLen) + 2) / 3);
    std::string out(static_cast<std::size_t>(encodedLen) + 1, '\0');
    const int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(out.data()), digest, static_cast<int>(digestLen));
    out.resize(static_cast<std::size_t>(std::max(written, 0)));
    return out;
}

void checkHttp(const tgbot::HttpResponse& resp, std::string_view name) {
    if (resp.status >= 200 && resp.status < 300) {
        return;
    }
    throw std::runtime_error(std::string{name} + " HTTP " + std::to_string(resp.status) + ": " +
                             resp.body.substr(0, 300));
}

} // namespace

AlertDispatcher::AlertDispatcher(net::any_io_executor executor) : executor_(std::move(executor)) {}

void AlertDispatcher::add(AlertSink sink) {
    if (!sink.send) {
        return;
    }
    sinks_.push_back(std::move(sink));
}

void AlertDispatcher::publish(std::string text) {
    if (sinks_.empty()) {
        spdlog::debug("alert dropped (no sink configured): {}", text);
        return;
    }
    for (auto& sink : sinks_) {
        auto copy = sink; // 每个渠道一份，detached 协程自己持有
        net::co_spawn(
            executor_,
            [sink = std::move(copy), text]() -> net::awaitable<void> {
                try {
                    co_await sink.send(text);
                } catch (const std::exception& e) {
                    spdlog::warn("alert sink '{}' failed: {}", sink.name, e.what());
                }
            },
            net::detached);
    }
    spdlog::info("alert dispatched to {} sink(s)", sinks_.size());
}

AlertSink makeOwnerDmSink(tgbot::BotClient& bot, std::vector<std::int64_t> owners) {
    return AlertSink{
        "telegram-owner",
        [&bot, owners = std::move(owners)](const std::string& text) -> net::awaitable<void> {
            for (const auto owner : owners) {
                try {
                    co_await bot.sendMessage(owner, "🛡 " + text);
                } catch (const std::exception& e) {
                    spdlog::warn("alert dm to owner {} failed: {}", owner, e.what());
                }
            }
        }};
}

AlertSink makeFeishuSink(std::string webhookUrl, std::string secret) {
    return AlertSink{
        "feishu",
        [url = std::move(webhookUrl),
         secret = std::move(secret)](const std::string& text) -> net::awaitable<void> {
            json body = {{"msg_type", "text"}, {"content", {{"text", text}}}};
            if (!secret.empty()) {
                const auto timestamp = std::to_string(static_cast<long long>(std::time(nullptr)));
                body["timestamp"] = timestamp;
                body["sign"] = feishuSign(timestamp, secret);
            }
            const auto resp = co_await postJson(url, body);
            checkHttp(resp, "feishu");
            // 飞书 HTTP 200 也可能是业务错误，尽量把 code 打出来
            try {
                const auto j = json::parse(resp.body);
                if (j.contains("code") && j["code"].is_number() &&
                    j["code"].get<long long>() != 0) {
                    throw std::runtime_error("feishu code=" + j["code"].dump() +
                                             " msg=" + j.value("msg", std::string{}));
                }
                if (j.contains("StatusCode") && j["StatusCode"].is_number() &&
                    j["StatusCode"].get<long long>() != 0) {
                    throw std::runtime_error("feishu StatusCode=" + j["StatusCode"].dump());
                }
            } catch (const json::exception&) {
                // 响应不是 JSON：HTTP 2xx 就当成功
            }
        }};
}

AlertSink makeDiscordSink(std::string webhookUrl) {
    return AlertSink{
        "discord", [url = std::move(webhookUrl)](const std::string& text) -> net::awaitable<void> {
            json body = {{"content", truncateUtf8(text, 1800)}};
            const auto resp = co_await postJson(url, body);
            checkHttp(resp, "discord");
        }};
}

} // namespace warden
