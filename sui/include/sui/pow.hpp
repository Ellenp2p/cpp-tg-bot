#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sui {

// 水龙头（faucet）的工作量证明，规范见 https://faucet.sui.io/pow-spec/
//
// 为什么要做这个：Sui 官方把 faucet 的限速换成了 PoW —— 每个请求必须
// 用 Argon2d 找到一个满足阈值的 nonce，否则拿不到测试币。
struct PowChallenge {
    std::string domain = "sui-faucet-pow/1";
    std::string chainId;
    std::string checkpointSeq;      // 十进制字符串，规范要求不要转成数字再转回来
    std::string checkpointDigest;   // base58
    std::string randomBytes;        // base64；无随机性时是空串（那一行为空）
    std::string faucetAddress;
    std::string recipient;
    std::uint64_t threshold = 0;    // floor(2^64 / difficulty)
    std::uint64_t amountMist = 0;
    int memoryKiB = 8192;
    int iterations = 1;
    int parallelism = 1;
};

// 八行、用 '\n' 连接、末尾无换行
std::string buildPowPreimage(const PowChallenge& challenge,
                             std::string_view nonceDecimal);

// Argon2d（version 0x13），输出 32 字节
std::vector<std::uint8_t> argon2d32(std::string_view password,
                                    std::string_view salt, int memoryKiB,
                                    int iterations, int parallelism);

struct PowSolution {
    std::string nonce;    // 十进制字符串
    std::string hashHex;  // 64 位小写 hex
    std::uint64_t v = 0;  // hash 前 8 字节按大端解释
};

// 从 nonce=0 开始暴力找满足 v < threshold 的解
std::optional<PowSolution> solvePow(const PowChallenge& challenge,
                                    std::uint64_t maxAttempts = 5'000'000);

}  // namespace sui
