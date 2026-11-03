#pragma once

// 媒体豁免：按 “发送者 + 媒体类型” 在排程前过滤。
// 被豁免的消息不生成 deletions 任务，所以恢复后也不会补删。

#include "media.hpp"
#include "store.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace warden {

inline bool hasExemption(const ChatSettings& settings,
                         std::string_view scope,
                         std::int64_t userId,
                         std::string_view username,
                         std::string_view kind) {
    for (const auto& item : settings.exemptions) {
        if (item.scope != scope || item.kind != kind) {
            continue;
        }
        if (scope == "user") {
            if (item.userId != userId) {
                continue;
            }
        } else if (scope == "username") {
            if (item.username != username) {
                continue;
            }
        }
        return true;
    }
    return false;
}

inline bool isMediaExempt(const ChatSettings& settings,
                          std::int64_t senderId,
                          std::string_view username,
                          bool senderIsAdmin,
                          bool anonymousAdmin,
                          std::string_view kind) {
    if (settings.exemptions.empty()) {
        return false;
    }
    if ((senderIsAdmin || anonymousAdmin) && hasExemption(settings, "admin", 0, {}, kind)) {
        return true;
    }
    if (senderId != 0 && hasExemption(settings, "user", senderId, {}, kind)) {
        return true;
    }
    if (!username.empty() && hasExemption(settings, "username", 0, username, kind)) {
        return true;
    }
    return false;
}

} // namespace warden
