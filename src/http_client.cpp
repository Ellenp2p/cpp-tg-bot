#include "http_client.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#ifdef TB_CACERT_PEM
#include <filesystem>
#endif

namespace tb {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = net::ssl;

net::awaitable<HttpResponse> httpsGet(const std::string& host,
                                      const std::string& target) {
    auto executor = co_await net::this_coro::executor;

    // TLS 上下文：校验证书链 + 校验主机名，不关闭任何校验
    ssl::context ctx(ssl::context::tls_client);
    ctx.set_options(ssl::context::default_workarounds |
                    ssl::context::no_sslv2 |
                    ssl::context::no_sslv3);
    ctx.set_verify_mode(ssl::verify_peer);
    {
        boost::system::error_code ec;
        ctx.set_default_verify_paths(ec);  // Unix 系统 CA 库
#ifdef TB_CACERT_PEM
        std::error_code fs_ec;
        if (std::filesystem::exists(TB_CACERT_PEM, fs_ec)) {
            ctx.load_verify_file(TB_CACERT_PEM);  // Windows 等平台的兜底
        }
#endif
    }

    ssl::stream<net::ip::tcp::socket> stream(executor, ctx);
    stream.set_verify_callback(ssl::host_name_verification(host));

    // 解析 DNS -> 连接 -> TLS 握手，全部异步、可 co_await
    net::ip::tcp::resolver resolver(executor);
    auto const endpoints = co_await resolver.async_resolve(
        host, "https", net::use_awaitable);
    co_await net::async_connect(stream.next_layer(), endpoints,
                                net::use_awaitable);
    co_await stream.async_handshake(ssl::stream_base::client,
                                    net::use_awaitable);

    // 发送 HTTP/1.1 请求（Connection: close，读到完整响应为止）
    http::request<http::string_body> req{http::verb::get, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "cpp-tg-bot/0.1");
    req.set(http::field::connection, "close");
    co_await http::async_write(stream, req, net::use_awaitable);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    co_await http::async_read(stream, buffer, res, net::use_awaitable);

    // 主动关闭 TLS；对端先断时这里会报 stream_truncated，属正常，忽略
    beast::error_code shutdown_ec;
    stream.shutdown(shutdown_ec);

    co_return HttpResponse{static_cast<int>(res.result_int()),
                           std::move(res.body())};
}

}  // namespace tb
