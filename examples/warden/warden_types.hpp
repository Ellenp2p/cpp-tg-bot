#pragma once
#include <boost/asio/awaitable.hpp>
#include <nlohmann/json.hpp>
#include <tgbot/tg_bot.hpp>
namespace warden {
using nlohmann::json;
namespace net = boost::asio;
using tgbot::BotClient;
} // namespace warden
