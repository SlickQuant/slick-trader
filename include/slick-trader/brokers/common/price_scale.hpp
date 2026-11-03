#pragma once

#include <slick-trader/utils/types.hpp>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>

namespace slick::trader {

// Exact conversion between an exchange's integer wire prices and price_t (1e-8).
//
//     price_t = wire x num / den
//
// The ratio is kept reduced, so a conversion is one integer division and one exact
// multiplication. A price the target scale cannot hold exactly is refused rather than
// rounded: it can only be off-tick, and trading it would trade a price nobody quoted.
class PriceScale {
public:
    // Empty unless both terms are positive.
    static std::optional<PriceScale> from_ratio(int64_t num, int64_t den) noexcept {
        if (num <= 0 || den <= 0) {
            return std::nullopt;
        }
        const int64_t g = std::gcd(num, den);
        return PriceScale(num / g, den / g);
    }

    // The identity: wire prices already are price_t.
    static PriceScale identity() noexcept { return PriceScale(1, 1); }

    // Wire -> price_t. False for a null (INT64_MAX or INT64_MIN, which is every wire
    // format's null), an off-scale price, or one too large to hold.
    [[nodiscard]] bool to_price(int64_t wire, price_t& out) const noexcept {
        if (wire == WIRE_NULL_MAX || wire == WIRE_NULL_MIN || wire % den_ != 0) {
            return false;
        }
        const int64_t q = wire / den_;
        if (q > MAX_VALUE / num_ || q < -MAX_VALUE / num_) {
            return false;
        }
        out = q * num_;
        return true;
    }

    // price_t -> wire. False for NULL_PRICE, or a price finer than the wire can express.
    [[nodiscard]] bool to_wire(price_t price, int64_t& out) const noexcept {
        if (price == NULL_PRICE || price % num_ != 0) {
            return false;
        }
        const int64_t q = price / num_;
        if (q > MAX_VALUE / den_ || q < -MAX_VALUE / den_) {
            return false;
        }
        out = q * den_;
        return true;
    }

    [[nodiscard]] int64_t numerator() const noexcept { return num_; }
    [[nodiscard]] int64_t denominator() const noexcept { return den_; }

private:
    PriceScale(int64_t num, int64_t den) noexcept : num_(num), den_(den) {}

    static constexpr int64_t WIRE_NULL_MAX = std::numeric_limits<int64_t>::max();
    static constexpr int64_t WIRE_NULL_MIN = std::numeric_limits<int64_t>::min();
    // Keeps every result clear of both nulls, NULL_PRICE included.
    static constexpr int64_t MAX_VALUE = std::numeric_limits<int64_t>::max() - 1;

    int64_t num_;
    int64_t den_;
};

} // namespace slick::trader
