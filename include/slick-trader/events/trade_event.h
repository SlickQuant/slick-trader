#pragma once

#include <slick-trader/events/event.h>
#include <slick-trader/utils/types.h>
#include <slick-trader/utils/order_types.h>
#include <type_traits>
#include <cstdint>

namespace slick::trader {

// TradeEvent - POD struct for lock-free queue
#pragma pack(push, 1)
struct TradeEvent : public Event {
    price_t price;
    qty_t quantity;
    bool is_buyer_maker;
    uint64_t trade_id;
    Side side;

    TradeEvent()
        : Event(EventType::TRADE)
        , price(0)
        , quantity(0)
        , is_buyer_maker(false)
        , trade_id(0)
        , side(Side::UNKNOWN)
    {}
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<TradeEvent>, "TradeEvent must be trivially copyable");

} // namespace slick::trader
