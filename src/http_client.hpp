#pragma once

#include <boost/asio/awaitable.hpp>

#include <string>

namespace tb {

struct HttpResponse {
    int status;
    std::string body;
};

// 发起一个 HTTPS GET 请求并 co_await 其结果。
// M1 阶段实现：每次请求新建连接，Connection: close（简单可靠）；
// 连接复用/连接池留待 M4 优化。
boost::asio::awaitable<HttpResponse> httpsGet(const std::string& host,
                                              const std::string& target);

}  // namespace tb
