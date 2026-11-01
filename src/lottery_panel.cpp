#include <tgbot/lottery_panel.hpp>

#include <tgbot/rich_text.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace tgbot {

namespace {

std::string formatRemaining(std::chrono::seconds left) {
    if (left.count() <= 0) {
        return "即将开奖";
    }
    const auto minutes = left.count() / 60;
    const auto seconds = left.count() % 60;
    if (minutes <= 0) {
        return std::to_string(seconds) + " 秒";
    }
    return std::to_string(minutes) + " 分 " + std::to_string(seconds) + " 秒";
}

}  // namespace

LotteryPanel::LotteryPanel(Config config)
    : config_(std::move(config)),
      deadline_(std::chrono::system_clock::now() + config_.duration) {}

std::chrono::seconds LotteryPanel::remaining() const {
    const auto left = deadline_ - std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(left);
}

PanelView LotteryPanel::render() const {
    PanelView view;
    view.parseMode = ParseMode::kHtml;

    std::string text = "🎁 <b>" + escapeHtml(config_.title) + "</b>\n";
    text += "奖品：" + escapeHtml(config_.prize) + " × " +
            std::to_string(std::max(1, config_.winnerCount)) + "\n";
    text += "参与：" + std::to_string(participants_.size()) + " 人\n";

    if (closed_) {
        text += "\n状态：<b>已开奖</b>\n";
        if (winners_.empty()) {
            text += "参与人数不足，本轮无人中奖。";
        } else {
            text += "中奖者：";
            for (std::size_t i = 0; i < winners_.size(); ++i) {
                if (i > 0) {
                    text += "、";
                }
                const auto it = participants_.find(winners_[i]);
                text += it != participants_.end() ? escapeHtml(it->second)
                                                  : std::to_string(winners_[i]);
            }
            if (config_.rewardPrivate) {
                text += "\n中奖者请看只有自己能收到的那条消息 👀";
            }
        }
    } else {
        text += "剩余：" + formatRemaining(remaining()) + "\n";
        text += "\n点下面的按钮参与，到点自动开奖。";
    }
    view.text = std::move(text);

    if (!closed_) {
        nlohmann::json rows = nlohmann::json::array();
        rows.push_back(nlohmann::json::array(
            {{{"text", config_.joinLabel},
              {"callback_data", PanelManager::callbackData("join")}}}));
        rows.push_back(nlohmann::json::array(
            {{{"text", config_.drawLabel},
              {"callback_data", PanelManager::callbackData("draw")}}}));
        view.keyboard = {{"inline_keyboard", rows}};
    }
    return view;
}

bool LotteryPanel::onClick(const PanelEvent& event, PanelEffects& effects) {
    if (event.action == "join") {
        if (closed_) {
            effects.answer = "已经开奖了";
            return false;
        }
        if (event.userId == 0) {
            effects.answer = "拿不到你的用户信息";
            return false;
        }
        if (participants_.count(event.userId) != 0) {
            effects.answer = "你已经在名单里了";
            return false;
        }
        participants_[event.userId] =
            event.userName.empty() ? "用户" + std::to_string(event.userId)
                                   : event.userName;
        effects.answer =
            "参加成功 🎉 你是第 " + std::to_string(participants_.size()) + " 位";
        return true;
    }

    if (event.action == "draw") {
        if (config_.creatorId != 0 && event.userId != config_.creatorId) {
            effects.answer = "只有发起者可以开奖";
            effects.showAlert = true;
            return false;
        }
        if (closed_) {
            effects.answer = "已经开过奖了";
            return false;
        }
        if (participants_.empty()) {
            effects.answer = "还没有人参与";
            return false;
        }
        draw(effects);
        return true;
    }

    effects.answer = "未知操作";
    return false;
}

std::optional<std::chrono::system_clock::time_point> LotteryPanel::deadline()
    const {
    if (closed_) {
        return std::nullopt;
    }
    return deadline_;
}

void LotteryPanel::onDeadline(PanelEffects& effects) {
    if (closed_) {
        return;
    }
    if (participants_.empty()) {
        closed_ = true;
        effects.publicMessages.push_back("⏰ 抽奖结束：没有人参与。");
        return;
    }
    draw(effects);
}

void LotteryPanel::draw(PanelEffects& effects) {
    closed_ = true;

    std::vector<std::int64_t> ids;
    ids.reserve(participants_.size());
    for (const auto& [id, name] : participants_) {
        (void)name;
        ids.push_back(id);
    }
    std::shuffle(ids.begin(), ids.end(), rng_);

    const auto count = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(1, config_.winnerCount)), ids.size());
    winners_.assign(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(count));

    if (!config_.rewardPrivate) {
        return;  // 默认只在群里公布名单，不私下发码
    }
    for (const auto id : winners_) {
        PanelEffects::Private pm;
        pm.userId = id;
        // 私密消息走纯文本：sendEphemeralMessage 目前不带 parse_mode，
        // 塞 HTML 标签只会原样显示出来
        pm.text = "🎉 恭喜中奖！\n奖品：" + config_.prize +
                  "\n你的专属码：" + rewardFor(id) +
                  "\n（这条消息只有你能看到，别转给别人）";
        effects.privateMessages.push_back(std::move(pm));
    }
}

std::string LotteryPanel::rewardFor(std::int64_t userId) const {
    if (config_.rewardCode) {
        return config_.rewardCode(userId);
    }
    std::uniform_int_distribution<int> dist(100000, 999999);
    (void)userId;
    return "GIFT-" + std::to_string(dist(rng_));
}

}  // namespace tgbot
