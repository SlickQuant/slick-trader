#pragma once

#include <slick-trader/events/event.h>
#include <slick-trader/utils/types.h>
#include <type_traits>

namespace slick::trader {

constexpr size_t MAX_ORDER_BOOK_LEVELS = 10;

#pragma pack(push, 1)
struct OrderBookPriceLevel {
    price_t price;
    qty_t quantity;

    OrderBookPriceLevel() : price(0), quantity(0) {}
    OrderBookPriceLevel(price_t p, qty_t q) : price(p), quantity(q) {}
};

// OrderBookEvent - POD struct for lock-free queue
struct OrderBookEvent : public Event {
    OrderBookPriceLevel bids[MAX_ORDER_BOOK_LEVELS];
    OrderBookPriceLevel asks[MAX_ORDER_BOOK_LEVELS];
    uint16_t num_bids;
    uint16_t num_asks;
    uint64_t sequence_num;
    bool is_snapshot;  // true for snapshot, false for update

    OrderBookEvent()
        : Event(EventType::ORDER_BOOK)
        , bids{}
        , asks{}
        , num_bids(0)
        , num_asks(0)
        , sequence_num(0)
        , is_snapshot(false) {}
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<OrderBookEvent>, "OrderBookEvent must be trivially copyable");

} // namespace slick::trader
