// 最小 Sui SDK 命令行：用来在测试网端到端验证 SDK 本身。
// 用法：
//   sui_cli keygen <seedHex>
//   sui_cli balance <address>
//   sui_cli faucet <address>
//   sui_cli transfer <seedHex> <toAddress> [mist]

#include <sui/client.hpp>
#include <sui/crypto.hpp>
#include <sui/pow.hpp>
#include <sui/transaction.hpp>

#include <tgbot/http_client.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace net = boost::asio;

namespace {

std::string arg(int argc, char** argv, int index) {
    return index < argc ? std::string{argv[index]} : std::string{};
}

net::awaitable<void> run(int argc, char** argv) {
    const std::string command = arg(argc, argv, 1);
    sui::SuiClient client;

    if (command == "keygen") {
        const auto kp = sui::Keypair::fromSeedHex(arg(argc, argv, 2));
        std::printf("address: %s\n", kp.addressHex().c_str());
        std::printf("pubkey : %s\n", sui::toHex(kp.publicKey()).c_str());
        co_return;
    }

    if (command == "balance") {
        const auto value = co_await client.balance(arg(argc, argv, 2));
        std::printf("%llu MIST (%s SUI)\n", static_cast<unsigned long long>(value),
                    std::to_string(value / 1'000'000'000.0).c_str());
        co_return;
    }

    if (command == "faucet") {
        const std::string address = arg(argc, argv, 2);
        const auto challenge = co_await client.fetchChallenge(address);
        if (challenge.threshold == 0) {
            std::printf("challenge failed: %s\n", client.lastError().c_str());
            co_return;
        }
        std::printf("challenge: checkpoint=%s threshold=%llu amount=%llu MIST\n",
                    challenge.checkpointSeq.c_str(),
                    static_cast<unsigned long long>(challenge.threshold),
                    static_cast<unsigned long long>(challenge.amountMist));

        const auto started = std::chrono::steady_clock::now();
        const auto solution = sui::solvePow(challenge);
        if (!solution.has_value()) {
            std::printf("pow: no solution\n");
            co_return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        std::printf("pow: nonce=%s v=%llu (%lldms, %s)\n",
                    solution->nonce.c_str(),
                    static_cast<unsigned long long>(solution->v),
                    static_cast<long long>(elapsed),
                    solution->hashHex.c_str());

        const auto gas = co_await client.requestGas(
            address, challenge.checkpointSeq, solution->nonce,
            solution->hashHex);
        std::printf("gas: %s digest=%s code=%s %s\n",
                    gas.success ? "SUCCESS" : "FAILED", gas.digest.c_str(),
                    gas.code.c_str(), gas.error.c_str());
        co_return;
    }
    if (command == "transfer") {
        const auto kp = sui::Keypair::fromSeedHex(arg(argc, argv, 2));
        const std::string to = arg(argc, argv, 3);
        const auto amount = arg(argc, argv, 4).empty()
                                ? std::uint64_t{1'000'000}  // 0.001 SUI
                                : std::stoull(arg(argc, argv, 4));

        auto coins = co_await client.coins(kp.addressHex());
        std::printf("coin objects: %zu\n", coins.size());

        sui::TransferRequest request;
        request.sender = kp.address();
        // 没有 coin 对象时，gas 走 address balance，必须用 ValidDuring
        const auto info = co_await client.chainInfo();
        const auto chainBytes = sui::fromBase58(info.chainIdentifier);
        if (chainBytes.size() >= 32) {
            std::memcpy(request.chain.data(), chainBytes.data(), 32);
        }
        request.expirationNonce = 0;
        request.minEpoch = info.epoch;
        request.maxEpoch = info.epoch + 1;
        std::printf("epoch: %llu chain: %.8s\n",
                    static_cast<unsigned long long>(info.epoch),
                    info.chainIdentifier.c_str());
        const auto toBytes = sui::fromHex(to);
        if (toBytes.size() != 32) {
            std::printf("bad address length\n");
            co_return;
        }
        std::memcpy(request.recipient.data(), toBytes.data(), 32);
        request.amountMist = amount;
        if (!coins.empty()) {
            request.gasCoins.push_back(coins.front().ref);
        }

        const auto txBytes = sui::buildTransferTransaction(request);
        const auto signature = kp.signTransaction(txBytes);

        std::printf("tx bytes : %zu\n", txBytes.size());
        if (!coins.empty()) {
            std::printf("gas coin : %s v%llu\n",
                        sui::toHex(coins.front().ref.objectId).c_str(),
                        static_cast<unsigned long long>(coins.front().ref.version));
        } else {
            std::printf("gas coin : (address balance)\n");
        }

        const auto outcome = co_await client.executeTransaction(
            sui::toBase64(txBytes), sui::toBase64(signature));
        std::printf("digest: %s\n", outcome.digest.c_str());
        std::printf("status: %s\n", outcome.success ? "SUCCESS" : "FAILED");
        if (!outcome.error.empty()) {
            std::printf("error : %s\n", outcome.error.c_str());
        }
        co_return;
    }

    std::printf("unknown command\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    net::io_context io;
    std::exception_ptr failure;
    net::co_spawn(
        io,
        [&]() -> net::awaitable<void> {
            try {
                co_await run(argc, argv);
            } catch (const std::exception& e) {
                std::printf("exception: %s\n", e.what());
                failure = std::current_exception();
            }
        },
        net::detached);
    io.run();
    return failure ? 1 : 0;
}
