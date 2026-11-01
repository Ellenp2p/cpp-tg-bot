#include <sui/transaction.hpp>

#include <sui/bcs.hpp>

#include <span>

namespace sui {

namespace {

void writeU64Le(BcsWriter& writer, std::uint64_t value) { writer.u64(value); }

}  // namespace

std::vector<std::uint8_t> buildTransferTransaction(
    const TransferRequest& request) {
    BcsWriter writer;

    // TransactionData 是个枚举，V1 的变体 tag 是 0。
    // 少了这个字节后面全部错位（节点会报 "invalid length ... expected 32"）
    writer.uleb(0);

    // ---- TransactionKind::ProgrammableTransaction（变体 0）----
    writer.uleb(0);

    // ProgrammableTransaction.inputs（2 个 Pure 参数）
    writer.uleb(2);

    // input 0：金额（u64 小端）
    writer.uleb(0);  // CallArg::Pure
    {
        std::vector<std::uint8_t> amount(8);
        for (int i = 0; i < 8; ++i) {
            amount[static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>((request.amountMist >> (8 * i)) & 0xFF);
        }
        writer.blob(amount);
    }

    // input 1：收款地址（32 字节）
    writer.uleb(0);  // CallArg::Pure
    writer.blob(std::span<const std::uint8_t>(request.recipient));

    // ProgrammableTransaction.commands（2 条）
    writer.uleb(2);

    // cmd 0：SplitCoins(GasCoin, [Input(0)]) → Result(0)
    writer.uleb(2);  // Command::SplitCoins
    writer.uleb(0);  // Argument::GasCoin
    writer.uleb(1);  // vector 长度
    writer.uleb(1);  // Argument::Input
    writer.u16(0);

    // cmd 1：TransferObjects([Result(0)], Input(1))
    writer.uleb(1);  // Command::TransferObjects
    writer.uleb(1);  // 对象数量
    writer.uleb(2);  // Argument::Result
    writer.u16(0);
    writer.uleb(1);  // Argument::Input
    writer.u16(1);

    // ---- TransactionData 其余字段 ----
    writer.raw(std::span<const std::uint8_t>(request.sender));  // sender

    // gas_payment { objects, owner, price, budget }
    writer.uleb(request.gasCoins.size());
    for (const auto& coin : request.gasCoins) {
        writer.raw(std::span<const std::uint8_t>(coin.objectId));
        writeU64Le(writer, coin.version);
        writer.raw(std::span<const std::uint8_t>(coin.digest));
    }
    writer.raw(std::span<const std::uint8_t>(request.sender));  // owner
    writer.u64(request.gasPrice);
    writer.u64(request.gasBudget);

    // expiration
    if (request.useValidDuring) {
        // 从真实链上交易反推出的布局：
        //   tag 2
        //   Option<u64> min_epoch   (01 + u64)
        //   Option<u64> max_epoch   (01 + u64)
        //   Option<u64> min_timestamp (00)
        //   Option<u64> max_timestamp (00)
        //   Vec<u8> chain           (20 + 32 字节 genesis digest)
        writer.uleb(2);
        writer.uleb(1);
        writer.u64(request.minEpoch);
        writer.uleb(1);
        writer.u64(request.maxEpoch);
        writer.boolean(false);
        writer.boolean(false);
        writer.blob(std::span<const std::uint8_t>(request.chain));
    } else {
        writer.uleb(0);  // None
    }

    return writer.take();
}

}  // namespace sui
