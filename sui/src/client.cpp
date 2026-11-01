#include <sui/client.hpp>

#include <tgbot/http_client.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <stdexcept>

namespace sui {

namespace net = boost::asio;
using nlohmann::json;

namespace {

// base58 解出来的长度可能不足 32（前导零被省略），左侧补零
std::array<std::uint8_t, 32> fixed32(const std::vector<std::uint8_t>& bytes) {
    std::array<std::uint8_t, 32> out{};
    if (bytes.size() > 32) {
        throw std::runtime_error("digest longer than 32 bytes");
    }
    std::memcpy(out.data() + (32 - bytes.size()), bytes.data(), bytes.size());
    return out;
}

std::uint64_t parseBigInt(const json& value) {
    if (value.is_null()) {
        return 0;
    }
    if (value.is_string()) {
        return std::stoull(value.get<std::string>());
    }
    return value.get<std::uint64_t>();
}

}  // namespace

SuiClient::SuiClient(std::string host, std::string path)
    : host_(std::move(host)), path_(std::move(path)) {}

net::awaitable<json> SuiClient::query(std::string_view gql, json variables) {
    json body{{"query", std::string{gql}}};
    if (!variables.is_null()) {
        body["variables"] = std::move(variables);
    }

    tgbot::HttpRequest request;
    request.method = tgbot::HttpMethod::kPost;
    request.target = path_;
    request.body = body.dump();
    request.contentType = "application/json";
    request.readTimeout = std::chrono::seconds{30};

    const auto response = co_await tgbot::httpsRequest(host_, request);
    try {
        auto parsed = json::parse(response.body);
        if (parsed.contains("errors")) {
            lastError_ = parsed["errors"].dump();
        }
        co_return parsed;
    } catch (const std::exception& e) {
        lastError_ = std::string{"bad json: "} + e.what();
        co_return json::object();
    }
}

net::awaitable<std::uint64_t> SuiClient::balance(const std::string& address) {
    static constexpr std::string_view kQuery = R"(
        query($owner: SuiAddress!) {
          address(address: $owner) {
            balance(coinType: "0x2::sui::SUI") { totalBalance }
          }
        })";
    const auto result = co_await query(kQuery, {{"owner", address}});
    if (!result.contains("data") || result["data"].is_null()) {
        co_return 0;
    }
    const auto& node = result["data"]["address"]["balance"]["totalBalance"];
    co_return parseBigInt(node);
}

net::awaitable<std::vector<CoinInfo>> SuiClient::coins(
    const std::string& address, int limit) {
    static constexpr std::string_view kQuery = R"(
        query($owner: SuiAddress!, $limit: Int) {
          address(address: $owner) {
            objects(filter: {type: "0x2::coin::Coin<0x2::sui::SUI>"}, first: $limit) {
              nodes { address version digest }
            }
          }
        })";
    const auto result = co_await query(kQuery, {{"owner", address}, {"limit", limit}});

    std::vector<CoinInfo> out;
    if (!result.contains("data") || result["data"].is_null()) {
        co_return out;
    }
    const auto& nodes = result["data"]["address"]["objects"]["nodes"];
    if (!nodes.is_array()) {
        co_return out;
    }
    for (const auto& node : nodes) {
        CoinInfo info;
        const auto idHex = node.value("address", std::string{});
        const auto idBytes = fromHex(idHex);
        info.ref.objectId = fixed32(idBytes);
        info.ref.version = node.value("version", std::uint64_t{0});
        info.ref.digest = fixed32(fromBase58(node.value("digest", std::string{})));
        out.push_back(info);
    }
    co_return out;
}

net::awaitable<ExecutionOutcome> SuiClient::executeTransaction(
    const std::string& txBcsBase64, const std::string& signatureBase64) {
    static constexpr std::string_view kMutation = R"(
        mutation($tx: Base64!, $sigs: [Base64!]!) {
          executeTransaction(transactionDataBcs: $tx, signatures: $sigs) {
            effects { status digest executionError { message } }
          }
        })";

    ExecutionOutcome outcome;
    const auto result = co_await query(
        kMutation, {{"tx", txBcsBase64}, {"sigs", json::array({signatureBase64})}});

    if (!result.contains("data") || result["data"].is_null()) {
        outcome.error = lastError_.empty() ? "no data in response" : lastError_;
        co_return outcome;
    }
    const auto& node = result["data"]["executeTransaction"];
    outcome.digest = node["effects"].value("digest", std::string{});
    const auto status = node["effects"].value("status", std::string{});
    outcome.success = status == "SUCCESS";
    if (!outcome.success) {
        const auto& err = node["effects"]["executionError"];
        outcome.error = err.is_null() ? status
                                      : err.value("message", std::string{status});
    }
    co_return outcome;
}


net::awaitable<PowChallenge> SuiClient::fetchChallenge(
    const std::string& recipient) {
    tgbot::HttpRequest request;
    request.method = tgbot::HttpMethod::kGet;
    request.target = "/v3/challenge?recipient=" + recipient;
    request.readTimeout = std::chrono::seconds{30};

    PowChallenge challenge;
    try {
        const auto response =
            co_await tgbot::httpsRequest("faucet.testnet.sui.io", request);
        const auto parsed = json::parse(response.body);
        challenge.chainId = parsed.value("chainId", std::string{});
        challenge.checkpointSeq = parsed.value("checkpointSeq", std::string{});
        challenge.checkpointDigest =
            parsed.value("checkpointDigest", std::string{});
        challenge.randomBytes = parsed.value("randomBytes", std::string{});
        challenge.faucetAddress =
            parsed.value("faucetAddress", std::string{});
        challenge.recipient = parsed.value("recipient", std::string{});
        challenge.threshold = std::stoull(parsed.value("threshold", "0"));
        challenge.amountMist = std::stoull(parsed.value("amountMist", "0"));
        challenge.memoryKiB = parsed.value("memorySize", 8192);
        challenge.iterations = parsed.value("iterations", 1);
        challenge.parallelism = parsed.value("parallelism", 1);
        challenge.domain = parsed.value("domain", std::string{"sui-faucet-pow/1"});
    } catch (const std::exception& e) {
        lastError_ = std::string{"challenge failed: "} + e.what();
    }
    co_return challenge;
}

net::awaitable<SuiClient::GasResult> SuiClient::requestGas(
    const std::string& recipient, const std::string& checkpointSeq,
    const std::string& nonce, const std::string& hashHex) {
    const json body{{"recipient", recipient},
                    {"checkpointSeq", checkpointSeq},
                    {"nonce", nonce},
                    {"hashHex", hashHex}};

    tgbot::HttpRequest request;
    request.method = tgbot::HttpMethod::kPost;
    request.target = "/v3/gas";
    request.body = body.dump();
    request.contentType = "application/json";
    request.readTimeout = std::chrono::seconds{60};

    GasResult result;
    try {
        const auto response =
            co_await tgbot::httpsRequest("faucet.testnet.sui.io", request);
        const auto parsed = json::parse(response.body);
        result.success = parsed.value("status", std::string{}) == "success";
        result.digest = parsed.value("digest", std::string{});
        result.code = parsed.value("code", std::string{});
        result.error = parsed.value("error", std::string{});
        if (!result.success && result.error.empty()) {
            result.error = response.body;
        }
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    co_return result;
}


net::awaitable<SuiClient::ChainInfo> SuiClient::chainInfo() {
    static constexpr std::string_view kQuery = R"(
        query { chainIdentifier epoch { epochId } })";
    const auto result = co_await query(kQuery, json::object());
    ChainInfo info;
    if (result.contains("data") && !result["data"].is_null()) {
        info.chainIdentifier =
            result["data"].value("chainIdentifier", std::string{});
        info.epoch = result["data"]["epoch"].value("epochId", std::uint64_t{0});
    }
    co_return info;
}

}  // namespace sui
