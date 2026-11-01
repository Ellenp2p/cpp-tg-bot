#include <tgbot/red_packet_panel.hpp>

#include <tgbot/rich_text.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace tgbot {

namespace {

std::string formatMoney(std::int64_t cents) {
    const auto yuan = cents / 100;
    const auto fen = cents % 100;
    std::string out = std::to_string(yuan) + ".";
    if (fen < 10) {
        out += "0";
    }
    out += std::to_string(fen);
    return out;
}

}  // namespace

RedPacketPanel::RedPacketPanel(Config config)
    : config_(std::move(config)),
      remainingCents_(config_.totalCents),
      remainingCount_(std::max(1, config_.totalCount)),
      deadline_(std::chrono::system_clock::now() + config_.ttl),
      rng_(config_.seed != 0 ? config_.seed : std::random_device{}()) {}

std::int64_t RedPacketPanel::split(std::int64_t remaining, int count) {
    if (count <= 1) {
        return remaining;  // 最后一份全给
    }
    // 二倍均值：单份上限 = 剩余均值 × 2，避免有人独吞
    const auto maxPart = std::max<std::int64_t>(1, remaining * 2 / count);
    std::uniform_int_distribution<std::int64_t> dist(1, maxPart);
    const auto cap = remaining - (count - 1);  // 给后面每人至少留 1 分
    return std::max<std::int64_t>(1, std::min(dist(rng_), cap));
}

PanelView RedPacketPanel::render() const {
    PanelView view;
    view.parseMode = ParseMode::kHtml;

    std::string text = "🧧 <b>" + escapeHtml(config_.blessing) + "</b>\n\n";
    text += "总金额：<b>¥" + formatMoney(config_.totalCents) + "</b>\n";

    if (remainingCount_ <= 0 || remainingCents_ <= 0) {
        text += "状态：<b>已抢完</b>（" + std::to_string(grabs_.size()) + "/" +
                std::to_string(config_.totalCount) + "）\n";
    } else if (expired_) {
        text += "状态：<b>已过期</b>，剩余 ¥" + formatMoney(remainingCents_) +
                " 未领取\n";
    } else {
        text += "剩余：<b>¥" + formatMoney(remainingCents_) + "</b>（" +
                std::to_string(grabs_.size()) + "/" +
                std::to_string(config_.totalCount) + " 已领取）\n";
    }

    if (config_.showList && !grabs_.empty()) {
        text += "\n";
        constexpr std::size_t kMaxShown = 10;
        for (std::size_t i = 0; i < grabs_.size() && i < kMaxShown; ++i) {
            text += escapeHtml(grabs_[i].name) + "  ¥" +
                    formatMoney(grabs_[i].cents) + "\n";
        }
        if (grabs_.size() > kMaxShown) {
            text += "…… 另有 " + std::to_string(grabs_.size() - kMaxShown) +
                    " 人抢到\n";
        }
    }

    view.text = std::move(text);
    if (remainingCount_ > 0 && remainingCents_ > 0 && !expired_) {
        nlohmann::json rows = nlohmann::json::array();
        rows.push_back(nlohmann::json::array(
            {{{"text", config_.grabLabel},
              {"callback_data", PanelManager::callbackData("grab")}}}));
        view.keyboard = {{"inline_keyboard", rows}};
    }
    return view;
}

bool RedPacketPanel::onClick(const PanelEvent& event, PanelEffects& effects) {
    if (event.action != "grab") {
        effects.answer = "未知操作";
        return false;
    }
    if (remainingCount_ <= 0 || remainingCents_ <= 0) {
        effects.answer = "手慢了，红包派完了";
        return false;
    }
    if (expired_) {
        effects.answer = "红包已过期";
        return false;
    }
    if (event.userId == 0) {
        effects.answer = "拿不到你的用户信息";
        return false;
    }
    if (!config_.creatorCanGrab && event.userId == config_.creatorId) {
        effects.answer = "发起人不能抢自己的红包";
        effects.showAlert = true;
        return false;
    }
    for (const auto& grab : grabs_) {
        if (grab.userId == event.userId) {
            effects.answer = "你已经抢过了：¥" + formatMoney(grab.cents);
            return false;
        }
    }

    const auto cents = split(remainingCents_, remainingCount_);
    remainingCents_ -= cents;
    --remainingCount_;
    grabs_.push_back(Grab{event.userId,
                          event.userName.empty()
                              ? "用户" + std::to_string(event.userId)
                              : event.userName,
                          cents});

    effects.answer = "抢到 ¥" + formatMoney(cents) + " 🎉";
    return true;
}

std::int64_t RedPacketPanel::grabbedCents(std::int64_t userId) const {
    for (const auto& grab : grabs_) {
        if (grab.userId == userId) {
            return grab.cents;
        }
    }
    return 0;
}

std::optional<std::chrono::system_clock::time_point>
RedPacketPanel::deadline() const {
    if (remainingCount_ <= 0 || remainingCents_ <= 0 || expired_) {
        return std::nullopt;
    }
    return deadline_;
}

void RedPacketPanel::onDeadline(PanelEffects& effects) {
    if (remainingCount_ <= 0 || remainingCents_ <= 0) {
        return;
    }
    expired_ = true;
    (void)effects;  // 过期只在原地改消息，不额外发通知
}

}  // namespace tgbot
