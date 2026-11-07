#pragma once

// Minimal local HTTP client from Warden to warden-platform.
//
// Only used for payment callbacks and takes the internal token out of the
// public bot token. It intentionally speaks plain HTTP because the platform
// control plane is expected to listen on loopback unless a TLS reverse proxy
// is put in front of it.

#include <boost/asio/awaitable.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace warden {

namespace platform_client_detail {

inline std::pair<std::string, std::string> splitBaseUrl(const std::string& url) {
    constexpr std::string_view prefix = "http://";
    if (url.rfind(prefix, 0) != 0) {
        throw std::runtime_error("WARDEN_PLATFORM_URL must start with http://");
    }
    auto rest = url.substr(prefix.size());
    const auto slash = rest.find('/');
    if (slash != std::string::npos) {
        rest.resize(slash);
    }
    const auto colon = rest.rfind(':');
    if (colon == std::string::npos) {
        return {rest, "80"};
    }
    return {rest.substr(0, colon), rest.substr(colon + 1)};
}

} // namespace platform_client_detail

class PlatformClient {
public:
    PlatformClient(std::string baseUrl, std::string internalToken)
        : token_(std::move(internalToken)) {
        if (baseUrl.empty() || token_.empty()) {
            return;
        }
        auto [host, port] = platform_client_detail::splitBaseUrl(baseUrl);
        host_ = std::move(host);
        port_ = std::move(port);
    }

    bool enabled() const { return !host_.empty(); }

    boost::asio::awaitable<nlohmann::json> post(std::string path,
                                                nlohmann::json body) const {
        namespace net = boost::asio;
        namespace beast = boost::beast;
        namespace http = beast::http;
        using tcp = net::ip::tcp;

        if (!enabled()) {
            throw std::runtime_error("platform client is disabled");
        }

        tcp::resolver resolver{co_await net::this_coro::executor};
        const auto endpoints = co_await resolver.async_resolve(host_, port_, net::use_awaitable);
        beast::tcp_stream stream{co_await net::this_coro::executor};
        stream.expires_after(std::chrono::seconds(10));
        co_await stream.async_connect(endpoints, net::use_awaitable);

        http::request<http::string_body> request{http::verb::post, std::move(path), 11};
        request.set(http::field::host, host_ + ":" + port_);
        request.set(http::field::user_agent, "warden/0.2");
        request.set(http::field::content_type, "application/json");
        request.set("X-Internal-Token", token_);
        request.body() = body.dump();
        request.prepare_payload();
        co_await http::async_write(stream, request, net::use_awaitable);

        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        co_await http::async_read(stream, buffer, response, net::use_awaitable);
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(response.body());
        } catch (const std::exception&) {
            throw std::runtime_error("platform returned invalid JSON");
        }
        if (response.result() != http::status::ok &&
            response.result() != http::status::created) {
            throw std::runtime_error(parsed.value("message",
                                                  std::string{"platform request failed"}));
        }
        co_return parsed;
    }

private:
    std::string host_;
    std::string port_;
    std::string token_;
};

} // namespace warden
