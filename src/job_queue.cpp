#include <tgbot/job_queue.hpp>

#include <tgbot/util.hpp>

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace tgbot {

namespace {

std::int64_t toSeconds(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}

std::int64_t nowSeconds() {
    return toSeconds(std::chrono::system_clock::now());
}

// 指数退避，最长 5 分钟
std::int64_t backoffSeconds(int attempts) {
    const int capped = std::min(attempts, 8);
    return std::min<std::int64_t>(300, std::int64_t{1} << capped);
}

class Statement {
public:
    explicit Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string{"sqlite prepare failed: "} + sqlite3_errmsg(db));
        }
    }
    ~Statement() {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bindInt64(int index, std::int64_t value) { sqlite3_bind_int64(stmt_, index, value); }
    void bindText(int index, std::string_view value) {
        sqlite3_bind_text(
            stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    }
    // true = 有一行
    bool step() { return sqlite3_step(stmt_) == SQLITE_ROW; }
    void run() {
        if (sqlite3_step(stmt_) != SQLITE_DONE) {
            throw std::runtime_error("sqlite step failed");
        }
    }
    std::int64_t columnInt64(int index) { return sqlite3_column_int64(stmt_, index); }
    std::string columnText(int index) {
        const auto* text = sqlite3_column_text(stmt_, index);
        return text == nullptr ? std::string{} : std::string{reinterpret_cast<const char*>(text)};
    }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

} // namespace

JobQueue::JobQueue(std::string path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        throw std::runtime_error("cannot open job database: " + path);
    }
    exec("PRAGMA journal_mode=WAL");
    exec(
        "CREATE TABLE IF NOT EXISTS jobs("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " key TEXT NOT NULL UNIQUE,"
        " kind TEXT NOT NULL,"
        " payload TEXT NOT NULL DEFAULT '',"
        " state TEXT NOT NULL DEFAULT 'pending',"
        " attempts INTEGER NOT NULL DEFAULT 0,"
        " max_attempts INTEGER NOT NULL DEFAULT 8,"
        " next_at INTEGER NOT NULL,"
        " last_error TEXT,"
        " created_at INTEGER NOT NULL,"
        " updated_at INTEGER NOT NULL)");
    exec("CREATE INDEX IF NOT EXISTS idx_jobs_due ON jobs(state, next_at)");
    // 单消费者：上次崩溃时卡在 running 的任务重新放回待执行
    exec("UPDATE jobs SET state='pending' WHERE state='running'");
}

JobQueue::~JobQueue() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

void JobQueue::exec(const char* sql) const {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string message = err != nullptr ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("sqlite exec failed: " + message);
    }
}

void JobQueue::enqueue(const Job& job) {
    const auto now = nowSeconds();
    Statement stmt(db_,
                   "INSERT OR IGNORE INTO jobs"
                   "(key, kind, payload, state, attempts, max_attempts,"
                   " next_at, created_at, updated_at)"
                   " VALUES(?, ?, ?, 'pending', 0, ?, ?, ?, ?)");
    stmt.bindText(1, job.key);
    stmt.bindText(2, job.kind);
    stmt.bindText(3, job.payload);
    stmt.bindInt64(4, job.maxAttempts);
    stmt.bindInt64(5, toSeconds(job.runAt));
    stmt.bindInt64(6, now);
    stmt.bindInt64(7, now);
    stmt.run();
}

bool JobQueue::cancel(std::string_view key) {
    Statement stmt(db_, "DELETE FROM jobs WHERE key=? AND state='pending'");
    stmt.bindText(1, key);
    stmt.run();
    return sqlite3_changes(db_) > 0;
}

std::optional<JobRecord> JobQueue::claimNext() {
    const auto now = nowSeconds();
    exec("BEGIN IMMEDIATE");

    JobRecord record;
    {
        Statement select(db_,
                         "SELECT id, key, kind, payload, attempts FROM jobs"
                         " WHERE state='pending' AND next_at<=? ORDER BY next_at LIMIT 1");
        select.bindInt64(1, now);
        if (!select.step()) {
            exec("ROLLBACK");
            return std::nullopt;
        }
        record.id = select.columnInt64(0);
        record.key = select.columnText(1);
        record.kind = select.columnText(2);
        record.payload = select.columnText(3);
        record.attempts = static_cast<int>(select.columnInt64(4)) + 1;
    }

    {
        Statement update(db_,
                         "UPDATE jobs SET state='running',"
                         " attempts=attempts+1, updated_at=? WHERE id=?");
        update.bindInt64(1, now);
        update.bindInt64(2, record.id);
        update.run();
    }
    exec("COMMIT");
    return record;
}

void JobQueue::markDone(std::int64_t id) {
    Statement stmt(db_,
                   "UPDATE jobs SET state='done', last_error=NULL,"
                   " updated_at=? WHERE id=?");
    stmt.bindInt64(1, nowSeconds());
    stmt.bindInt64(2, id);
    stmt.run();
}

void JobQueue::markRetry(std::int64_t id, std::string_view error) {
    const auto now = nowSeconds();
    const auto next = now + backoffSeconds(1);
    Statement stmt(db_,
                   "UPDATE jobs SET"
                   " state=CASE WHEN attempts>=max_attempts THEN 'dead'"
                   "            ELSE 'pending' END,"
                   " next_at=?, last_error=?, updated_at=? WHERE id=?");
    stmt.bindInt64(1, next);
    stmt.bindText(2, error);
    stmt.bindInt64(3, now);
    stmt.bindInt64(4, id);
    stmt.run();
}

std::size_t JobQueue::countPending() const {
    Statement stmt(db_, "SELECT COUNT(*) FROM jobs WHERE state='pending'");
    return stmt.step() ? static_cast<std::size_t>(stmt.columnInt64(0)) : 0;
}

std::size_t JobQueue::countDead() const {
    Statement stmt(db_, "SELECT COUNT(*) FROM jobs WHERE state='dead'");
    return stmt.step() ? static_cast<std::size_t>(stmt.columnInt64(0)) : 0;
}

boost::asio::awaitable<void> JobQueue::runWorker(Handler handler,
                                                 std::chrono::milliseconds pollInterval) {
    while (true) {
        auto job = claimNext();
        if (!job.has_value()) {
            co_await co_sleep(pollInterval);
            continue;
        }
        try {
            co_await handler(*job);
            markDone(job->id);
        } catch (const std::exception& e) {
            spdlog::warn(
                "job {} ({}) 第 {} 次失败: {}", job->key, job->kind, job->attempts, e.what());
            markRetry(job->id, e.what());
        }
    }
}

} // namespace tgbot
