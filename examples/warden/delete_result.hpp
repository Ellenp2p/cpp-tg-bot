#pragma once

// 删除结果分类：把 Telegram 的 400 描述映射成"消息已经没了 / 真失败"。
// 单独成头文件是为了能不依赖网络单测——这条逻辑错了会导致：
//   - 把"已被管理员手动删除"当成失败，反复重试 + 告警刷屏；
//   - 或者把"没权限"当成成功，静默漏删。

#include <string_view>

namespace warden {

inline bool deleteErrorMeansGone(std::string_view description) {
    return description.find("message to delete not found") !=
               std::string_view::npos ||
           description.find("message can't be deleted") !=
               std::string_view::npos ||
           description.find("message is too old") != std::string_view::npos ||
           description.find("MESSAGE_ID_INVALID") != std::string_view::npos ||
           description.find("MESSAGE_NOT_FOUND") != std::string_view::npos ||
           description.find("ephemeral message not found") !=
               std::string_view::npos;
}

}  // namespace warden
