#include <tgbot/amount.hpp>

#include <algorithm>
#include <random>
#include <stdexcept>

namespace tgbot {

namespace {

// 64×64 → 128
void mul64(std::uint64_t a, std::uint64_t b, std::uint64_t& hi,
           std::uint64_t& lo) {
    const auto a0 = a & 0xFFFFFFFFULL;
    const auto a1 = a >> 32;
    const auto b0 = b & 0xFFFFFFFFULL;
    const auto b1 = b >> 32;
    const auto p00 = a0 * b0;
    const auto p01 = a0 * b1;
    const auto p10 = a1 * b0;
    const auto p11 = a1 * b1;
    const auto mid = (p00 >> 32) + (p01 & 0xFFFFFFFFULL) +
                     (p10 & 0xFFFFFFFFULL);
    lo = (p00 & 0xFFFFFFFFULL) | (mid << 32);
    hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

}  // namespace

Amount Amount::fromUint64(std::uint64_t value) { return Amount(0, value); }

std::optional<Amount> Amount::parse(std::string_view decimal) {
    if (decimal.empty()) {
        return std::nullopt;
    }
    Amount acc;
    for (const char c : decimal) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        try {
            acc = acc.mulSmall(10) + Amount::fromUint64(
                                         static_cast<std::uint64_t>(c - '0'));
        } catch (const std::exception&) {
            return std::nullopt;  // 溢出
        }
    }
    return acc;
}

std::optional<Amount> Amount::fromCents(std::int64_t cents, int decimals) {
    if (cents < 0 || decimals < 2 || decimals > 30) {
        return std::nullopt;
    }
    Amount value = Amount::fromUint64(static_cast<std::uint64_t>(cents));
    try {
        for (int i = 0; i < decimals - 2; ++i) {
            value = value.mulSmall(10);
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return value;
}

std::string Amount::toString() const {
    if (isZero()) {
        return "0";
    }
    std::string digits;
    Amount value = *this;
    while (!value.isZero()) {
        const auto [quotient, remainder] = value.divmod(10);
        digits.push_back(static_cast<char>('0' + remainder));
        value = quotient;
    }
    std::reverse(digits.begin(), digits.end());
    return digits;
}

std::uint64_t Amount::toUint64() const {
    if (hi_ != 0) {
        throw std::overflow_error("Amount 超出 uint64");
    }
    return lo_;
}

Amount operator+(const Amount& a, const Amount& b) {
    const auto lo = a.lo_ + b.lo_;
    const bool carry = lo < a.lo_;
    auto hi = a.hi_ + b.hi_;
    if (carry && ++hi == 0) {
        throw std::overflow_error("Amount 加法溢出");
    }
    return Amount(hi, lo);
}

Amount operator-(const Amount& a, const Amount& b) {
    if (b > a) {
        throw std::underflow_error("Amount 减法下溢");
    }
    const bool borrow = a.lo_ < b.lo_;
    return Amount(a.hi_ - b.hi_ - (borrow ? 1 : 0), a.lo_ - b.lo_);
}

Amount Amount::mulSmall(std::uint64_t factor) const {
    std::uint64_t hi1 = 0;
    std::uint64_t lo1 = 0;
    mul64(lo_, factor, hi1, lo1);
    std::uint64_t hi2 = 0;
    std::uint64_t lo2 = 0;
    mul64(hi_, factor, hi2, lo2);
    if (hi2 != 0) {
        throw std::overflow_error("Amount 乘法溢出");
    }
    const auto newHi = hi1 + lo2;
    if (newHi < hi1) {
        throw std::overflow_error("Amount 乘法溢出");
    }
    return Amount(newHi, lo1);
}

std::pair<Amount, std::uint64_t> Amount::divmod(std::uint64_t divisor) const {
    if (divisor == 0) {
        throw std::domain_error("Amount 除以 0");
    }
    Amount quotient;
    std::uint64_t remainderHi = 0;
    std::uint64_t remainderLo = 0;

    for (int i = 127; i >= 0; --i) {
        // remainder <<= 1，并把被除数的第 i 位塞进最低位
        remainderHi = (remainderHi << 1) | (remainderLo >> 63);
        remainderLo <<= 1;
        const bool bit =
            i >= 64 ? ((hi_ >> (i - 64)) & 1U) != 0 : ((lo_ >> i) & 1U) != 0;
        if (bit) {
            remainderLo |= 1U;
        }
        if (remainderHi != 0 || remainderLo >= divisor) {
            const bool borrow = remainderLo < divisor;
            remainderLo -= divisor;
            if (borrow) {
                remainderHi -= 1;
            }
            if (i >= 64) {
                quotient.hi_ |= (std::uint64_t{1} << (i - 64));
            } else {
                quotient.lo_ |= (std::uint64_t{1} << i);
            }
        }
    }
    return {quotient, remainderLo};
}

Amount Amount::randomBetween(const Amount& low, const Amount& high) const {
    if (high <= low) {
        return low;
    }
    if (!high.fitsUint64()) {
        // 超出 64 位时退化为中点，保证不越界（实际红包金额不会走到这里）
        return low + (high - low).divmod(2).first;
    }
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::uint64_t> dist(low.toUint64(),
                                                      high.toUint64());
    return Amount::fromUint64(dist(rng));
}

}  // namespace tgbot
