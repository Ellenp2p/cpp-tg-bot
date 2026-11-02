#pragma once

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

namespace tgbot {

// 链上交易状态（对账用）
enum class ChainTxStatus {
    kConfirmed, // 已上链且成功
    kFailed,    // 已上链但失败（reverted / revert）
    kPending,   // 还在 mempool，别急
    kNotFound,  // 查不到这笔交易
    kUnknown,   // 节点答非所问（多半是链/RPC 出问题了）
};

std::string_view toString(ChainTxStatus status);

// 按 tx hash 查链
using ChainProbe = std::function<boost::asio::awaitable<ChainTxStatus>(std::string_view txRef)>;

// 告警出口：echo_bot 里就是"给管理员发消息"
using AlertSink =
    std::function<boost::asio::awaitable<void>(std::string_view level, std::string_view message)>;

// 熔断器：连续失败到阈值就认定"链/RPC 挂了"，暂停一切资金操作，
// 只留对账任务去探测恢复。避免 RPC 挂掉时机器人疯狂重试、把队列打爆。
class ChainMonitor {
public:
    struct Options {
        int failureThreshold = 5;             // 连续失败多少次判定挂了
        std::chrono::seconds pausedProbe{60}; // 挂了之后多久探一次
    };

    // 注意：不能写 Options options = {}——GCC/clang 不允许用嵌套类型的
    // 默认成员初始化器当默认实参（MSVC 会放过），所以拆成两个重载
    ChainMonitor();
    explicit ChainMonitor(Options options);

    void recordSuccess();
    void recordFailure(std::string_view error);

    bool paused() const { return paused_; }
    int consecutiveFailures() const { return failures_; }
    const std::string& lastError() const { return lastError_; }

    // 熔断期间：是否到了下一次探测的时间
    bool shouldProbe(std::chrono::steady_clock::time_point now) const;

private:
    Options options_;
    int failures_ = 0;
    bool paused_ = false;
    std::string lastError_;
    std::chrono::steady_clock::time_point pausedAt_{};
    std::chrono::steady_clock::time_point lastProbe_{};
};

} // namespace tgbot
