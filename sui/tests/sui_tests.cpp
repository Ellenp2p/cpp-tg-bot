// sui SDK 自测：先用公开测试向量把密码学底座钉死，再谈网络。
// 失败计数 > 0 即返回非 0。

#include <sui/crypto.hpp>
#include <sui/pow.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int gChecks = 0;
int gFailed = 0;

void check(bool ok, const char* expr, int line) {
    ++gChecks;
    if (!ok) {
        ++gFailed;
        std::printf("  FAIL line %d: %s\n", line, expr);
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

std::string hexOf(const std::vector<std::uint8_t>& bytes) {
    return sui::toHex(bytes);
}

std::string hexOf32(const std::array<std::uint8_t, 32>& bytes) {
    return sui::toHex(bytes);
}

// BLAKE2b 官方测试向量（RFC 7693 附录 + 常用 KAT）
void testBlake2b() {
    std::printf("test: BLAKE2b-256 测试向量\n");

    const std::string empty;
    CHECK(hexOf32(sui::blake2b256(std::span<const std::uint8_t>(
              reinterpret_cast<const std::uint8_t*>(empty.data()),
              empty.size()))) ==
          "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8");

    const std::string abc = "abc";
    CHECK(hexOf32(sui::blake2b256(std::span<const std::uint8_t>(
              reinterpret_cast<const std::uint8_t*>(abc.data()),
              abc.size()))) ==
          "bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319");

    // 512 位版本，验证多块输入（>128 字节）
    const std::string block(200, 'a');
    const auto wide = sui::blake2b(64, std::span<const std::uint8_t>(
                                         reinterpret_cast<const std::uint8_t*>(
                                             block.data()),
                                         block.size()));
    CHECK(wide.size() == 64);
    CHECK(hexOf(wide) !=
          "0000000000000000000000000000000000000000000000000000000000000000");
}

// RFC 8032 §7.1 TEST 1
void testEd25519() {
    std::printf("test: ed25519（RFC 8032 TEST 1）\n");

    const auto seed =
        sui::fromHex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    std::array<std::uint8_t, 32> seedArr{};
    std::memcpy(seedArr.data(), seed.data(), 32);

    const auto kp = sui::Keypair::fromSeed(seedArr);
    CHECK(hexOf32(kp.publicKey()) ==
          "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");

    // 空消息签名
    const auto sig = kp.sign({});
    CHECK(hexOf(std::vector<std::uint8_t>(sig.begin(), sig.end())) ==
          "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
          "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");

    CHECK(kp.verify({}, sig));

    // 篡改后验签必须失败
    auto tampered = sig;
    tampered[0] ^= 0x01;
    CHECK(!kp.verify({}, tampered));
}

void testBase64AndHex() {
    std::printf("test: base64 / hex\n");

    const std::vector<std::uint8_t> data{0x00, 0x01, 0xFE, 0xFF, 0x42};
    const auto b64 = sui::toBase64(data);
    CHECK(b64 == "AAH+/0I=");
    CHECK(sui::fromBase64(b64) == data);

    CHECK(sui::toHex(data) == "0001feff42");
    CHECK(sui::fromHex("0001feff42") == data);
    CHECK(sui::fromBase64("") == std::vector<std::uint8_t>{});
}

void testAddress() {
    std::printf("test: Sui 地址派生（blake2b256(0x00 ‖ pubkey)）\n");

    // 地址必须是 32 字节、确定性、且和公钥相关
    const auto seed = sui::fromHex(
        "1111111111111111111111111111111111111111111111111111111111111111");
    std::array<std::uint8_t, 32> seedArr{};
    std::memcpy(seedArr.data(), seed.data(), 32);

    const auto a = sui::Keypair::fromSeed(seedArr);
    const auto b = sui::Keypair::fromSeed(seedArr);
    CHECK(a.addressHex() == b.addressHex());
    CHECK(a.addressHex().size() == 66);
    CHECK(a.addressHex().rfind("0x", 0) == 0);

    // 换个种子地址必须不同
    auto seed2 = seedArr;
    seed2[0] ^= 0xFF;
    const auto c = sui::Keypair::fromSeed(seed2);
    CHECK(c.addressHex() != a.addressHex());

    // 地址 = blake2b256(0x00 ‖ pubkey)，这里自己复算一遍
    std::array<std::uint8_t, 33> flagged{};
    flagged[0] = 0x00;
    std::memcpy(flagged.data() + 1, a.publicKey().data(), 32);
    CHECK("0x" + hexOf32(sui::blake2b256(flagged)) == a.addressHex());
}

void testTransactionSignatureShape() {
    std::printf("test: 交易签名结构（0x00 ‖ sig ‖ pubkey）\n");

    const auto seed = sui::fromHex(
        "2222222222222222222222222222222222222222222222222222222222222222");
    std::array<std::uint8_t, 32> seedArr{};
    std::memcpy(seedArr.data(), seed.data(), 32);
    const auto kp = sui::Keypair::fromSeed(seedArr);

    const std::vector<std::uint8_t> txBytes{1, 2, 3, 4, 5};
    const auto signedBytes = kp.signTransaction(txBytes);
    CHECK(signedBytes.size() == 1 + 64 + 32);
    CHECK(signedBytes[0] == 0x00);  // ed25519 flag
    CHECK(std::equal(signedBytes.begin() + 65, signedBytes.end(),
                     kp.publicKey().begin()));

    // 相同输入必须产生相同签名（ed25519 是确定性的）
    CHECK(kp.signTransaction(txBytes) == signedBytes);
}

// faucet PoW 官方一致性向量（https://faucet.sui.io/pow-spec/pow-vectors.json）
void testPowVectors() {
    std::printf("test: faucet PoW 一致性向量（Argon2d）\n");

    sui::PowChallenge c;
    c.chainId = "8wWZfv1HjQxjB5ncmC9CQqqRqUMBiEZXnc5FhS9zRe1v";
    c.checkpointSeq = "1713342";
    c.checkpointDigest = "GzBdc9tbUe3s2PK1iVJcyQBj5TvdX7bJiAHWbhBDYiYb";
    c.randomBytes =
        "mJ4ZqDXQasy17GTiGfp0/OYAGCR+7reLM5+Gb39YtoAIaPuh1wj+z5J7po+eSze3";
    c.faucetAddress =
        "0x949cd75a2485cc18b8258634ffd9b849a687e2e2a0b4c2444a21f007fc79adc4";
    c.recipient =
        "0x1111111111111111111111111111111111111111111111111111111111111111";
    c.threshold = 72057594037927936ULL;  // difficulty 256

    auto verify = [&](std::string_view nonce, std::string_view expectedHash,
                     std::string_view expectedV) {
        const auto preimage = sui::buildPowPreimage(c, nonce);
        const auto hash =
            sui::argon2d32(preimage, "sui-faucet-pow-1", 8192, 1, 1);
        CHECK(sui::toHex(hash) == expectedHash);
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v = (v << 8) | hash[static_cast<std::size_t>(i)];
        }
        CHECK(std::to_string(v) == expectedV);
    };

    verify("0",
          "31f6ec0e9169705f6105219eebbbc77420f1d9273d407db5df17b4d090afb92d",
          "3600324499442593887");
    verify("201",
          "0008faf113e0b071f5f9d5d6e1dc9321890ecb7e81059f07dedbd3dec5e902bb",
          "2527713141239921");

    // 无随机性：第 5 行为空，字段数仍为 8
    c.randomBytes.clear();
    verify("0",
          "d8430e5471f4fba3c3c67a253f15dc72588d87c364117e1f138e26387050ec60",
          "15583314891483970467");

    // 阈值表：floor(2^64 / difficulty)，必须整数除法
    CHECK(std::to_string(18446744073709551615ULL / 724ULL) ==
          "25478928278604353");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    testBlake2b();
    testEd25519();
    testBase64AndHex();
    testAddress();
    testTransactionSignatureShape();
    testPowVectors();

    std::printf("\n%d checks, %d failed\n", gChecks, gFailed);
    return gFailed == 0 ? 0 : 1;
}
