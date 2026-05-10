#pragma once

#include <slick-trader/utils/types.hpp>
#include <string>
#include <string_view>
#include <cstdint>

namespace slick::trader {

// Position for a single instrument
struct Position {
    std::string instrument;
    qty_t quantity{0};           // Current position size (positive = long, negative = short)
    price_t avg_price{0};        // Average entry price
    price_t realized_pnl{0};     // Gross realized PnL from closed trades
    price_t realized_fees{0};   // Fees on closed trades (entry fees of closed lots + their exit fees)
    price_t open_fees{0};       // Entry fees paid for currently held lots (not yet realized)
    uint64_t last_update_time{0}; // Last update timestamp

    Position() = default;
    Position(std::string_view inst) : instrument(std::string(inst)) {}

    bool is_flat() const { return quantity == 0; }
    bool is_long() const { return quantity > 0; }
    bool is_short() const { return quantity < 0; }

    // Calculate unrealized PnL given current market price
    double get_unrealized_pnl(price_t mark_price) const {
        if (is_flat()) return 0.0;
        return from_qty_t(quantity) * (from_price_t(mark_price) - from_price_t(avg_price));
    }

    double get_total_pnl(price_t mark_price) const {
        return from_price_t(realized_pnl) + get_unrealized_pnl(mark_price);
    }
};

} // namespace slick::trader
