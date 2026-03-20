#pragma once

#include <slick-trader/core/position_tracker.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick/logger.hpp>
#include <unordered_map>
#include <string>
#include <chrono>

namespace slick::trader {

// Per-instrument risk limits
struct InstrumentRiskLimits {
    qty_t max_position_size{to_qty_t(1000000.0)};  // Max position for this instrument
    price_t max_order_value{to_price_t(100000.0)};  // Max notional value per order
};

// Risk limits configuration
struct RiskLimits {
    // Default position limits (applies to all instruments without specific limits)
    qty_t max_position_size{to_qty_t(1000000.0)};  // Max position per instrument
    price_t max_order_value{to_price_t(100000.0)};  // Max notional value per order

    // Per-instrument overrides (keyed by instrument name)
    std::unordered_map<std::string, InstrumentRiskLimits> instrument_limits;

    // Rate limits
    uint32_t max_orders_per_second{100};

    // Loss limits
    price_t max_loss_per_strategy{to_price_t(-10000.0)};  // Max loss before halt

    // Get effective limits for an instrument
    InstrumentRiskLimits get_instrument_limits(std::string_view instrument) const {
        auto it = instrument_limits.find(std::string(instrument));
        if (it != instrument_limits.end()) {
            return it->second;
        }
        // Return default limits
        return InstrumentRiskLimits{max_position_size, max_order_value};
    }
};

// Pre-trade risk manager
// Validates orders before submission
class RiskManager {
public:
    explicit RiskManager(PositionTracker& position_tracker)
        : position_tracker_(position_tracker) {}

    ~RiskManager() = default;

    // Set risk limits for a strategy
    void set_limits(const std::string& strategy_id, const RiskLimits& limits);

    // Get risk limits for a strategy
    RiskLimits get_limits(const std::string& strategy_id) const;

    // Validate order before submission
    // Returns true if order passes all checks, false otherwise
    bool validate_order(const std::string& strategy_id, std::string_view instrument,
                        Side side, qty_t quantity, price_t price, std::string* reject_reason = nullptr);

    // Check if strategy is halted due to risk breach
    bool is_halted(const std::string& strategy_id) const;

    // Manually halt/resume a strategy
    void halt_strategy(const std::string& strategy_id, const std::string& reason);
    void resume_strategy(const std::string& strategy_id);

    // Clear all limits and halt states
    void clear_all();

private:
    struct RateLimit {
        uint32_t count{0};
        uint64_t window_start_ns{0};
    };

    bool check_position_limit(const std::string& strategy_id, std::string_view instrument,
                                bool is_buy, qty_t order_qty, std::string* reason);

    bool check_order_value_limit(std::string_view instrument, price_t price, qty_t quantity, 
                                    const RiskLimits& limits, std::string* reason);

    bool check_rate_limit(const std::string& strategy_id, const RiskLimits& limits,
                            std::string* reason);

    PositionTracker& position_tracker_;
    std::unordered_map<std::string, RiskLimits> limits_;
    std::unordered_map<std::string, RateLimit> rate_limits_;
    std::unordered_map<std::string, std::string> halted_strategies_;  // strategy_id -> reason
};

} // namespace slick::trader
