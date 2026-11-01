#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sui {

// BLAKE2b，输出长度可变（Sui 用的是 32 字节版本）。
// 注意：BLAKE2b-256 不是 BLAKE2b-512 的截断，输出长度参与初始状态，
// 所以不能用 OpenSSL 的 blake2b512 截断来替代。
std::vector<std::uint8_t> blake2b(std::size_t outLen,
                                  std::span<const std::uint8_t> data);
std::array<std::uint8_t, 32> blake2b256(std::span<const std::uint8_t> data);

std::string toHex(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> fromHex(std::string_view hex);
std::string toBase64(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> fromBase64(std::string_view text);

// Sui 的 object digest 用 base58 表示（不是 hex）
std::vector<std::uint8_t> fromBase58(std::string_view text);

// Sui 的 ed25519 账户：32 字节种子 → 公钥 → 地址
class Keypair {
public:
    static Keypair fromSeed(std::span<const std::uint8_t, 32> seed);
    // 从 32 字节私钥的十六进制字符串
    static Keypair fromSeedHex(std::string_view hex);

    const std::array<std::uint8_t, 32>& publicKey() const { return publicKey_; }
    // Sui 地址 = blake2b256(0x00 ‖ pubkey)，0x00 是 ed25519 的签名方案标记
    const std::array<std::uint8_t, 32>& address() const { return address_; }
    std::string addressHex() const;  // "0x" + 64 hex

    // 对任意消息做 ed25519 签名（64 字节）
    std::array<std::uint8_t, 64> sign(std::span<const std::uint8_t> message) const;
    bool verify(std::span<const std::uint8_t> message,
                std::span<const std::uint8_t, 64> signature) const;

    // Sui 交易签名：intent(0x00,0x00,0x00) ‖ txBytes → blake2b256 → ed25519
    // 返回 "0x00 ‖ sig ‖ pubkey"，可直接 base64 后提交给节点
    std::vector<std::uint8_t> signTransaction(
        std::span<const std::uint8_t> txBytes) const;

    ~Keypair();
    Keypair(Keypair&& other) noexcept;
    Keypair& operator=(Keypair&& other) noexcept;
    Keypair(const Keypair&) = delete;
    Keypair& operator=(const Keypair&) = delete;

private:
    Keypair();
    struct Impl;
    Impl* impl_ = nullptr;
    std::array<std::uint8_t, 32> publicKey_{};
    std::array<std::uint8_t, 32> address_{};
};

}  // namespace sui
