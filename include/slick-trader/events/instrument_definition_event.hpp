#pragma once

#include <slick-trader/events/event.hpp>
#include <slick-trader/utils/types.hpp>
#include <algorithm>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace slick::trader {

constexpr size_t MAX_CURRENCY_LENGTH = 8;

// InstrumentDefinitionEvent - the static description of one tradable instrument,
// published by a broker's market data connector (for CME, from MDP 3.0's instrument
// definitions). StrategyManager applies the contract multiplier to position and risk
// accounting and forwards the event to every strategy.
#pragma pack(push, 1)
struct InstrumentDefinitionEvent : public Event {
    price_t tick_size;            // Minimum price increment, in the same price units as orders and market data
    double contract_multiplier;   // Currency value of a one-unit price move on one unit of quantity (ES: 50, BTC-USD: 1)
    qty_t min_quantity;           // Smallest order quantity (0 = no limit)
    qty_t max_quantity;           // Largest order quantity (0 = no limit)
    qty_t quantity_increment;     // Order quantity step (0 = any)
    char currency[MAX_CURRENCY_LENGTH];  // Settlement currency, e.g. "USD"

    InstrumentDefinitionEvent()
        : Event(EventType::INSTRUMENT_DEFINITION)
        , tick_size(0)
        , contract_multiplier(1.0)
        , min_quantity(0)
        , max_quantity(0)
        , quantity_increment(0)
        , currency{}
    {}

    void set_currency(std::string_view ccy) {
        size_t len = std::min(ccy.size(), MAX_CURRENCY_LENGTH - 1);
        std::memcpy(currency, ccy.data(), len);
        currency[len] = '\0';
    }
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<InstrumentDefinitionEvent>, "InstrumentDefinitionEvent must be trivially copyable");

} // namespace slick::trader
