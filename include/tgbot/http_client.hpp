#pragma once

#include <boost/asio/awaitable.hpp>

#include <string>

namespace tgbot {

struct HttpResponse {
    int status;
    std::string body;
    // keep-alive 响应的连接可回池复用；Connection: close 的用完即弃
    bool reusable;
};

// 发起 HTTPS GET 请求并 co_await 其结果。
// keep-alive 连接池：空闲连接复用省 TLS 握手，池满/断开的连接自动丢弃。
// 池按 host 分组复用（不同 host 绝不共用连接，否则会复用到别的
// 服务端证书校验通过的连接）。
// 前提：全部调用跑在同一个 io_context 单线程上（池操作无锁）。
boost::asio::awaitable<HttpResponse> httpsGet(const std::string& host,
                                              const std::string& target);

// 清空空闲连接池。连接对象持有 io_context 的 executor，必须在
// io_context 销毁之前清空（BotClient 析构时会调用；自建客户端也要记得调）。
void clearIdlePool();

}  // namespace tgbot
