#pragma once

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <string>

namespace tgbot {

struct HttpResponse {
    int status;
    std::string body;
    // keep-alive 响应的连接可回池复用；Connection: close 的用完即弃
    bool reusable;
};

enum class HttpMethod { kGet, kPost };

// 一次 HTTPS 请求的描述。
// readTimeout 是**读响应**的超时：长轮询（getUpdates timeout=30）必须比它大，
// 否则正常的等待会被当成故障。
struct HttpRequest {
    HttpMethod method = HttpMethod::kGet;
    std::string target; // 路径 + 可选 query，例如 /bot<token>/getMe
    std::string body;   // 仅 POST 使用（表单编码）
    std::string contentType = "application/x-www-form-urlencoded";
    std::chrono::seconds readTimeout{30};
};

// 发起 HTTPS 请求并 co_await 其结果。
// 连接池：空闲连接复用省 TLS 握手，池满/断开的连接自动丢弃；按 host 分组，
// 不同 host 绝不共用连接（否则会复用到别的服务端证书校验通过的连接）。
// 全链路都有超时（连接 10s / 握手 15s / 写 15s / 读由 readTimeout 指定），
// 避免对端黑洞时请求永久挂起。
// 前提：全部调用跑在同一个 io_context 单线程上（池操作无锁）。
boost::asio::awaitable<HttpResponse> httpsRequest(const std::string& host,
                                                  const HttpRequest& request);

// 便捷版 GET（保留给只想发 GET 的调用方）
boost::asio::awaitable<HttpResponse> httpsGet(const std::string& host, const std::string& target);

// 清空空闲连接池。连接对象持有 io_context 的 executor，必须在
// io_context 销毁之前清空（BotClient 析构时会调用；自建客户端也要记得调）。
void clearIdlePool();

} // namespace tgbot
