#pragma once

#include <slick-trader/events/event.hpp>
#include <type_traits>

namespace slick::trader {

// The WebServer asking, over the event bus, for MANUAL to hold the level 2 book of the
// event's instrument at its broker while a ladder shows it (`subscribe`), or to let it go.
#pragma pack(push, 1)
struct LadderSubscriptionEvent : public Event {
    bool subscribe;

    LadderSubscriptionEvent() : Event(EventType::LADDER_SUBSCRIPTION), subscribe(true) {}
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<LadderSubscriptionEvent>, "LadderSubscriptionEvent must be trivially copyable");

} // namespace slick::trader
