// Test-only strategy plugin, built as one from before plugins named the strategy API they were
// built against: it exports create_strategy() and destroy_strategy() and nothing else.
// StrategyLoader has to refuse it.

#include <slick-trader/strategy/strategy_base.hpp>
#include <slick-trader/strategy/strategy_export.hpp>

class StaleStrategy : public slick::trader::StrategyBase {
public:
    StaleStrategy() : StrategyBase("Stale", "1.0.0") {}
};

extern "C" {

SLICK_TRADER_EXPORT slick::trader::IStrategy* create_strategy() { return new StaleStrategy(); }

SLICK_TRADER_EXPORT void destroy_strategy(slick::trader::IStrategy* strategy) { delete strategy; }

} // extern "C"
