#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace tgbot {

// 键值状态存储抽象：业务侧只依赖这个接口，后端可换
// （内置 SQLite 实现 SqliteStateStore；小项目也可以自己写个 JSON 后端）。
//
// 典型用法——按群存开关：
//   store.set("sticker_expire:" + std::to_string(chatId), "1");
//   const bool on = store.get(key).has_value();
//
// 约定：单 io_context 线程内使用；值为字符串，复杂结构请自行序列化（如 JSON）。
class StateStore {
public:
    virtual ~StateStore() = default;

    virtual std::optional<std::string> get(std::string_view key) = 0;
    virtual void set(std::string_view key, std::string_view value) = 0;
    virtual void erase(std::string_view key) = 0;
};

}  // namespace tgbot
