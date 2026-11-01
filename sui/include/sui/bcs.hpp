#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sui {

// BCS（Binary Canonical Serialization）编码器，只实现 Sui 交易需要的那部分。
// 规则：整数全小端；vector<u8> 是 uleb128 长度 + 原始字节；
// 枚举是 uleb128 变体索引 + 变体内容；结构体按字段顺序拼接。
class BcsWriter {
public:
    void u8(std::uint8_t value) { buffer_.push_back(value); }

    void u16(std::uint16_t value) {
        buffer_.push_back(static_cast<std::uint8_t>(value & 0xFF));
        buffer_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    }

    void u32(std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            buffer_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
        }
    }

    void u64(std::uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            buffer_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
        }
    }

    // ULEB128，用于长度和枚举 tag
    void uleb(std::uint64_t value) {
        while (true) {
            auto byte = static_cast<std::uint8_t>(value & 0x7F);
            value >>= 7;
            if (value != 0) {
                buffer_.push_back(static_cast<std::uint8_t>(byte | 0x80));
            } else {
                buffer_.push_back(byte);
                break;
            }
        }
    }

    void raw(std::span<const std::uint8_t> bytes) {
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }

    // vector<u8>：长度前缀 + 内容
    void blob(std::span<const std::uint8_t> bytes) {
        uleb(bytes.size());
        raw(bytes);
    }

    void blob(std::string_view text) {
        blob(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }

    void boolean(bool value) { u8(value ? 1 : 0); }

    const std::vector<std::uint8_t>& data() const { return buffer_; }
    std::vector<std::uint8_t> take() { return std::move(buffer_); }

private:
    std::vector<std::uint8_t> buffer_;
};

}  // namespace sui
