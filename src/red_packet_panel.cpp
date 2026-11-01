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

RedPacketPanel::RedPacketPanel(std::shared_ptr<RedPacketLedger> ledger,
                               std::int64_t packetId, Options options)
    : ledger_(std::move(ledger)),
      packetId_(packetId),
      options_(std::move(options)),
      rng_(options_.seed != 0 ? options_.seed : std::random_device{}()) {}

std::int64_t RedPacketPanel::split(std::int64_t remaining, int count) {
    if (count <= 1) {
        return remaining;  // 最后一份全给
    }
    const auto maxPart = std::max<std::int64_t>(1, remaining * 2 / count);
    std::uniform_int_distribution<std::int64_t> dist(1, maxPart);
    const auto cap = remaining - (count - 1);  // 给后面每人至少留 1 分
    return std::max<std::int64_t>(1, std::min(dist(rng_), cap));
}

PanelView RedPacketPanel::render() const {
    PanelView view;
    view.parseMode = ParseMode::kHtml;

    const auto row = ledger_ ? ledger_->get(packetId_) : std::nullopt;
    if (!row.has_value()) {
        view.text = "这个红包不存在或已被删除。";
        return view;
    }

    const auto grabs = ledger_->grabs(packetId_);

    std::string text = "🧧 <b>" + escapeHtml(options_.blessing) + "</b>\n\n";
    text += "总金额：<b>¥" + formatMoney(row->totalCents) + "</b>\n";

    if (row->state == "exhausted") {
        text += "状态：<b>已抢完</b>（" + std::to_string(grabs.size()) + "/" +
                std::to_string(row->totalCount) + "）\n";
    } else if (row->state == "expired") {
        text += "状态：<b>已过期</b>，剩余 ¥" +
                formatMoney(row->remainingCents) + " 未领取\n";
    } else if (row->state == "refunding" || row->state == "refunded") {
        text += "状态：<b>已过期</b>，剩余 ¥" +
                formatMoney(row->remainingCents) + " 已退回发起人\n";
    } else {
        text += "剩余：<b>¥" + formatMoney(row->remainingCents) + "</b>（" +
                std::to_string(grabs.size()) + "/" +
                std::to_string(row->totalCount) + " 已领取）\n";
    }

    if (options_.showList && !grabs.empty()) {
        text += "\n";
        constexpr std::size_t kMaxShown = 10;
        for (std::size_t i = 0; i < grabs.size() && i < kMaxShown; ++i) {
            const auto& grab = grabs[i];
            text += escapeHtml(grab.userName.empty()
                                   ? "用户" + std::to_string(grab.userId)
                                   : grab.userName) +
                    "  ¥" + formatMoney(grab.cents) + "\n";
        }
        if (grabs.size() > kMaxShown) {
            text += "…… 另有 " + std::to_string(grabs.size() - kMaxShown) +
                    " 人抢到\n";
        }
    }

    view.text = std::move(text);
    if (row->state == "open" && row->remainingCents > 0) {
        nlohmann::json rows = nlohmann::json::array();
        rows.push_back(nlohmann::json::array(
            {{{"text", options_.grabLabel},
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
    const auto row = ledger_ ? ledger_->get(packetId_) : std::nullopt;
    if (!row.has_value()) {
        effects.answer = "红包不存在";
        return false;
    }
    if (row->state != "open") {
        effects.answer = "手慢了，红包派完了";
        return false;
    }
    if (event.userId == 0) {
        effects.answer = "拿不到你的用户信息";
        return false;
    }
    if (!options_.creatorCanGrab && event.userId == row->creatorId) {
        effects.answer = "发起人不能抢自己的红包";
        effects.showAlert = true;
        return false;
    }

    const auto grabs = ledger_->grabs(packetId_);
    for (const auto& grab : grabs) {
        if (grab.userId == event.userId) {
            effects.answer = "你已经抢过了：¥" + formatMoney(grab.cents);
            return false;
        }
    }

    const auto cents = split(row->remainingCents, row->remainingCount);
    const auto name = event.userName.empty()
                          ? "用户" + std::to_string(event.userId)
                          : event.userName;
    if (!ledger_->grab(packetId_, event.userId, name, cents)) {
        effects.answer = "手慢了，红包派完了";
        return false;
    }

    effects.answer = "抢到 ¥" + formatMoney(cents) + " 🎉";
    return true;
}

std::optional<std::chrono::system_clock::time_point>
RedPacketPanel::deadline() const {
    const auto row = ledger_ ? ledger_->get(packetId_) : std::nullopt;
    if (!row.has_value() || row->state != "open") {
        return std::nullopt;
    }
    return std::chrono::system_clock::time_point{
        std::chrono::seconds{row->expiresAt}};
}

void RedPacketPanel::onDeadline(PanelEffects& /*effects*/) {
    if (ledger_) {
        ledger_->markExpired(packetId_);
    }
}

bool RedPacketPanel::finished() const {
    const auto row = ledger_ ? ledger_->get(packetId_) : std::nullopt;
    return !row.has_value() || row->state != "open";
}

}  // namespace tgbot
