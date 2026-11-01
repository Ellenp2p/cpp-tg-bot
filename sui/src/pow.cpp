#include <sui/pow.hpp>

#include <sui/crypto.hpp>

#include <argon2.h>

#include <cstring>
#include <stdexcept>

namespace sui {

std::string buildPowPreimage(const PowChallenge& challenge,
                             std::string_view nonceDecimal) {
    // 规范第 2 节：八行，'\n' 连接，末尾没有换行。
    // randomBytes 为空时第 5 行就是空行，字段数仍是八个。
    std::string preimage;
    preimage.reserve(256);
    preimage += challenge.domain;
    preimage += '\n';
    preimage += challenge.chainId;
    preimage += '\n';
    preimage += challenge.checkpointSeq;
    preimage += '\n';
    preimage += challenge.checkpointDigest;
    preimage += '\n';
    preimage += challenge.randomBytes;
    preimage += '\n';
    preimage += challenge.faucetAddress;
    preimage += '\n';
    preimage += challenge.recipient;
    preimage += '\n';
    preimage += nonceDecimal;
    return preimage;
}

std::vector<std::uint8_t> argon2d32(std::string_view password,
                                    std::string_view salt, int memoryKiB,
                                    int iterations, int parallelism) {
    std::vector<std::uint8_t> out(32);
    const int rc = argon2d_hash_raw(
        static_cast<std::uint32_t>(iterations),
        static_cast<std::uint32_t>(memoryKiB),
        static_cast<std::uint32_t>(parallelism), password.data(),
        password.size(), salt.data(), salt.size(), out.data(), out.size());
    if (rc != ARGON2_OK) {
        throw std::runtime_error(std::string{"argon2d failed: "} +
                                 argon2_error_message(rc));
    }
    return out;
}

std::optional<PowSolution> solvePow(const PowChallenge& challenge,
                                    std::uint64_t maxAttempts) {
    const std::string salt = "sui-faucet-pow-1";
    for (std::uint64_t nonce = 0; nonce < maxAttempts; ++nonce) {
        const std::string nonceText = std::to_string(nonce);
        const std::string preimage = buildPowPreimage(challenge, nonceText);
        const auto hash = argon2d32(preimage, salt, challenge.memoryKiB,
                                    challenge.iterations, challenge.parallelism);

        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v = (v << 8) | hash[static_cast<std::size_t>(i)];
        }
        if (v < challenge.threshold) {
            PowSolution solution;
            solution.nonce = nonceText;
            solution.hashHex = toHex(hash);
            solution.v = v;
            return solution;
        }
    }
    return std::nullopt;
}

}  // namespace sui
