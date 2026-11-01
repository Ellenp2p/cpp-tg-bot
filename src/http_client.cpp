#include <tgbot/http_client.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/system/system_error.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
// 只为了 GetModuleFileNameW；顺手压掉 windows.h 的 min/max 宏污染
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace tgbot {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = net::ssl;
namespace fs = std::filesystem;

namespace {

// 各阶段超时。读超时是按请求给的（长轮询需要 40s+），见 HttpRequest。
constexpr auto kConnectTimeout = std::chrono::seconds{10};
constexpr auto kHandshakeTimeout = std::chrono::seconds{15};
constexpr auto kWriteTimeout = std::chrono::seconds{15};

// ---- CA 证书定位 ----
// 发布物是"exe + cacert.pem 放一起"，所以不能只认构建期写死的绝对路径：
// 换台机器那个路径就不存在，Windows 上会静默退化成空信任库 → 所有握手失败。

fs::path executableDirectory() {
#if defined(_WIN32)
    wchar_t buf[32768];
    const DWORD n =
        ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        return fs::path{buf}.parent_path();
    }
#elif defined(__linux__)
    std::error_code ec;
    const auto self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !self.empty()) {
        return self.parent_path();
    }
#elif defined(__APPLE__)
    char buf[4096];
    std::uint32_t size = static_cast<std::uint32_t>(sizeof(buf));
    if (::_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        const auto exe = fs::weakly_canonical(fs::path{buf}, ec);
        if (!ec) {
            return exe.parent_path();
        }
    }
#endif
    return {};
}

// 依次尝试：SSL_CERT_FILE → exe 同目录 → 工作目录 → 构建期路径
std::vector<fs::path> caBundleCandidates() {
    std::vector<fs::path> candidates;
    if (const char* env = std::getenv("SSL_CERT_FILE");
        env != nullptr && *env != '\0') {
        candidates.emplace_back(env);
    }
    if (const auto dir = executableDirectory(); !dir.empty()) {
        candidates.push_back(dir / "cacert.pem");
    }
    std::error_code ec;
    if (const auto cwd = fs::current_path(ec); !ec && !cwd.empty()) {
        candidates.push_back(cwd / "cacert.pem");
    }
#ifdef TB_CACERT_PEM
    candidates.emplace_back(TB_CACERT_PEM);  // 构建目录（本地开发时直接用）
#endif
    return candidates;
}

// TLS 上下文整个进程共享一个（校验证书链 + 主机名，全部开启）。
// 前提：只在单 io_context 单线程内使用。
ssl::context& tlsContext() {
    static ssl::context ctx = [] {
        ssl::context c(ssl::context::tls_client);
        c.set_options(ssl::context::default_workarounds |
                      ssl::context::no_sslv2 | ssl::context::no_sslv3);
        c.set_verify_mode(ssl::verify_peer);

        // 系统 CA 库（Linux/macOS 通常可用；Windows 上一般拿不到东西）
        boost::system::error_code ec;
        c.set_default_verify_paths(ec);
        bool loaded = !ec;

        for (const auto& path : caBundleCandidates()) {
            std::error_code fsEc;
            if (!fs::is_regular_file(path, fsEc)) {
                continue;
            }
            try {
                c.load_verify_file(path.string());
                spdlog::info("TLS CA bundle: {} (system CA: {})", path.string(),
                             loaded ? "yes" : "no");
                loaded = true;
                break;
            } catch (const std::exception& e) {
                spdlog::warn("load CA bundle '{}' failed: {}", path.string(),
                             e.what());
            }
        }
        if (!loaded) {
            spdlog::error(
                "找不到任何 CA 证书，TLS 校验必然失败：请把 cacert.pem 放到"
                "可执行文件同目录，或设置 SSL_CERT_FILE");
        }
        return c;
    }();
    return ctx;
}

// 用 beast::tcp_stream 而不是裸 socket：它自带"下一次读写必须在这个时间内
// 完成"的截止时间，黑洞连接不会再让请求无限挂起。
struct Connection {
    ssl::stream<beast::tcp_stream> stream;
    beast::flat_buffer buffer;  // 读缓冲必须随连接复用，不能丢

    explicit Connection(const net::any_io_executor& ex)
        : stream(ex, tlsContext()) {}
    ~Connection() {
        boost::system::error_code ec;
        stream.next_layer().socket().close(ec);
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
};

constexpr std::size_t kMaxIdlePerHost = 8;  // 每个 host 的空闲连接上限

// 按 host 分组的空闲连接池：不同 host 之间绝不共用连接——连接上绑定了
// 建连时的 host_name_verification 回调，混用等于给别的 host 开了后门。
std::map<std::string, std::vector<std::unique_ptr<Connection>>>& idlePool() {
    static std::map<std::string, std::vector<std::unique_ptr<Connection>>> pool;
    return pool;
}

struct AcquiredConnection {
    std::unique_ptr<Connection> conn;
    bool fromPool = false;
};

net::awaitable<AcquiredConnection> acquire(const std::string& host) {
    auto executor = co_await net::this_coro::executor;
    // 检查到取出之间没有 co_await，单线程下原子
    if (auto& bucket = idlePool()[host]; !bucket.empty()) {
        auto pooled = std::move(bucket.back());
        bucket.pop_back();
        co_return AcquiredConnection{std::move(pooled), true};
    }

    auto conn = std::make_unique<Connection>(executor);
    conn->stream.set_verify_callback(ssl::host_name_verification(host));

    net::ip::tcp::resolver resolver(executor);
    const auto endpoints =
        co_await resolver.async_resolve(host, "https", net::use_awaitable);

    // 连接与握手都设截止时间：DNS 之后的 TCP 黑洞是最常见的"假死"来源
    conn->stream.next_layer().expires_after(kConnectTimeout);
    co_await conn->stream.next_layer().async_connect(endpoints,
                                                     net::use_awaitable);

    conn->stream.next_layer().expires_after(kHandshakeTimeout);
    co_await conn->stream.async_handshake(ssl::stream_base::client,
                                          net::use_awaitable);
    co_return AcquiredConnection{std::move(conn), false};
}

void release(std::unique_ptr<Connection> conn, const std::string& host,
             bool reusable) {
    if (!conn || !reusable) {
        return;  // unique_ptr 析构自动 close
    }
    auto& bucket = idlePool()[host];
    if (bucket.size() < kMaxIdlePerHost) {
        bucket.push_back(std::move(conn));
    }
}

// 单发请求（不做重试判定）：写请求 -> 读响应，异常原样上抛。
// 独立成协程是为了让 httpsRequest 里能用 try/catch 分类处理错误
// （MSVC 不允许在 catch 块内 co_await，但 try 块内可以）。
net::awaitable<HttpResponse> doRequest(Connection& conn,
                                       const std::string& host,
                                       const HttpRequest& r) {
    const auto verb =
        r.method == HttpMethod::kPost ? http::verb::post : http::verb::get;
    http::request<http::string_body> req{verb, r.target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "cpp-tg-bot/0.3");
    if (r.method == HttpMethod::kPost) {
        // 表单放在 body 里：长文本/大键盘不再受 URL 长度限制
        req.set(http::field::content_type, r.contentType);
        req.body() = r.body;
        req.prepare_payload();
    }

    conn.stream.next_layer().expires_after(kWriteTimeout);
    co_await http::async_write(conn.stream, req, net::use_awaitable);

    http::response<http::string_body> res;
    conn.stream.next_layer().expires_after(r.readTimeout);
    co_await http::async_read(conn.stream, conn.buffer, res,
                              net::use_awaitable);

    co_return HttpResponse{static_cast<int>(res.result_int()),
                           std::move(res.body()), res.keep_alive()};
}

bool isStaleConnectionCode(const boost::system::error_code& code) {
    return code == net::error::eof || code == http::error::end_of_stream ||
           code == http::error::partial_message ||
           code == net::error::connection_reset ||
           code == net::error::broken_pipe;
}

}  // namespace

void clearIdlePool() { idlePool().clear(); }

net::awaitable<HttpResponse> httpsRequest(const std::string& host,
                                          const HttpRequest& request) {
    // 最多两次尝试：池化连接可能被服务端在空闲期静默断开（重用前无法感知），
    // 遇到这类错误就丢弃连接、换新重试一次。
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto acquired = co_await acquire(host);
        auto& conn = acquired.conn;
        try {
            auto resp = co_await doRequest(*conn, host, request);
            release(std::move(conn), host, resp.reusable);
            co_return resp;
        } catch (const boost::system::system_error& e) {
            // 注意：use_awaitable 抛的是 boost::system::system_error，
            // 不是 beast::system_error（后者只是它的派生类），捕窄了会漏。
            const auto code = e.code();
            if (isStaleConnectionCode(code)) {
                continue;  // conn 析构 → 关闭，下一轮换新连接
            }
            // 读/写超时：只有"复用来的旧连接"值得换新重试；
            // 新建连接就超时说明对端真的不响应，应当上抛
            if (code == beast::error::timeout && acquired.fromPool) {
                continue;
            }
            throw;
        }
    }
    throw std::runtime_error(
        "httpsRequest: 复用连接失效后换新连接仍然失败（服务端不可达？）");
}

net::awaitable<HttpResponse> httpsGet(const std::string& host,
                                      const std::string& target) {
    HttpRequest req;
    req.method = HttpMethod::kGet;
    req.target = target;
    co_return co_await httpsRequest(host, req);
}

}  // namespace tgbot
