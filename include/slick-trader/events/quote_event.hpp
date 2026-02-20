#pragma once

#include <slick-trader/events/event.hpp>
#include <slick-trader/utils/types.hpp>
#include <type_traits>

namespace slick::trader {

// QuoteEvent - POD struct for lock-free queue
#pragma pack(push, 1)
struct QuoteEvent : public Event {
    price_t bid_price;
    qty_t bid_quantity;
    price_t ask_price;
    qty_t ask_quantity;

    QuoteEvent()
        : Event(EventType::QUOTE)
        , bid_price(0)
        , bid_quantity(0)
        , ask_price(0)
        , ask_quantity(0) {}
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<QuoteEvent>, "QuoteEvent must be trivially copyable");

} // namespace slick::trader
