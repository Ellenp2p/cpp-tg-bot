#pragma once

#include <sui/crypto.hpp>
#include <sui/pow.hpp>
#include <sui/transaction.hpp>

#include <boost/asio/awaitable.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sui {

struct CoinInfo {
    ObjectRef ref;
    std::uint64_t balance = 0;
};

struct ExecutionOutcome {
    std::string digest;
    bool success = false;
    std::string error;
};

// Sui GraphQL 客户端。
// 为什么不用 JSON-RPC：Sui 公共全节点已弃用 JSON-RPC（返回 -32601），
// 官方要求迁移到 gRPC 或 GraphQL。gRPC 要 HTTP/2，而我们的 HTTP 栈是
// HTTP/1.1（Beast），所以走 GraphQL —— 纯 HTTPS POST，最省事。
class SuiClient {
public:
    explicit SuiClient(std::string host = "graphql.testnet.sui.io",
                       std::string path = "/graphql");

    boost::asio::awaitable<nlohmann::json> query(std::string_view gql,
                                                 nlohmann::json variables);

    // 地址余额（MIST）
    boost::asio::awaitable<std::uint64_t> balance(const std::string& address);

    // 该地址持有的 SUI coin（交易里当 gas 用）
    boost::asio::awaitable<std::vector<CoinInfo>> coins(
        const std::string& address, int limit = 5);

    // 提交交易：txBcs 是 TransactionData 的 base64，signature 是签名 base64
    boost::asio::awaitable<ExecutionOutcome> executeTransaction(
        const std::string& txBcsBase64, const std::string& signatureBase64);

    // ---- faucet（v3：先取 challenge，再做 Argon2d PoW，最后提交）----
    boost::asio::awaitable<PowChallenge> fetchChallenge(
        const std::string& recipient);

    struct GasResult {
        bool success = false;
        std::string digest;
        std::string code;
        std::string error;
    };
    boost::asio::awaitable<GasResult> requestGas(
        const std::string& recipient, const std::string& checkpointSeq,
        const std::string& nonce, const std::string& hashHex);

    struct ChainInfo {
        std::string chainIdentifier;  // base58
        std::uint64_t epoch = 0;
    };
    boost::asio::awaitable<ChainInfo> chainInfo();

    const std::string& lastError() const { return lastError_; }

private:
    std::string host_;
    std::string path_;
    std::string lastError_;
};

}  // namespace sui
