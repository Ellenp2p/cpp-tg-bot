#pragma once

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

struct sqlite3;

namespace tgbot {

// 一条可持久化的延迟任务。
// key 是幂等键：同一个 key 重复入队会被忽略（重试、重启都不会重复执行）。
struct Job {
    std::string key;
    std::string kind;    // 业务类型，worker 自己按 kind 分发
    std::string payload; // 建议放 JSON
    std::chrono::system_clock::time_point runAt = std::chrono::system_clock::now();
    int maxAttempts = 8;
};

struct JobRecord {
    std::int64_t id = 0;
    std::string key;
    std::string kind;
    std::string payload;
    int attempts = 0;
};

// 基于 SQLite 的持久化任务队列：延迟执行 + 失败退避重试 + 死信。
//
// 为什么不用内存定时器：退款这类任务一旦丢，钱就真丢了。任务落库后，
// 进程重启仍会继续执行；key 唯一保证不会因为重试而重复退款。
//
// 假定单消费者（本项目的 io_context 是单线程的），不做多进程抢占。
class JobQueue {
public:
    using Handler = std::function<boost::asio::awaitable<void>(const JobRecord&)>;

    // path 传 ":memory:" 可用于测试
    explicit JobQueue(std::string path);
    ~JobQueue();
    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    // 入队；key 已存在则忽略（幂等）
    void enqueue(const Job& job);

    // 取消一个还没执行的任务
    bool cancel(std::string_view key);

    // 原子领取一条到期的任务；没有则返回 nullopt
    std::optional<JobRecord> claimNext();

    void markDone(std::int64_t id);
    // 失败：指数退避重排；超过 maxAttempts 进死信（dead）
    void markRetry(std::int64_t id, std::string_view error);

    std::size_t countPending() const;
    std::size_t countDead() const;

    // 工作循环：handler 抛异常即视为失败并重试
    boost::asio::awaitable<void> runWorker(
        Handler handler, std::chrono::milliseconds pollInterval = std::chrono::milliseconds{500});

private:
    void exec(const char* sql) const;
    sqlite3* db_ = nullptr;
};

} // namespace tgbot
