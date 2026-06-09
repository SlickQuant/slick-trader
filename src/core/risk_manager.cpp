#include <slick-trader/core/risk_manager.hpp>
#include <chrono>
#include <cmath>

namespace slick::trader {

void RiskManager::set_limits(const std::string& strategy_id, const RiskLimits& limits) {
    
    limits_[strategy_id] = limits;
    LOG_INFO("RiskManager: Set limits for {}: max_pos={}, max_order_value={}, max_orders/sec={}",
             strategy_id, from_qty_t(limits.max_position_size),
             from_price_t(limits.max_order_value), limits.max_orders_per_second);
}

RiskLimits RiskManager::get_limits(const std::string& strategy_id) const {
    
    auto it = limits_.find(strategy_id);
    if (it == limits_.end()) {
        return RiskLimits{};  // Return default limits
    }
    return it->second;
}

bool RiskManager::validate_order(const std::string& strategy_id, std::string_view instrument,
                                    Side side, qty_t quantity, price_t price, std::string* reject_reason) {

    // Check if strategy is halted
    if (halted_strategies_.find(strategy_id) != halted_strategies_.end()) {
        if (reject_reason) {
            *reject_reason = "Strategy halted: " + halted_strategies_[strategy_id];
        }
        return false;
    }

    // Get limits
    auto limits_it = limits_.find(strategy_id);
    if (limits_it == limits_.end()) {
        // No limits configured - allow by default
        return true;
    }

    const RiskLimits& limits = limits_it->second;

    // Check position limit
    if (!check_position_limit(strategy_id, instrument, side == Side::BUY,
                               quantity, reject_reason)) {
        return false;
    }

    // Check order value limit
    if (!check_order_value_limit(instrument, price, quantity, limits, reject_reason)) {
        return false;
    }

    // Check rate limit
    if (!check_rate_limit(strategy_id, limits, reject_reason)) {
        return false;
    }

    return true;
}

bool RiskManager::check_position_limit(const std::string& strategy_id, std::string_view instrument,
                                         bool is_buy, qty_t order_qty, std::string* reason) {
    // Get current position
    Position pos = position_tracker_.get_position(strategy_id, instrument);

    // Calculate position after order
    const qty_t signed_order_qty = is_buy ? order_qty : -order_qty;
    const qty_t new_position = pos.quantity + signed_order_qty;

    // Get limits
    auto limits_it = limits_.find(strategy_id);
    if (limits_it == limits_.end()) {
        return true;
    }

    // Get instrument-specific limits (or defaults)
    const auto inst_limits = limits_it->second.get_instrument_limits(instrument);
    const qty_t max_pos = inst_limits.max_position_size;

    const bool is_reducing = std::abs(from_qty_t(new_position)) < std::abs(from_qty_t(pos.quantity));
    if (!is_reducing && std::abs(from_qty_t(new_position)) > from_qty_t(max_pos)) {
        if (reason) {
            *reason = "Position limit exceeded for " + std::string(instrument) +
                      ": current=" + std::to_string(from_qty_t(pos.quantity)) +
                      " order=" + std::to_string(from_qty_t(order_qty)) +
                      " max=" + std::to_string(from_qty_t(max_pos));
        }
        LOG_WARN("RiskManager: Position limit check failed for {}: {}", strategy_id,
                 reason ? *reason : "");
        return false;
    }

    return true;
}

bool RiskManager::check_order_value_limit(std::string_view instrument, price_t price, qty_t quantity,
                                            const RiskLimits& limits, std::string* reason) {
    const double order_value = from_price_t(price) * from_qty_t(quantity);

    // Get instrument-specific limits (or defaults)
    const auto inst_limits = limits.get_instrument_limits(instrument);
    const double max_value = from_price_t(inst_limits.max_order_value);

    if (order_value > max_value) {
        if (reason) {
            *reason = "Order value limit exceeded for " + std::string(instrument) +
                      ": value=" + std::to_string(order_value) +
                      " max=" + std::to_string(max_value);
        }
        LOG_WARN("RiskManager: Order value check failed: {}", reason ? *reason : "");
        return false;
    }

    return true;
}

bool RiskManager::check_rate_limit(const std::string& strategy_id, const RiskLimits& limits,
                                     std::string* reason) {
    const uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    constexpr uint64_t ONE_SECOND_NS = 1'000'000'000;

    auto& rate_limit = rate_limits_[strategy_id];

    // Reset window if more than 1 second has passed
    if (now_ns - rate_limit.window_start_ns >= ONE_SECOND_NS) {
        rate_limit.count = 0;
        rate_limit.window_start_ns = now_ns;
    }

    // Check if we've exceeded the limit
    if (rate_limit.count >= limits.max_orders_per_second) {
        if (reason) {
            *reason = "Rate limit exceeded: " + std::to_string(rate_limit.count) +
                      " orders in last second (max=" + std::to_string(limits.max_orders_per_second) + ")";
        }
        LOG_WARN("RiskManager: Rate limit check failed for {}: {}", strategy_id,
                 reason ? *reason : "");
        return false;
    }

    // Increment counter
    rate_limit.count++;
    return true;
}

bool RiskManager::is_halted(const std::string& strategy_id) const {

    return halted_strategies_.find(strategy_id) != halted_strategies_.end();
}

std::string RiskManager::get_halt_reason(const std::string& strategy_id) const {
    auto it = halted_strategies_.find(strategy_id);
    return it != halted_strategies_.end() ? it->second : "";
}

void RiskManager::halt_strategy(const std::string& strategy_id, const std::string& reason) {
    
    halted_strategies_[strategy_id] = reason;
    LOG_ERROR("RiskManager: HALTED strategy {}: {}", strategy_id, reason);
}

void RiskManager::resume_strategy(const std::string& strategy_id) {
    
    halted_strategies_.erase(strategy_id);
    LOG_INFO("RiskManager: RESUMED strategy {}", strategy_id);
}

void RiskManager::clear_all() {
    
    limits_.clear();
    rate_limits_.clear();
    halted_strategies_.clear();
    LOG_INFO("RiskManager: Cleared all limits and halt states");
}

} // namespace slick::trader
