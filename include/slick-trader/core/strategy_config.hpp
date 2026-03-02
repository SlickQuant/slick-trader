#pragma once

#include <slick-trader/core/risk_manager.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <optional>

namespace slick::trader {

// Per-strategy configuration
// Loaded from JSON config files
struct StrategyConfig {
    std::string name;                    // Strategy identifier (matches plugin name)
    bool enabled{true};                  // Whether to load and run this strategy

    // Risk limits
    RiskLimits risk_limits;

    // Custom parameters (strategy-specific)
    // Strategies can access these via get_parameter()
    nlohmann::json parameters;

    // Parse from JSON
    static StrategyConfig from_json(const nlohmann::json& j);

    // Get typed parameter with default
    template<typename T>
    T get_parameter(const std::string& key, const T& default_value) const {
        if (parameters.contains(key)) {
            try {
                return parameters[key].get<T>();
            } catch (const std::exception&) {
                return default_value;
            }
        }
        return default_value;
    }
};

// Parse strategy configuration from JSON
inline StrategyConfig StrategyConfig::from_json(const nlohmann::json& j) {
    StrategyConfig config;

    config.name = j.value("name", "");
    config.enabled = j.value("enabled", true);

    // Parse risk limits if present
    if (j.contains("risk_limits")) {
        const auto& rl = j["risk_limits"];

        if (rl.contains("max_position_size")) {
            config.risk_limits.max_position_size = to_qty_t(rl["max_position_size"].get<double>());
        }

        if (rl.contains("max_order_value")) {
            config.risk_limits.max_order_value = to_price_t(rl["max_order_value"].get<double>());
        }

        if (rl.contains("max_orders_per_second")) {
            config.risk_limits.max_orders_per_second = rl["max_orders_per_second"].get<uint32_t>();
        }

        if (rl.contains("max_loss_per_strategy")) {
            config.risk_limits.max_loss_per_strategy = to_price_t(rl["max_loss_per_strategy"].get<double>());
        }

        // Parse per-instrument limits
        if (rl.contains("instruments") && rl["instruments"].is_object()) {
            for (const auto& [instrument, inst_limits] : rl["instruments"].items()) {
                InstrumentRiskLimits limits;

                if (inst_limits.contains("max_position_size")) {
                    limits.max_position_size = to_qty_t(inst_limits["max_position_size"].get<double>());
                }

                if (inst_limits.contains("max_order_value")) {
                    limits.max_order_value = to_price_t(inst_limits["max_order_value"].get<double>());
                }

                config.risk_limits.instrument_limits[instrument] = limits;
            }
        }
    }

    // Store custom parameters
    if (j.contains("parameters")) {
        config.parameters = j["parameters"];
    }

    return config;
}

} // namespace slick::trader
