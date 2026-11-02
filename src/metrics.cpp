#include <tgbot/metrics.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#else
#include <sys/resource.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#endif

namespace tgbot::metrics {
namespace {

using Clock = std::chrono::steady_clock;
const auto kStartedAt = Clock::now();

std::string escapeLabel(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

struct CounterFamily {
    std::string_view name;
    std::string_view help;
    std::string_view labelName; // 空 = 无 label
    std::mutex mu;
    std::map<std::string, std::uint64_t> values;

    void inc(std::string_view label) {
        std::lock_guard lock(mu);
        ++values[std::string(label)];
    }
};

struct Gauge {
    std::atomic<double> value{0};
    void set(double v) { value.store(v, std::memory_order_relaxed); }
};

struct Histogram {
    static constexpr std::array<double, 13> kBounds{
        0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0, 30.0};

    std::array<std::atomic<std::uint64_t>, kBounds.size()> buckets{};
    std::atomic<std::uint64_t> count{0};
    std::mutex sumMu;
    double sum = 0;

    void observe(double seconds) {
        if (!std::isfinite(seconds) || seconds < 0) {
            seconds = 0;
        }
        for (std::size_t i = 0; i < kBounds.size(); ++i) {
            if (seconds <= kBounds[i]) {
                buckets[i].fetch_add(1, std::memory_order_relaxed);
            }
        }
        count.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(sumMu);
        sum += seconds;
    }
};

struct HistogramFamily {
    std::string_view name;
    std::string_view help;
    std::string_view labelName;
    std::mutex mu;
    std::map<std::string, std::shared_ptr<Histogram>> values;

    std::shared_ptr<Histogram> get(std::string_view label) {
        std::lock_guard lock(mu);
        auto& slot = values[std::string(label)];
        if (!slot) {
            slot = std::make_shared<Histogram>();
        }
        return slot;
    }
};

CounterFamily updatesReceived{"warden_updates_received_total", "Received updates by type.", "type"};
CounterFamily updatesAccepted{
    "warden_updates_accepted_total", "Updates accepted into the queue.", ""};
CounterFamily updatesRejected{
    "warden_updates_rejected_total", "Updates rejected by backpressure.", "reason"};
CounterFamily telegramRequests{
    "warden_telegram_requests_total", "Telegram API logical calls by method.", "method"};
CounterFamily telegramErrors{"warden_telegram_errors_total", "Telegram API errors.", "method"};
CounterFamily telegram429Counter{
    "warden_telegram_429_total", "Telegram API 429 responses.", "method"};
CounterFamily telegramRetries{"warden_telegram_retries_total", "Telegram API retries.", "method"};
CounterFamily deletionsFinished{
    "warden_deletions_finished_total", "Deletion jobs finished by result.", "result"};
CounterFamily dbErrors{"warden_db_errors_total", "SQLite operation errors.", "op"};
CounterFamily commands{"warden_commands_total", "Command invocations.", "command"};
CounterFamily replies{"warden_replies_total", "Replies by delivery channel.", "channel"};

Gauge updateQueueDepth;
Gauge updateQueueBytes;
Gauge updateQueueCapacity;
Gauge updateQueueCapacityBytes;
Gauge workerBusyGauge;
Gauge workerCountGauge;
Gauge telegramInflight;
Gauge deletionsPendingGauge;
Gauge activeChatsGauge;
Gauge eventLoopLagGauge;

HistogramFamily updateWait{"warden_update_wait_seconds", "Queue wait before handling.", ""};
HistogramFamily updateHandle{"warden_update_handle_seconds", "Update handler duration.", "type"};
HistogramFamily telegramDuration{
    "warden_telegram_request_seconds", "Telegram API duration.", "method"};
HistogramFamily deletionDueLag{
    "warden_deletions_due_lag_seconds", "Delay between due time and deletion attempt.", ""};
HistogramFamily dbWrite{"warden_db_write_seconds", "SQLite operation duration.", "op"};

std::mutex buildMu;
std::string buildVersion;
std::string buildInstance;

void writeCounter(std::ostringstream& out, CounterFamily& family) {
    out << "# HELP " << family.name << ' ' << family.help << '\n';
    out << "# TYPE " << family.name << " counter\n";
    std::lock_guard lock(family.mu);
    for (const auto& [label, value] : family.values) {
        out << family.name;
        if (!family.labelName.empty()) {
            out << '{' << family.labelName << "=\"" << escapeLabel(label) << "\"}";
        }
        out << ' ' << value << '\n';
    }
}

void writeGauge(std::ostringstream& out,
                std::string_view name,
                std::string_view help,
                const Gauge& gauge) {
    out << "# HELP " << name << ' ' << help << '\n';
    out << "# TYPE " << name << " gauge\n";
    out << name << ' ' << gauge.value.load(std::memory_order_relaxed) << '\n';
}

void writeHistogram(std::ostringstream& out, HistogramFamily& family) {
    out << "# HELP " << family.name << ' ' << family.help << '\n';
    out << "# TYPE " << family.name << " histogram\n";
    std::lock_guard lock(family.mu);
    for (const auto& [label, hist] : family.values) {
        const std::string labels = family.labelName.empty() ? std::string{}
                                                            : std::string{family.labelName} +
                                                                  "=\"" + escapeLabel(label) + "\"";
        for (std::size_t i = 0; i < Histogram::kBounds.size(); ++i) {
            out << family.name << "_bucket{";
            if (!labels.empty()) {
                out << labels << ',';
            }
            out << "le=\"" << Histogram::kBounds[i] << "\"} "
                << hist->buckets[i].load(std::memory_order_relaxed) << '\n';
        }
        out << family.name << "_bucket{";
        if (!labels.empty()) {
            out << labels << ',';
        }
        out << "le=\"+Inf\"} " << hist->count.load(std::memory_order_relaxed) << '\n';
        double sum = 0;
        {
            std::lock_guard sumLock(hist->sumMu);
            sum = hist->sum;
        }
        out << family.name << "_sum{";
        if (!labels.empty()) {
            out << labels;
        }
        out << "} " << std::setprecision(9) << sum << '\n';
        out << family.name << "_count{";
        if (!labels.empty()) {
            out << labels;
        }
        out << "} " << hist->count.load(std::memory_order_relaxed) << '\n';
    }
}

std::uint64_t currentRssBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(
            GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return static_cast<std::uint64_t>(pmc.WorkingSetSize);
    }
    return 0;
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    std::uint64_t totalPages = 0;
    std::uint64_t residentPages = 0;
    if (statm >> totalPages >> residentPages) {
        return residentPages * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    }
    return 0;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(
            mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
        KERN_SUCCESS) {
        return info.resident_size;
    }
    return 0;
#else
    return 0;
#endif
}

double currentCpuSeconds() {
#ifdef _WIN32
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return 0;
    }
    ULARGE_INTEGER k{};
    ULARGE_INTEGER u{};
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) / 10'000'000.0;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
    const auto toSeconds = [](const timeval& tv) {
        return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1'000'000.0;
    };
    return toSeconds(usage.ru_utime) + toSeconds(usage.ru_stime);
#endif
}

} // namespace

void updateReceived(std::string_view type) {
    updatesReceived.inc(type);
}
void updateAccepted() {
    updatesAccepted.inc("");
}
void updateRejected(std::string_view reason) {
    updatesRejected.inc(reason);
}

void setUpdateQueue(double depth, double bytes) {
    updateQueueDepth.set(depth);
    updateQueueBytes.set(bytes);
}

void setUpdateQueueCapacity(double count, double bytes) {
    updateQueueCapacity.set(count);
    updateQueueCapacityBytes.set(bytes);
}

void setWorkerCount(int count) {
    workerCountGauge.set(count);
}
void workerBusy(int delta) {
    auto value =
        static_cast<std::int64_t>(workerBusyGauge.value.load(std::memory_order_relaxed)) + delta;
    workerBusyGauge.set(static_cast<double>(std::max<std::int64_t>(0, value)));
}

void observeUpdateWait(double seconds) {
    updateWait.get("")->observe(seconds);
}
void observeUpdateHandle(std::string_view type, double seconds) {
    updateHandle.get(type)->observe(seconds);
}

void telegramRequestStarted(std::string_view method) {
    telegramRequests.inc(method);
    telegramInflight.set(telegramInflight.value.load(std::memory_order_relaxed) + 1);
}

void telegramRequestFinished(std::string_view method, double seconds, bool ok) {
    telegramInflight.set(
        (std::max)(0.0, telegramInflight.value.load(std::memory_order_relaxed) - 1));
    telegramDuration.get(method)->observe(seconds);
    (void)ok;
}

void telegramError(std::string_view method, long code) {
    // 方法维度足够定位问题；错误码放进日志，避免 Prometheus label 基数膨胀。
    (void)code;
    telegramErrors.inc(method);
}

void telegram429(std::string_view method) {
    telegram429Counter.inc(method);
}
void telegramRetry(std::string_view method) {
    telegramRetries.inc(method);
}

void setDeletionsPending(int value) {
    deletionsPendingGauge.set(value);
}
void deletionFinished(std::string_view result) {
    deletionsFinished.inc(result);
}
void observeDeletionDueLag(double seconds) {
    deletionDueLag.get("")->observe(seconds);
}

void observeDbWrite(std::string_view op, double seconds) {
    dbWrite.get(op)->observe(seconds);
}
void dbError(std::string_view op) {
    dbErrors.inc(op);
}

void command(std::string_view name) {
    commands.inc(name);
}
void reply(std::string_view channel) {
    replies.inc(channel);
}

void setEventLoopLag(double seconds) {
    eventLoopLagGauge.set(seconds);
}
void setActiveChats(int count) {
    activeChatsGauge.set(count);
}

void setBuildInfo(std::string_view version, std::string_view instance) {
    std::lock_guard lock(buildMu);
    buildVersion = version;
    buildInstance = instance;
}

std::string render() {
    std::ostringstream out;
    writeCounter(out, updatesReceived);
    writeCounter(out, updatesAccepted);
    writeCounter(out, updatesRejected);
    writeCounter(out, telegramRequests);
    writeCounter(out, telegramErrors);
    writeCounter(out, telegram429Counter);
    writeCounter(out, telegramRetries);
    writeCounter(out, deletionsFinished);
    writeCounter(out, dbErrors);
    writeCounter(out, commands);
    writeCounter(out, replies);

    writeGauge(out, "warden_update_queue_depth", "Queued updates.", updateQueueDepth);
    writeGauge(out, "warden_update_queue_bytes", "Queued update bytes.", updateQueueBytes);
    writeGauge(
        out, "warden_update_queue_capacity", "Queue capacity in updates.", updateQueueCapacity);
    writeGauge(out,
               "warden_update_queue_capacity_bytes",
               "Queue capacity in bytes.",
               updateQueueCapacityBytes);
    writeGauge(out, "warden_worker_busy", "Workers currently handling an update.", workerBusyGauge);
    writeGauge(out, "warden_worker_count", "Configured update workers.", workerCountGauge);
    writeGauge(out, "warden_telegram_inflight", "Telegram API calls in flight.", telegramInflight);
    writeGauge(out, "warden_deletions_pending", "Pending deletion jobs.", deletionsPendingGauge);
    writeGauge(out, "warden_active_chats", "Known active chats.", activeChatsGauge);
    writeGauge(out, "warden_event_loop_lag_seconds", "io_context timer lag.", eventLoopLagGauge);

    writeHistogram(out, updateWait);
    writeHistogram(out, updateHandle);
    writeHistogram(out, telegramDuration);
    writeHistogram(out, deletionDueLag);
    writeHistogram(out, dbWrite);

    const auto uptime = std::chrono::duration<double>(Clock::now() - kStartedAt).count();
    out << "# HELP warden_uptime_seconds Process uptime.\n";
    out << "# TYPE warden_uptime_seconds gauge\n";
    out << "warden_uptime_seconds " << uptime << '\n';

    out << "# HELP warden_process_resident_memory_bytes Process RSS.\n";
    out << "# TYPE warden_process_resident_memory_bytes gauge\n";
    out << "warden_process_resident_memory_bytes " << currentRssBytes() << '\n';

    out << "# HELP warden_process_cpu_seconds_total Process CPU time.\n";
    out << "# TYPE warden_process_cpu_seconds_total counter\n";
    out << "warden_process_cpu_seconds_total " << currentCpuSeconds() << '\n';

    {
        std::lock_guard lock(buildMu);
        out << "# HELP warden_build_info Build metadata.\n";
        out << "# TYPE warden_build_info gauge\n";
        out << "warden_build_info{version=\"" << escapeLabel(buildVersion) << "\",instance=\""
            << escapeLabel(buildInstance) << "\"} 1\n";
    }

    return out.str();
}

} // namespace tgbot::metrics
