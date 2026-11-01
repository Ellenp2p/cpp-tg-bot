#include <sui/crypto.hpp>

#include <openssl/evp.h>

#include <cstring>
#include <cstring>
#include <stdexcept>

namespace sui {

namespace {

constexpr std::uint64_t kIv[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
    0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};

constexpr std::uint8_t kSigma[12][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
};

inline std::uint64_t rotr(std::uint64_t value, int bits) {
    return (value >> bits) | (value << (64 - bits));
}

void compress(std::uint64_t h[8], const std::uint8_t block[128],
              std::uint64_t counter, bool last) {
    std::uint64_t m[16];
    for (int i = 0; i < 16; ++i) {
        std::uint64_t value = 0;
        for (int j = 7; j >= 0; --j) {
            value = (value << 8) | block[static_cast<std::size_t>(i) * 8 + j];
        }
        m[i] = value;
    }

    std::uint64_t v[16];
    for (int i = 0; i < 8; ++i) {
        v[i] = h[i];
        v[i + 8] = kIv[i];
    }
    v[12] ^= counter;
    if (last) {
        v[14] = ~v[14];
    }

    const auto mix = [&v](int a, int b, int c, int d, std::uint64_t x,
                          std::uint64_t y) {
        v[a] = v[a] + v[b] + x;
        v[d] = rotr(v[d] ^ v[a], 32);
        v[c] = v[c] + v[d];
        v[b] = rotr(v[b] ^ v[c], 24);
        v[a] = v[a] + v[b] + y;
        v[d] = rotr(v[d] ^ v[a], 16);
        v[c] = v[c] + v[d];
        v[b] = rotr(v[b] ^ v[c], 63);
    };

    for (int round = 0; round < 12; ++round) {
        const auto* s = kSigma[round];
        mix(0, 4, 8, 12, m[s[0]], m[s[1]]);
        mix(1, 5, 9, 13, m[s[2]], m[s[3]]);
        mix(2, 6, 10, 14, m[s[4]], m[s[5]]);
        mix(3, 7, 11, 15, m[s[6]], m[s[7]]);
        mix(0, 5, 10, 15, m[s[8]], m[s[9]]);
        mix(1, 6, 11, 12, m[s[10]], m[s[11]]);
        mix(2, 7, 8, 13, m[s[12]], m[s[13]]);
        mix(3, 4, 9, 14, m[s[14]], m[s[15]]);
    }

    for (int i = 0; i < 8; ++i) {
        h[i] ^= v[i] ^ v[i + 8];
    }
}

}  // namespace

std::vector<std::uint8_t> blake2b(std::size_t outLen,
                                  std::span<const std::uint8_t> data) {
    if (outLen == 0 || outLen > 64) {
        throw std::invalid_argument("blake2b output length must be 1..64");
    }
    std::uint64_t h[8];
    for (int i = 0; i < 8; ++i) {
        h[i] = kIv[i];
    }
    // 参数块：digest_length | key_length<<8 | fanout<<16 | depth<<24
    h[0] ^= 0x01010000ULL ^ static_cast<std::uint64_t>(outLen);

    std::uint64_t counter = 0;
    std::size_t offset = 0;
    std::uint8_t block[128];

    while (data.size() - offset > 128) {
        std::memcpy(block, data.data() + offset, 128);
        offset += 128;
        counter += 128;
        compress(h, block, counter, false);
    }

    const std::size_t remaining = data.size() - offset;
    std::memset(block, 0, sizeof(block));
    if (remaining > 0) {
        std::memcpy(block, data.data() + offset, remaining);
    }
    counter += remaining;
    compress(h, block, counter, true);

    std::vector<std::uint8_t> out(outLen);
    for (std::size_t i = 0; i < outLen; ++i) {
        out[i] = static_cast<std::uint8_t>(h[i / 8] >> (8 * (i % 8)));
    }
    return out;
}

std::array<std::uint8_t, 32> blake2b256(std::span<const std::uint8_t> data) {
    const auto out = blake2b(32, data);
    std::array<std::uint8_t, 32> result{};
    std::memcpy(result.data(), out.data(), 32);
    return result;
}

std::string toHex(std::span<const std::uint8_t> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

std::vector<std::uint8_t> fromHex(std::string_view hex) {
    if (hex.rfind("0x", 0) == 0) {
        hex.remove_prefix(2);
    }
    if (hex.size() % 2 != 0) {
        throw std::invalid_argument("hex string must have even length");
    }
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const auto hi = static_cast<std::uint8_t>(hex[i]);
        const auto lo = static_cast<std::uint8_t>(hex[i + 1]);
        const auto digit = [](std::uint8_t c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            throw std::invalid_argument("invalid hex digit");
        };
        out.push_back(static_cast<std::uint8_t>(digit(hi) * 16 + digit(lo)));
    }
    return out;
}

namespace {
constexpr char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string toBase64(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t b0 = bytes[i];
        const std::uint32_t b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const std::uint32_t b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kBase64[(triple >> 18) & 0x3F]);
        out.push_back(kBase64[(triple >> 12) & 0x3F]);
        out.push_back(i + 1 < bytes.size() ? kBase64[(triple >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < bytes.size() ? kBase64[triple & 0x3F] : '=');
    }
    return out;
}

std::vector<std::uint8_t> fromBase64(std::string_view text) {
    std::vector<std::uint8_t> out;
    int accumulator = 0;
    int bits = -8;
    for (const char c : text) {
        if (c == '=') break;
        int value = -1;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '+') value = 62;
        else if (c == '/') value = 63;
        else continue;
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

std::vector<std::uint8_t> fromBase58(std::string_view text) {
    static constexpr char kAlphabet[] =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    std::vector<std::uint8_t> accumulator;  // 低位在前
    for (const char c : text) {
        const auto* pos = std::strchr(kAlphabet, c);
        if (pos == nullptr) {
            throw std::invalid_argument("invalid base58 character");
        }
        int carry = static_cast<int>(pos - kAlphabet);
        for (auto& byte : accumulator) {
            const int value = static_cast<int>(byte) * 58 + carry;
            byte = static_cast<std::uint8_t>(value & 0xFF);
            carry = value >> 8;
        }
        while (carry > 0) {
            accumulator.push_back(static_cast<std::uint8_t>(carry & 0xFF));
            carry >>= 8;
        }
    }
    std::size_t leadingZeros = 0;
    while (leadingZeros < text.size() && text[leadingZeros] == '1') {
        ++leadingZeros;
    }
    std::vector<std::uint8_t> out(leadingZeros, 0x00);
    out.insert(out.end(), accumulator.rbegin(), accumulator.rend());
    return out;
}

struct Keypair::Impl {
    EVP_PKEY* pkey = nullptr;
    ~Impl() {
        if (pkey != nullptr) {
            EVP_PKEY_free(pkey);
        }
    }
};

Keypair::Keypair() = default;

Keypair::~Keypair() { delete impl_; }

Keypair::Keypair(Keypair&& other) noexcept
    : impl_(other.impl_),
      publicKey_(other.publicKey_),
      address_(other.address_) {
    other.impl_ = nullptr;
}

Keypair& Keypair::operator=(Keypair&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        publicKey_ = other.publicKey_;
        address_ = other.address_;
        other.impl_ = nullptr;
    }
    return *this;
}

Keypair Keypair::fromSeed(std::span<const std::uint8_t, 32> seed) {
    Keypair kp;
    kp.impl_ = new Impl();
    kp.impl_->pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, seed.data(), seed.size());
    if (kp.impl_->pkey == nullptr) {
        throw std::runtime_error("failed to create ed25519 key");
    }
    std::size_t pubLen = kp.publicKey_.size();
    if (EVP_PKEY_get_raw_public_key(kp.impl_->pkey, kp.publicKey_.data(),
                                    &pubLen) != 1 ||
        pubLen != 32) {
        throw std::runtime_error("failed to derive ed25519 public key");
    }
    // Sui 地址 = blake2b256(0x00 ‖ pubkey)
    std::array<std::uint8_t, 33> flagged{};
    flagged[0] = 0x00;  // ed25519
    std::memcpy(flagged.data() + 1, kp.publicKey_.data(), 32);
    kp.address_ = blake2b256(flagged);
    return kp;
}

Keypair Keypair::fromSeedHex(std::string_view hex) {
    if (hex.rfind("0x", 0) == 0) {
        hex.remove_prefix(2);
    }
    const auto decoded = fromHex(hex);
    if (decoded.size() != 32) {
        throw std::invalid_argument("seed must be 32 bytes");
    }
    std::array<std::uint8_t, 32> seed{};
    std::memcpy(seed.data(), decoded.data(), 32);
    return fromSeed(seed);
}

std::string Keypair::addressHex() const { return "0x" + toHex(address_); }

std::array<std::uint8_t, 64> Keypair::sign(
    std::span<const std::uint8_t> message) const {
    std::array<std::uint8_t, 64> signature{};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        throw std::runtime_error("EVP_MD_CTX_new failed");
    }
    std::size_t sigLen = signature.size();
    const bool ok =
        EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, impl_->pkey) == 1 &&
        EVP_DigestSign(ctx, signature.data(), &sigLen, message.data(),
                       message.size()) == 1 &&
        sigLen == 64;
    EVP_MD_CTX_free(ctx);
    if (!ok) {
        throw std::runtime_error("ed25519 sign failed");
    }
    return signature;
}

bool Keypair::verify(std::span<const std::uint8_t> message,
                     std::span<const std::uint8_t, 64> signature) const {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        return false;
    }
    const bool ok =
        EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, impl_->pkey) == 1 &&
        EVP_DigestVerify(ctx, signature.data(), signature.size(),
                         message.data(), message.size()) == 1;
    EVP_MD_CTX_free(ctx);
    return ok;
}

std::vector<std::uint8_t> Keypair::signTransaction(
    std::span<const std::uint8_t> txBytes) const {
    // intent = (scope=TransactionData(0), version=0, app=Sui(0))
    std::vector<std::uint8_t> message{0x00, 0x00, 0x00};
    message.insert(message.end(), txBytes.begin(), txBytes.end());

    const auto digest = blake2b256(message);
    const auto signature = sign(digest);

    std::vector<std::uint8_t> out;
    out.reserve(1 + 64 + 32);
    out.push_back(0x00);  // ed25519 方案标记
    out.insert(out.end(), signature.begin(), signature.end());
    out.insert(out.end(), publicKey_.begin(), publicKey_.end());
    return out;
}

}  // namespace sui
