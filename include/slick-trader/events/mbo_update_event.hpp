#pragma once

#include <slick-trader/events/event.hpp>
#include <slick-trader/utils/types.hpp>
#include <slick-trader/utils/order_types.hpp>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace slick::trader {

// What happened to one order of a market-by-order (level 3) book.
enum class MboAction : uint8_t {
    ADD,        // a new order
    MODIFY,     // its price, quantity or priority changed
    REMOVE,     // gone - cancelled or fully filled; only order_id is read
};

// MboUpdateEvent - POD struct for lock-free queue
#pragma pack(push, 1)
struct MboOrder {
    uint64_t order_id;      // the exchange's order id - the number an OrderAckEvent carries as text
    uint64_t priority;      // place in the queue at its price: lower trades first. 0 = the exchange
                            // gives none, and orders queue in the order they arrive
    price_t price;
    qty_t quantity;         // displayed quantity
    Side side;
    MboAction action;
};

// Order-by-order changes to one instrument's book. An exchange event that does not fit one
// record is spread over several: only the last has is_last set, and the book is consistent -
// and handed to strategies - only then.
struct MboUpdateEvent : public Event {
    uint16_t num_orders;
    uint64_t sequence_num;
    bool is_snapshot;       // the book starts over from these orders; set on a snapshot's first record
    bool is_last;           // the last record of the exchange event or snapshot
    MboOrder orders[0];

    MboUpdateEvent()
        : Event(EventType::MBO_UPDATE)
        , num_orders(0)
        , sequence_num(0)
        , is_snapshot(false)
        , is_last(true)
    {}

    // Bytes a record carrying `count` orders takes - what to publish it with.
    static constexpr uint32_t size_for(std::size_t count) noexcept {
        return static_cast<uint32_t>(sizeof(MboUpdateEvent) + count * sizeof(MboOrder));
    }
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<MboUpdateEvent>, "MboUpdateEvent must be trivially copyable");

} // namespace slick::trader
