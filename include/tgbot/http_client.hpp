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
// 前提：全部调用跑在同一个 io_context 单线程上（池操作无锁）。
boost::asio::awaitable<HttpResponse> httpsGet(const std::string& host,
                                              const std::string& target);

}  // namespace tgbot
