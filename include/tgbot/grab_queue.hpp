#pragma once

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tgbot {

// 一个抢红包请求（按下单顺序编号）
struct GrabRequest {
    std::uint64_t seq = 0;
    std::int64_t userId = 0;
    std::string userName;
};

// 抢红包结果
struct GrabResult {
    std::uint64_t seq = 0;
    std::int64_t userId = 0;
    bool ok = false;
    std::string detail;  // 成功时是金额，失败时是原因
    bool fromNetwork = false;  // 仅供测试观察并发
};

// 抢红包队列：容量 = 剩余份数，先进先出，超员直接拒绝。
//
// 为什么需要它：人多时如果每个点击都立刻发一笔链上交易，会同时打爆
// RPC 和 Telegram；而且同一份红包被并发领取会出现版本冲突。
// 这里先做"本地闸门"：只放前 N 个进来，其余的当场回错误。
class GrabQueue {
public:
    enum class Accept {
        kAccepted,   // 入队成功
        kDuplicate,  // 这个人已经在队列里了
        kFull,       // 名额满了（杜绝"抢不到还要花钱"的请求）
        kClosed,     // 红包已完结
    };

    explicit GrabQueue(std::size_t capacity);

    Accept submit(std::int64_t userId, std::string userName = {});

    // 取下一个待处理请求（FIFO）
    std::optional<GrabRequest> pop();

    // 是否还有名额
    bool full() const { return accepted_ >= capacity_; }
    std::size_t accepted() const { return accepted_; }
    std::size_t pending() const { return queue_.size(); }
    std::size_t capacity() const { return capacity_; }

    // 红包抢完 / 过期时关闭，之后一律拒绝
    void close();
    bool closed() const { return closed_; }
    void resetClosed();

private:
    std::size_t capacity_;
    std::size_t accepted_ = 0;
    std::uint64_t nextSeq_ = 1;
    bool closed_ = false;
    std::deque<GrabRequest> queue_;
    std::map<std::int64_t, std::uint64_t> byUser_;  // userId -> seq
};

// 批量执行器：拉一批请求并发提交，但结果严格按 seq 顺序交给渲染回调。
//
// 这样即使第 3 个请求先返回，也会等第 1、2 个先渲染，用户在 message 里
// 看到的永远是"按大家点击的先后顺序"追加的行，不会跳来跳去。
class GrabRunner {
public:
    struct Options {
        std::size_t maxInFlight = 5;                // 同时最多几笔在飞
        std::chrono::milliseconds poll{2};          // 等待完成状态的轮询间隔
        std::chrono::seconds batchTimeout{120};     // 单批最长等待
    };

    using Submit = std::function<boost::asio::awaitable<GrabResult>(GrabRequest)>;
    using Render =
        std::function<boost::asio::awaitable<void>(std::vector<GrabResult>)>;

    // 注意：不能写 Options options = {}（GCC/clang 不允许嵌套类型的默认成员
    // 初始化器作默认实参），所以拆成两个重载
    GrabRunner();
    explicit GrabRunner(Options options);

    // 把队列里已接收的请求全部处理掉，按 seq 顺序分批渲染。
    // 返回处理条数。
    boost::asio::awaitable<std::size_t> drain(GrabQueue& queue, Submit submit,
                                              Render render);

private:
    Options options_;
};

}  // namespace tgbot
