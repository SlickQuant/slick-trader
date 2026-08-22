// Test-only strategy plugin.
//
// Built exactly like a real strategy: a shared library that links slick-trader-core
// statically and therefore owns a private copy of every singleton in that library.
// The probe entry points below report what THIS module's BrokerStatusRegistry says,
// which is what makes the cross-module bug observable from a test.

#include <slick-trader/strategy/strategy_base.hpp>
#include <slick-trader/strategy/strategy_export.hpp>

using namespace slick::trader;

class ProbeStrategy : public StrategyBase {
public:
    ProbeStrategy() : StrategyBase("Probe", "1.0.0") {}
};

SLICK_TRADER_EXPORT_STRATEGY(ProbeStrategy)

extern "C" {

SLICK_TRADER_EXPORT bool probe_is_order_enabled(int broker_id) {
    return BrokerStatusRegistry::instance().is_order_enabled(static_cast<BrokerId>(broker_id));
}

SLICK_TRADER_EXPORT bool probe_is_order_connected(int broker_id) {
    return BrokerStatusRegistry::instance().is_order_connected(static_cast<BrokerId>(broker_id));
}

} // extern "C"
