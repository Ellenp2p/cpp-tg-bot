#pragma once

#include <tgbot/panel.hpp>
#include <tgbot/red_packet_ledger.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>

namespace tgbot {

// 群红包面板：一条消息就是红包本身，状态全部存在 SQLite 账本里。
//
//   - 进程重启后仍在（账本持久化）
//   - 领取是带条件的原子 UPDATE，不可能超发
//   - 到期由任务队列触发退款（见 red_packet_service.hpp），不是内存定时器
class RedPacketPanel : public Panel {
public:
    struct Options {
        std::string blessing = "恭喜发财，大吉大利";
        std::string grabLabel = "🧧 抢红包";
        bool creatorCanGrab = false;
        bool showList = true;
        std::uint32_t seed = 0;  // 0 = 随机；测试可固定
    };

    RedPacketPanel(std::shared_ptr<RedPacketLedger> ledger,
                   std::int64_t packetId, Options options = {});

    std::string_view type() const override { return "red_packet"; }
    PanelView render() const override;
    bool onClick(const PanelEvent& event, PanelEffects& effects) override;
    std::optional<std::chrono::system_clock::time_point> deadline() const override;
    void onDeadline(PanelEffects& effects) override;
    bool finished() const override;

    std::int64_t packetId() const { return packetId_; }

private:
    // 二倍均值法：单份上限 = 剩余均值 × 2，最后一份拿尾数
    std::int64_t split(std::int64_t remaining, int count);

    std::shared_ptr<RedPacketLedger> ledger_;
    std::int64_t packetId_ = 0;
    Options options_;
    mutable std::mt19937 rng_;
};

}  // namespace tgbot
