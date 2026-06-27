#pragma once

#include "event.hpp"
#include <slick-trader/utils/types.hpp>

namespace slick::trader {

#pragma pack(push, 1)
struct MDSubscription : public Event {
    SubscriptionType subscription_type;
    uint32_t depth;
    uint32_t num_instruments;
    char instruments[0][32];
};
#pragma pack(pop)
}