#pragma once

#include <tgbot/panel.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace tgbot {

// 群红包面板：和微信群里一样，一条消息就是红包本身。
//   点「抢红包」→ 当场分到一笔钱，消息原地刷新"谁抢了多少、还剩多少"
//   抢完 → 消息变成"已抢完"，按钮消失
// 全程在群里可见，不依赖私聊/私密消息。
class RedPacketPanel : public Panel {
public:
    struct Config {
        std::string blessing = "恭喜发财，大吉大利";
        std::string grabLabel = "🧧 抢红包";
        std::int64_t totalCents = 10000;  // 总金额（分），默认 ¥100.00
        int totalCount = 5;               // 份数
        std::int64_t creatorId = 0;
        bool creatorCanGrab = false;      // 和微信一致：发起人不抢自己的
        bool showList = true;             // 是否在消息里列出谁抢了多少
        std::chrono::seconds ttl{3600};   // 未抢完的有效期
        std::uint32_t seed = 0;           // 0 = 随机；测试可固定
    };

    explicit RedPacketPanel(Config config);

    std::string_view type() const override { return "red_packet"; }
    PanelView render() const override;
    bool onClick(const PanelEvent& event, PanelEffects& effects) override;
    std::optional<std::chrono::system_clock::time_point> deadline() const override;
    void onDeadline(PanelEffects& effects) override;
    bool finished() const override { return remainingCount_ <= 0 || remainingCents_ <= 0; }

    // 查询接口（测试用）
    std::int64_t remainingCents() const { return remainingCents_; }
    int remainingCount() const { return remainingCount_; }
    std::size_t grabberCount() const { return grabs_.size(); }
    std::int64_t grabbedCents(std::int64_t userId) const;

private:
    struct Grab {
        std::int64_t userId = 0;
        std::string name;
        std::int64_t cents = 0;
    };

    // 二倍均值法：每人拿到 1 分 ~ 剩余均值×2，最后一人拿剩下的
    std::int64_t split(std::int64_t remaining, int count);

    Config config_;
    std::int64_t remainingCents_ = 0;
    int remainingCount_ = 0;
    bool expired_ = false;
    std::vector<Grab> grabs_;
    std::chrono::system_clock::time_point deadline_;
    mutable std::mt19937 rng_;
};

}  // namespace tgbot
