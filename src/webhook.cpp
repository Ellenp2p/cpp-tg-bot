#include <tgbot/webhook.hpp>

#include "chat_dispatch_util.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace tgbot {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = net::ssl;
using nlohmann::json;

namespace {

constexpr std::size_t kBodyLimit = 1024 * 1024;  // update 上限 1MB，足够
constexpr auto kIoTimeout = std::chrono::seconds(30);

// 从完整 URL 取路径部分（webhook 只接收对该路径的 POST）
std::string pathOf(const std::string& url) {
    const auto scheme = url.find("://");
    const auto from = scheme == std::string::npos ? 0 : scheme + 3;
    const auto slash = url.find('/', from);
    return slash == std::string::npos ? "/" : url.substr(slash);
}

// ---- I/O 超时：防慢连接占用会话。两种流用不同方式设置 ----

void setExpiry(beast::tcp_stream& stream) {
    stream.expires_after(kIoTimeout);
}

void setExpiry(ssl::stream<beast::tcp_stream>& stream) {
    stream.next_layer().expires_after(kIoTimeout);
}

// ---- 单连接会话：plain 与 TLS 流共用 ----
// Telegram 对非 2xx 会自动重试，所以校验失败也要给出明确状态码而不是断连。

template <typename Stream>
net::awaitable<void> session(std::unique_ptr<Stream> stream,
                             const WebhookConfig& cfg, std::string_view path,
                             detail::ChatStrands& strands, BotClient& bot,
                             Dispatcher& dispatcher,
                             const std::atomic<bool>* shuttingDown) {
    beast::flat_buffer buffer;  // 跨 keep-alive 请求复用读缓冲
    try {
        while (true) {
            setExpiry(*stream);
            // 显式设 body 上限：超限会抛 body_limit（按对端断连处理，
            // Telegram 会自动重试），避免巨量 body 耗尽内存
            http::request_parser<http::string_body> parser;
            parser.body_limit(kBodyLimit);
            co_await http::async_read(*stream, buffer, parser,
                                      net::use_awaitable);
            http::request<http::string_body> req = parser.release();

            http::status status = http::status::ok;
            json update;
            bool doDispatch = false;

            if (req.method() != http::verb::post) {
                status = http::status::method_not_allowed;
            } else if (req.target() != path) {
                status = http::status::not_found;
            } else if (!cfg.secretToken.empty()) {
                const auto it =
                    req.find("X-Telegram-Bot-Api-Secret-Token");
                if (it == req.end() || it->value() != cfg.secretToken) {
                    status = http::status::forbidden;
                }
            }

            if (status == http::status::ok) {
                bool parsed = true;
                try {
                    update = json::parse(req.body());
                    if (!update.is_object()) {
                        parsed = false;
                    }
                } catch (const std::exception&) {
                    parsed = false;  // co_await 不允许在 catch 内，先记标志
                }
                if (!parsed) {
                    status = http::status::bad_request;
                } else if (shuttingDown != nullptr &&
                           shuttingDown->load()) {
                    // 停机中：不派发，回 503 让 Telegram 稍后重试
                    status = http::status::service_unavailable;
                } else {
                    doDispatch = true;
                }
            }

            http::response<http::string_body> res{status, req.version()};
            res.set(http::field::server, "cpp-tg-bot-webhook");
            res.set(http::field::content_type, "application/json");
            // 503 时主动关连接，让 Telegram 换新连接重试
            res.keep_alive(req.keep_alive() &&
                           status != http::status::service_unavailable);
            res.body() = status == http::status::ok ? R"({"ok":true})"
                                                    : R"({"ok":false})";
            res.prepare_payload();
            setExpiry(*stream);
            co_await http::async_write(*stream, res, net::use_awaitable);

            if (!res.keep_alive()) {
                co_return;
            }
            if (doDispatch) {
                // 响应已确认（Telegram 视为投递成功），再异步派发业务
                strands.dispatch(update, bot, dispatcher);
            }
        }
    } catch (const beast::system_error&) {
        // 对端断连/超时：会话自然结束
    } catch (const std::exception& e) {
        spdlog::warn("webhook session error: {}", e.what());
    }
}

// ---- 监听循环：接受连接并 spawn 会话协程 ----
// 注意：acceptor 在 webhookLoop 里建好再传进来——bind/listen 失败必须
// 让启动直接失败（fatal）；若在这里面做，异常会被 detached 吞掉，
// 表现为"webhook registered 但根本没有监听"的静默故障。

net::awaitable<void> acceptLoop(net::ip::tcp::acceptor acceptor,
                                const WebhookConfig& cfg, std::string_view path,
                                BotClient& bot, Dispatcher& dispatcher,
                                const std::atomic<bool>* shuttingDown,
                                ssl::context* tlsCtx) {
    const auto executor = co_await net::this_coro::executor;
    spdlog::info("webhook listening on {}://{}:{}", tlsCtx ? "https" : "http",
                 cfg.host, cfg.port);

    detail::ChatStrands strands(executor);
    try {
        while (true) {
            auto socket = co_await acceptor.async_accept(net::use_awaitable);
            net::co_spawn(
                executor,
                [sock = std::move(socket), &cfg, path, &strands, &bot,
                 &dispatcher, shuttingDown,
                 tlsCtx]() mutable -> net::awaitable<void> {
                    auto tcp =
                        std::make_unique<beast::tcp_stream>(std::move(sock));
                    try {
                        if (tlsCtx != nullptr) {
                            auto tls = std::make_unique<
                                ssl::stream<beast::tcp_stream>>(std::move(*tcp),
                                                                *tlsCtx);
                            tls->next_layer().expires_after(kIoTimeout);
                            co_await tls->async_handshake(
                                ssl::stream_base::server, net::use_awaitable);
                            co_await session(std::move(tls), cfg, path, strands,
                                             bot, dispatcher, shuttingDown);
                        } else {
                            co_await session(std::move(tcp), cfg, path, strands,
                                             bot, dispatcher, shuttingDown);
                        }
                    } catch (const std::exception& e) {
                        // TLS 握手失败等：丢弃该连接，不影响服务
                        spdlog::warn("webhook connection closed: {}", e.what());
                    }
                },
                net::detached);
        }
    } catch (const std::exception& e) {
        // accept 本身出错（资源耗尽等）：记下原因，监听中断但进程不崩
        spdlog::error("webhook accept loop terminated: {}", e.what());
    }
}

}  // namespace

net::awaitable<void> webhookLoop(BotClient& bot, Dispatcher& dispatcher,
                                 const WebhookConfig& cfg,
                                 const std::atomic<bool>* shuttingDown) {
    const auto executor = co_await net::this_coro::executor;
    const std::string path = pathOf(cfg.publicUrl);

    // TLS 直连形态：加载证书；否则明文监听（反代终结 TLS）
    std::unique_ptr<ssl::context> tls;
    if (!cfg.tlsCert.empty() && !cfg.tlsKey.empty()) {
        tls = std::make_unique<ssl::context>(ssl::context::tls_server);
        tls->set_options(ssl::context::default_workarounds);
        tls->use_certificate_chain_file(cfg.tlsCert);
        tls->use_private_key_file(cfg.tlsKey, ssl::context::pem);
        spdlog::info("TLS enabled: cert={}", cfg.tlsCert);
    } else if (!cfg.tlsCert.empty() || !cfg.tlsKey.empty()) {
        throw std::runtime_error(
            "webhook: tlsCert and tlsKey must be set together");
    }

    // 先起监听，再注册 webhook；bind 失败在这里抛（fatal），
    // 避免"注册了却没人接"的静默故障。SO_REUSEADDR 方便重启快速复用端口。
    const net::ip::tcp::endpoint endpoint{net::ip::make_address(cfg.host),
                                          cfg.port};
    net::ip::tcp::acceptor acceptor(executor);
    acceptor.open(endpoint.protocol());
    // 端口独占语义按平台区分：
    // - POSIX：SO_REUSEADDR 是正常的服务端语义（快速重启可复用处于
    //   TIME_WAIT 的端口），且仍能拦截"另一进程正在监听"的冲突
    // - Windows：SO_REUSEADDR 会允许第二个进程绑定同一端口（连接被两个
    //   实例随机分发，静默故障），必须改用 SO_EXCLUSIVEADDRUSE 强制独占
    //   （Asio 未封装该选项，直接 setsockopt）
#ifdef _WIN32
    const BOOL exclusive = TRUE;
    if (::setsockopt(acceptor.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive),
                     sizeof(exclusive)) != 0) {
        throw std::runtime_error("webhook: failed to set SO_EXCLUSIVEADDRUSE");
    }
#else
    acceptor.set_option(net::socket_base::reuse_address(true));
#endif
    acceptor.bind(endpoint);
    acceptor.listen(net::socket_base::max_listen_connections);
    net::co_spawn(executor,
                  acceptLoop(std::move(acceptor), cfg, path, bot, dispatcher,
                             shuttingDown, tls.get()),
                  net::detached);

    if (cfg.registerOnStart) {
        co_await bot.setWebhook(cfg.publicUrl, cfg.secretToken,
                                cfg.maxConnections, cfg.dropPendingUpdates);
        spdlog::info(
            "webhook registered: {} (port {}, tls={}, secret={})",
            cfg.publicUrl, cfg.port,
            tls ? "on" : "off (plain, put a reverse proxy with TLS in front)",
            cfg.secretToken.empty() ? "off" : "on");
    } else {
        spdlog::warn(
            "webhook registration skipped (registerOnStart=false), "
            "local self-test mode: {} (port {}, secret={})", cfg.publicUrl,
            cfg.port, cfg.secretToken.empty() ? "off" : "on");
    }

    // 监听循环独立运行，这里永久挂起直到 io_context 停止
    net::steady_timer idle(executor);
    idle.expires_at(net::steady_timer::time_point::max());
    boost::system::error_code ec;
    co_await idle.async_wait(net::redirect_error(net::use_awaitable, ec));
}

}  // namespace tgbot
