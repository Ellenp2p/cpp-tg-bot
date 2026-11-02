#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tgbot {

// 128 位无符号整数，用作"链上最小单位"的金额类型（wei / token 最小单位）。
//
// 为什么不用 unsigned __int128：MSVC 不支持（只有 GCC/clang 有），
// 而本项目的 Windows 构建是主力平台。所以这里用两个 64 位 limb 手写，
// 只实现金额真正需要的运算，溢出时抛 std::overflow_error 而不是静默回绕。
class Amount {
public:
    Amount() = default;
    static Amount fromUint64(std::uint64_t value);
    static Amount zero() { return Amount{}; }

    // 十进制字符串；非法（空、非数字、溢出）返回 nullopt
    static std::optional<Amount> parse(std::string_view decimal);

    // 分 → 最小单位（cents × 10^(decimals-2)）；decimals<2 时返回 nullopt
    static std::optional<Amount> fromCents(std::int64_t cents, int decimals);

    std::string toString() const;

    bool isZero() const { return hi_ == 0 && lo_ == 0; }
    // 是否能用 uint64 表示
    bool fitsUint64() const { return hi_ == 0; }
    std::uint64_t toUint64() const;

    friend bool operator==(const Amount& a, const Amount& b) {
        return a.hi_ == b.hi_ && a.lo_ == b.lo_;
    }
    friend bool operator!=(const Amount& a, const Amount& b) { return !(a == b); }
    friend bool operator<(const Amount& a, const Amount& b) {
        return a.hi_ < b.hi_ || (a.hi_ == b.hi_ && a.lo_ < b.lo_);
    }
    friend bool operator<=(const Amount& a, const Amount& b) { return !(b < a); }
    friend bool operator>(const Amount& a, const Amount& b) { return b < a; }

    friend Amount operator+(const Amount& a, const Amount& b); // 溢出抛
    friend Amount operator-(const Amount& a, const Amount& b); // 下溢抛
    Amount mulSmall(std::uint64_t factor) const;               // 溢出抛
    // 除以小整数：返回 (商, 余数)
    std::pair<Amount, std::uint64_t> divmod(std::uint64_t divisor) const;
    // 取 [lo, hi] 区间内的随机值（用于红包拆分），保证每次都有界
    Amount randomBetween(const Amount& low, const Amount& high) const;

    std::uint64_t high() const { return hi_; }
    std::uint64_t low() const { return lo_; }

private:
    Amount(std::uint64_t hi, std::uint64_t lo) : hi_(hi), lo_(lo) {}
    std::uint64_t hi_ = 0;
    std::uint64_t lo_ = 0;
};

} // namespace tgbot
