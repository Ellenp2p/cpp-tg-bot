#include <tgbot/grab_queue.hpp>

#include <tgbot/util.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/this_coro.hpp>

#include <exception>
#include <memory>
#include <utility>

namespace tgbot {

namespace net = boost::asio;

GrabQueue::GrabQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

GrabQueue::Accept GrabQueue::submit(std::int64_t userId, std::string userName) {
    if (closed_) {
        return Accept::kClosed;
    }
    if (byUser_.count(userId) != 0) {
        return Accept::kDuplicate;
    }
    if (accepted_ >= capacity_) {
        return Accept::kFull; // 没名额了：当场拒绝，不让他白花手续费
    }

    GrabRequest request;
    request.seq = nextSeq_++;
    request.userId = userId;
    request.userName = std::move(userName);

    byUser_.emplace(userId, request.seq);
    queue_.push_back(std::move(request));
    ++accepted_;
    return Accept::kAccepted;
}

std::optional<GrabRequest> GrabQueue::pop() {
    if (queue_.empty()) {
        return std::nullopt;
    }
    auto request = std::move(queue_.front());
    queue_.pop_front();
    return request;
}

void GrabQueue::close() {
    closed_ = true;
}

void GrabQueue::resetClosed() {
    closed_ = false;
}

GrabRunner::GrabRunner() : GrabRunner(Options{}) {}

GrabRunner::GrabRunner(Options options) : options_(options) {
    if (options_.maxInFlight == 0) {
        options_.maxInFlight = 1;
    }
}

boost::asio::awaitable<std::size_t> GrabRunner::drain(GrabQueue& queue,
                                                      Submit submit,
                                                      Render render) {
    const auto executor = co_await net::this_coro::executor;
    std::size_t processed = 0;

    while (true) {
        // 1) 取一批（顺序 = 入队顺序）
        std::vector<GrabRequest> batch;
        batch.reserve(options_.maxInFlight);
        while (batch.size() < options_.maxInFlight) {
            auto request = queue.pop();
            if (!request.has_value()) {
                break;
            }
            batch.push_back(std::move(*request));
        }
        if (batch.empty()) {
            break;
        }

        // 2) 并发提交（同一个 io_context 上并行推进，结果各自落位）
        struct BatchState {
            std::size_t pending = 0;
            std::vector<std::optional<GrabResult>> slots;
        };
        auto state = std::make_shared<BatchState>();
        state->slots.resize(batch.size());
        state->pending = batch.size();

        for (std::size_t i = 0; i < batch.size(); ++i) {
            const GrabRequest request = batch[i];
            GrabRunner::Submit task = submit;
            net::co_spawn(
                executor,
                [state, i, request, task]() -> net::awaitable<void> {
                    GrabResult result;
                    try {
                        result = co_await task(request);
                    } catch (const std::exception& e) {
                        result.seq = request.seq;
                        result.userId = request.userId;
                        result.ok = false;
                        result.detail = std::string{"提交失败: "} + e.what();
                    }
                    state->slots[i] = std::move(result);
                    if (state->pending > 0) {
                        --state->pending;
                    }
                },
                net::detached);
        }

        // 3) 等这一批结束（单批超时就当作失败，继续往下走）
        const auto deadline = std::chrono::steady_clock::now() + options_.batchTimeout;
        while (state->pending > 0 && std::chrono::steady_clock::now() < deadline) {
            co_await co_sleep(options_.poll);
        }

        // 4) 按 seq 顺序整理（超时没回来的补一条失败）
        std::vector<GrabResult> ordered;
        ordered.reserve(batch.size());
        for (std::size_t i = 0; i < batch.size(); ++i) {
            if (state->slots[i].has_value()) {
                ordered.push_back(std::move(*state->slots[i]));
            } else {
                GrabResult missing;
                missing.seq = batch[i].seq;
                missing.userId = batch[i].userId;
                missing.ok = false;
                missing.detail = "超时未返回";
                ordered.push_back(std::move(missing));
            }
        }

        // 5) 一次性把这一批按顺序交给渲染（同一批内顺序天然正确）
        co_await render(std::move(ordered));
        processed += batch.size();
    }

    co_return processed;
}

} // namespace tgbot
