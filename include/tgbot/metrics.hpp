#pragma once

// 轻量 Prometheus 指标注册表：不依赖 prometheus-cpp。
// 所有埋点函数都可以在单 io_context 线程里无锁调用；内部对 label map
// 使用 mutex，保证 /metrics 与业务协程并发时安全。

#include <string>
#include <string_view>

namespace tgbot::metrics {

// ---- update ingress ----
void updateReceived(std::string_view type);
void updateAccepted();
void updateRejected(std::string_view reason);
void setUpdateQueue(double depth, double bytes);
void setUpdateQueueCapacity(double count, double bytes);
void setWorkerCount(int count);
void workerBusy(int delta);
void observeUpdateWait(double seconds);
void observeUpdateHandle(std::string_view type, double seconds);

// ---- Telegram API ----
void telegramRequestStarted(std::string_view method);
void telegramRequestFinished(std::string_view method, double seconds, bool ok);
void telegramError(std::string_view method, long code);
void telegram429(std::string_view method);
void telegramRetry(std::string_view method);

// ---- deletion engine ----
void setDeletionsPending(int value);
void deletionFinished(std::string_view result);
void observeDeletionDueLag(double seconds);
void observeDbWrite(std::string_view op, double seconds);
void dbError(std::string_view op);

// ---- business ----
void command(std::string_view name);
void reply(std::string_view channel);

// ---- runtime ----
void setEventLoopLag(double seconds);
void setActiveChats(int count);
void setBuildInfo(std::string_view version, std::string_view instance);

// Prometheus text exposition format 0.0.4.
std::string render();

} // namespace tgbot::metrics
