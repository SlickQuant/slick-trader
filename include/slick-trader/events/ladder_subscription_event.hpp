#pragma once

#include <slick-trader/events/event.hpp>
#include <cstdint>
#include <type_traits>

namespace slick::trader {

// The feeds MANUAL holds of an instrument for a ladder, as bits: its level 2 book, and its trades
// for the LTQ column. The WebServer skips any feed a strategy already streams.
enum LadderFeed : uint8_t {
    LADDER_BOOK = 1,
    LADDER_TRADES = 2,
    LADDER_ALL = LADDER_BOOK | LADDER_TRADES,
};

// Sent by the WebServer over the event bus. MANUAL holds the `feeds` of the event's instrument at
// its broker while a ladder shows it (`subscribe`), and releases what it held otherwise.
#pragma pack(push, 1)
struct LadderSubscriptionEvent : public Event {
    bool subscribe;
    uint8_t feeds;   // LadderFeed bits, read only on a hold

    LadderSubscriptionEvent() : Event(EventType::LADDER_SUBSCRIPTION), subscribe(true), feeds(LADDER_ALL) {}
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<LadderSubscriptionEvent>, "LadderSubscriptionEvent must be trivially copyable");

} // namespace slick::trader
