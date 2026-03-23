#pragma once

#include <slick-trader/events/event.hpp>
#include <slick-trader/utils/types.hpp>
#include <slick-trader/utils/order_types.hpp>
#include <type_traits>

namespace slick::trader {

// QuoteEvent - POD struct for lock-free queue
#pragma pack(push, 1)
struct PriceLevel {
    price_t price;
    qty_t quantity;
    Side side;

    PriceLevel() : price(0), quantity(0), side(Side::BUY) {}
    PriceLevel(price_t p, qty_t q, Side s) : price(p), quantity(q), side(s) {}
};

struct Level2UpdateEvent : public Event {
    uint16_t num_levels;
    uint64_t sequence_num;
    bool is_snapshot;  // true for snapshot, false for update
    PriceLevel levels[0];

    Level2UpdateEvent()
        : Event(EventType::LEVEL2_UPDATE)
        , num_levels(0)
        , sequence_num(0)
        , is_snapshot(false)
    {}
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<Level2UpdateEvent>, "Level2UpdateEvent must be trivially copyable");

} // namespace slick::trader
