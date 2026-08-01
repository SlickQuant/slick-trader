#pragma once

#include <slick-trader/strategy/strategy_base.hpp>

namespace slick::trader {

// Lightweight strategy that owns all orders placed manually through the Web UI.
// Always registered at startup; never subscribes to market data.
class ManualStrategy : public StrategyBase {
public:
    static constexpr const char* ID = "MANUAL";

    ManualStrategy() : StrategyBase(ID, "1.0.0") {}

    std::vector<SubscriptionRequest> get_subscriptions() const override { return {}; }

protected:
    bool on_initialize() override { return true; }
};

} // namespace slick::trader
