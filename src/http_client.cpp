#include <tgbot/http_client.hpp>

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

#include <memory>
#include <stdexcept>
#include <vector>

namespace tgbot {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = net::ssl;

namespace {

// TLS 上下文整个进程共享一个（校验证书链 + 主机名，全部开启）。
// 前提：只在单 io_context 单线程内使用。
ssl::context& tlsContext() {
    static ssl::context ctx = [] {
        ssl::context c(ssl::context::tls_client);
        c.set_options(ssl::context::default_workarounds |
                      ssl::context::no_sslv2 |
                      ssl::context::no_sslv3);
        c.set_verify_mode(ssl::verify_peer);
        boost::system::error_code ec;
        c.set_default_verify_paths(ec);  // Unix 系统 CA 库
#ifdef TB_CACERT_PEM
        std::error_code fs_ec;
        if (std::filesystem::exists(TB_CACERT_PEM, fs_ec)) {
            c.load_verify_file(TB_CACERT_PEM);  // Windows 等平台的兜底
        }
#endif
        return c;
    }();
    return ctx;
}

struct Connection {
    ssl::stream<net::ip::tcp::socket> stream;
    beast::flat_buffer buffer;  // 读缓冲必须随连接复用，不能丢

    explicit Connection(const net::any_io_executor& ex)
        : stream(ex, tlsContext()) {}
    ~Connection() {
        boost::system::error_code ec;
        stream.next_layer().close(ec);
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
};

constexpr std::size_t kMaxIdle = 8;  // 空闲连接池上限，防句柄堆积

// 本项目只连 api.telegram.org 一个 host，池不按 host 分键；
// 若将来多 host，把 vector 换成 map<host, vector<...>> 即可。
std::vector<std::unique_ptr<Connection>>& idlePool() {
    static std::vector<std::unique_ptr<Connection>> pool;
    return pool;
}

net::awaitable<std::unique_ptr<Connection>> acquire(const std::string& host) {
    auto executor = co_await net::this_coro::executor;
    auto& pool = idlePool();
    if (!pool.empty()) {
        // 检查到取出之间没有 co_await，单线程下原子
        auto conn = std::move(pool.back());
        pool.pop_back();
        co_return conn;
    }
    auto conn = std::make_unique<Connection>(executor);
    conn->stream.set_verify_callback(ssl::host_name_verification(host));
    net::ip::tcp::resolver resolver(executor);
    const auto endpoints =
        co_await resolver.async_resolve(host, "https", net::use_awaitable);
    co_await net::async_connect(conn->stream.next_layer(), endpoints,
                                net::use_awaitable);
    co_await conn->stream.async_handshake(ssl::stream_base::client,
                                          net::use_awaitable);
    co_return conn;
}

void release(std::unique_ptr<Connection> conn, bool reusable) {
    auto& pool = idlePool();
    if (reusable && pool.size() < kMaxIdle) {
        pool.push_back(std::move(conn));
    }
    // 不可复用（对端关闭/出错）：unique_ptr 析构自动 close
}

// 单发请求（不做重试判定）：写请求 -> 读响应，异常原样上抛。
// 独立成协程是为了让 httpsGet 里能用 try/catch 分类处理错误
// （MSVC 不允许在 catch 块内 co_await，但 try 块内可以）。
net::awaitable<HttpResponse> doGet(Connection& conn, const std::string& host,
                                   const std::string& target) {
    // HTTP/1.1 默认 keep-alive（不写 Connection: close）
    http::request<http::string_body> req{http::verb::get, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "cpp-tg-bot/0.1");

    co_await http::async_write(conn.stream, req, net::use_awaitable);

    http::response<http::string_body> res;
    co_await http::async_read(conn.stream, conn.buffer, res,
                              net::use_awaitable);

    co_return HttpResponse{static_cast<int>(res.result_int()),
                           std::move(res.body()), res.keep_alive()};
}

}  // namespace

net::awaitable<HttpResponse> httpsGet(const std::string& host,
                                      const std::string& target) {
    // 最多两次尝试：池化连接可能被服务端在空闲期静默断开（重用前无法感知），
    // 遇到这类错误就丢弃连接、换新重试一次。
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto conn = co_await acquire(host);
        try {
            auto resp = co_await doGet(*conn, host, target);
            release(std::move(conn), resp.reusable);
            co_return resp;
        } catch (const beast::system_error& e) {
            // 写失败 / 服务端提前关连接（空闲超时）：连接已废，换新重试
            if (e.code() == http::error::end_of_stream ||
                e.code() == net::error::operation_aborted) {
                continue;
            }
            throw;
        }
    }
    throw std::runtime_error("httpsGet: request failed on fresh connection");
}

}  // namespace tgbot
