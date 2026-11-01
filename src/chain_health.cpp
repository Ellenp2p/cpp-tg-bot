#include <tgbot/chain_health.hpp>

namespace tgbot {

std::string_view toString(ChainTxStatus status) {
    switch (status) {
        case ChainTxStatus::kConfirmed:
            return "confirmed";
        case ChainTxStatus::kFailed:
            return "failed";
        case ChainTxStatus::kPending:
            return "pending";
        case ChainTxStatus::kNotFound:
            return "not_found";
        case ChainTxStatus::kUnknown:
            break;
    }
    return "unknown";
}

ChainMonitor::ChainMonitor(Options options) : options_(options) {}

void ChainMonitor::recordSuccess() {
    failures_ = 0;
    if (paused_) {
        paused_ = false;
    }
}

void ChainMonitor::recordFailure(std::string_view error) {
    ++failures_;
    lastError_ = std::string{error};
    if (!paused_ && failures_ >= options_.failureThreshold) {
        paused_ = true;
        pausedAt_ = std::chrono::steady_clock::now();
        lastProbe_ = pausedAt_;
    }
}

bool ChainMonitor::shouldProbe(std::chrono::steady_clock::time_point now) const {
    if (!paused_) {
        return false;
    }
    return now - lastProbe_ >= options_.pausedProbe;
}

}  // namespace tgbot
