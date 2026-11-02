#include "health.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/system_error.hpp>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace warden {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using nlohmann::json;

HealthServer::HealthServer(Store& store, std::atomic<bool>& draining,
                           std::string instanceId, std::string version,
                           std::string controlToken)
    : store_(store),
      draining_(draining),
      instanceId_(std::move(instanceId)),
      version_(std::move(version)),
      controlToken_(std::move(controlToken)),
      startedAt_(std::chrono::steady_clock::now()) {}

void HealthServer::start(const net::any_io_executor& executor,
                         const std::string& host, unsigned short port) {
    const net::ip::tcp::endpoint endpoint{net::ip::make_address(host), port};
    auto acceptor = std::make_shared<net::ip::tcp::acceptor>(executor);
    acceptor->open(endpoint.protocol());
    acceptor->set_option(net::socket_base::reuse_address(true));
    acceptor->bind(endpoint);
    acceptor->listen(net::socket_base::max_listen_connections);
    spdlog::info("health server listening on http://{}:{}/healthz", host, port);

    auto self = shared_from_this();
    net::co_spawn(
        executor,
        [self, acceptor]() -> net::awaitable<void> {
            co_await self->acceptLoop(acceptor);
        },
        net::detached);
}

net::awaitable<void> HealthServer::acceptLoop(
    std::shared_ptr<net::ip::tcp::acceptor> acceptor) {
    const auto executor = co_await net::this_coro::executor;
    try {
        while (true) {
            auto socket = co_await acceptor->async_accept(net::use_awaitable);
            auto self = shared_from_this();
            net::co_spawn(
                executor,
                [self, sock = std::move(socket)]() mutable
                    -> net::awaitable<void> {
                    co_await self->session(std::move(sock));
                },
                net::detached);
        }
    } catch (const boost::system::system_error& e) {
        if (e.code() != net::error::operation_aborted) {
            spdlog::warn("health accept loop terminated: {}", e.what());
        }
    } catch (const std::exception& e) {
        spdlog::warn("health accept loop terminated: {}", e.what());
    }
}

net::awaitable<void> HealthServer::session(net::ip::tcp::socket socket) {
    beast::tcp_stream stream(std::move(socket));
    try {
        stream.expires_after(std::chrono::seconds{5});
        beast::flat_buffer buffer;
        http::request_parser<http::string_body> parser;
        parser.body_limit(4096);
        co_await http::async_read(stream, buffer, parser,
                                  net::use_awaitable);
        auto req = parser.release();

        http::status status = http::status::ok;
        std::string body;

        if (req.method() == http::verb::get && req.target() == "/healthz") {
            body = json{{"ok", true},
                        {"instance", instanceId_},
                        {"version", version_},
                        {"draining", draining_.load()},
                        {"pending_deletions", store_.pendingDeletions()}}
                       .dump();
        } else if (req.method() == http::verb::get &&
                   req.target() == "/readyz") {
            const bool ready = !draining_.load();
            status = ready ? http::status::ok
                           : http::status::service_unavailable;
            body = json{{"ok", ready},
                        {"instance", instanceId_},
                        {"version", version_},
                        {"draining", draining_.load()},
                        {"pending_deletions", store_.pendingDeletions()}}
                       .dump();
        } else if (req.method() == http::verb::post &&
                   req.target() == "/drain") {
            const auto tokenIt = req.find("X-Warden-Token");
            if (controlToken_.empty()) {
                status = http::status::forbidden;
                body = R"({"ok":false,"error":"control token not configured"})";
            } else if (tokenIt == req.end() ||
                       tokenIt->value() != controlToken_) {
                status = http::status::forbidden;
                body = R"({"ok":false,"error":"bad token"})";
            } else {
                draining_.store(true);
                spdlog::info("drain requested via /drain");
                status = http::status::accepted;
                body = R"({"ok":true,"draining":true})";
            }
        } else {
            status = http::status::not_found;
            body = R"({"ok":false,"error":"not found"})";
        }

        http::response<http::string_body> res{status, req.version()};
        res.set(http::field::server, "warden-health");
        res.set(http::field::content_type, "application/json");
        res.set(http::field::cache_control, "no-store");
        res.keep_alive(false);
        res.body() = std::move(body);
        res.prepare_payload();
        stream.expires_after(std::chrono::seconds{5});
        co_await http::async_write(stream, res, net::use_awaitable);
        boost::system::error_code ignored;
        stream.socket().shutdown(net::ip::tcp::socket::shutdown_both, ignored);
    } catch (const boost::system::system_error& e) {
        if (e.code() != net::error::operation_aborted &&
            e.code() != http::error::end_of_stream) {
            spdlog::debug("health session closed: {}", e.what());
        }
    } catch (const std::exception& e) {
        spdlog::debug("health session error: {}", e.what());
    }
}

}  // namespace warden
