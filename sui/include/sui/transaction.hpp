#pragma once

#include <sui/crypto.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sui {

// 一个链上对象引用（交易里引用 gas coin 时必须给全）
struct ObjectRef {
    std::array<std::uint8_t, 32> objectId{};
    std::uint64_t version = 0;
    std::array<std::uint8_t, 32> digest{};
};

// 用 gas coin 直接支付给某个地址：SplitCoins(Gas, [amount]) + TransferObjects
// 参考 PTB：
//   input 0 = Pure(amount u64 LE)
//   input 1 = Pure(recipient)
//   cmd 0   = SplitCoins(GasCoin, [Input(0)])   → Result(0)
//   cmd 1   = TransferObjects([Result(0)], Input(1))
struct TransferRequest {
    std::array<std::uint8_t, 32> sender{};
    std::array<std::uint8_t, 32> recipient{};
    std::uint64_t amountMist = 0;
    std::vector<ObjectRef> gasCoins;
    std::uint64_t gasPrice = 1000;         // MIST per gas unit
    std::uint64_t gasBudget = 10'000'000;  // 0.01 SUI

    // 过期策略。
    // 当交易没有任何"地址拥有的输入"（例如 gas 全从 address balance 出）时，
    // Sui 要求用 ValidDuring 而不是 None，否则会报：
    //   "Transactions must either have address-owned inputs, or a ValidDuring
    //    expiration with at most two epochs of validity"
    std::array<std::uint8_t, 32> chain{};  // genesis checkpoint digest（32 字节）
    std::uint32_t expirationNonce = 0;
    std::uint64_t minEpoch = 0;
    std::uint64_t maxEpoch = 0;
    bool useValidDuring = true;
};

// 返回 TransactionData 的 BCS 字节（不含 intent 前缀）
std::vector<std::uint8_t> buildTransferTransaction(const TransferRequest& request);

}  // namespace sui
