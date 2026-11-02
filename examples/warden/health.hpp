#pragma once

// 本地健康/控制 HTTP 服务（默认 127.0.0.1:9090）：
//   GET  /healthz  进程活着 → 200（drain 中也 200）
//   GET  /readyz   可以接流量 → 200；drain 中 → 503
//   POST /drain    优雅停机（需要 WARDEN_CONTROL_TOKEN）
//
// 和 webhook 分开端口：长轮询模式也能用；蓝绿发布时旧进程的 /drain 可以由
// 发布脚本直接 curl，不必依赖 Unix 信号（Windows 也能用）。

#include "store.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

namespace warden {

class HealthServer : public std::enable_shared_from_this<HealthServer> {
public:
    HealthServer(Store& store, std::atomic<bool>& draining,
                 std::string instanceId, std::string version,
                 std::string controlToken);

    // bind/listen 失败直接抛：健康端口被占不应该静默降级
    void start(const boost::asio::any_io_executor& executor,
               const std::string& host, unsigned short port);

private:
    boost::asio::awaitable<void> acceptLoop(
        std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor);
    boost::asio::awaitable<void> session(
        boost::asio::ip::tcp::socket socket);

    Store& store_;
    std::atomic<bool>& draining_;
    std::string instanceId_;
    std::string version_;
    std::string controlToken_;
    std::chrono::steady_clock::time_point startedAt_;
};

}  // namespace warden
