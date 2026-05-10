#pragma once

#include <slick-trader/utils/types.hpp>
#include <cmath>

namespace slick::trader {

enum class FeeType {
    PERCENTAGE,   // fee = |notional| * rate  (e.g. Coinbase: rate = 0.006)
    FIXED_AMOUNT  // fee = rate per fill (flat amount in quote currency)
};

struct FeeStructure {
    FeeType type{FeeType::PERCENTAGE};
    double rate{0.0};
};

// Fallback fee computation used when fill event carries no fee (non-Coinbase brokers)
inline price_t compute_fee(price_t fill_price, qty_t fill_qty, const FeeStructure& fee) {
    if (fee.rate <= 0.0) return 0;
    if (fee.type == FeeType::PERCENTAGE) {
        double notional = std::abs(from_price_t(fill_price) * from_qty_t(fill_qty));
        return to_price_t(notional * fee.rate);
    }
    return to_price_t(fee.rate);  // FIXED_AMOUNT: flat fee per fill
}

} // namespace slick::trader
