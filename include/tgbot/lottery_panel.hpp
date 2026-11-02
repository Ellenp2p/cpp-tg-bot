#pragma once

#include <tgbot/panel.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace tgbot {

// 抽奖面板：一条消息就是操作台。
//   [参与抽奖] → 计入名单、原地刷新人数
//   [开奖]     → 发起者可点；或倒计时到点自动开奖
// 中奖者各收到一条"只有自己可见"的私密消息（专属红包码）。
//
// 输入输出都在 PanelEffects 里，面板本身不碰网络，可以直接单测。
class LotteryPanel : public Panel {
public:
    struct Config {
        std::string title = "抽奖";
        std::string prize = "专属红包";
        std::string joinLabel = "🎉 参与抽奖";
        std::string drawLabel = "🎲 开奖";
        int winnerCount = 1;
        std::chrono::seconds duration{300};
        std::int64_t creatorId = 0; // 0 表示谁都能开奖
        // 中奖者是否额外收到一条"只有自己可见"的消息（专属码）。
        // 默认关闭：中奖结果全部在群里公开，不需要去私聊找机器人。
        bool rewardPrivate = false;
        // 中奖者的专属码；不设置则用内置生成器
        std::function<std::string(std::int64_t)> rewardCode;
    };

    explicit LotteryPanel(Config config);

    std::string_view type() const override { return "lottery"; }
    PanelView render() const override;
    bool onClick(const PanelEvent& event, PanelEffects& effects) override;
    std::optional<std::chrono::system_clock::time_point> deadline() const override;
    void onDeadline(PanelEffects& effects) override;
    bool finished() const override { return closed_; }

    // 查询接口（测试/业务用）
    std::size_t participantCount() const { return participants_.size(); }
    bool joined(std::int64_t userId) const { return participants_.count(userId) > 0; }
    const std::vector<std::int64_t>& winners() const { return winners_; }
    bool closed() const { return closed_; }

private:
    void draw(PanelEffects& effects);
    std::string rewardFor(std::int64_t userId) const;
    std::chrono::seconds remaining() const;

    Config config_;
    std::map<std::int64_t, std::string> participants_; // userId -> 显示名
    std::vector<std::int64_t> winners_;
    bool closed_ = false;
    std::chrono::system_clock::time_point deadline_;
    mutable std::mt19937 rng_{std::random_device{}()};
};

} // namespace tgbot
