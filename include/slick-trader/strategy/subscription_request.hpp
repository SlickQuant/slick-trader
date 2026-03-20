#pragma once

#include <slick-trader/events/event.hpp>
#include <slick-trader/utils/types.hpp>
#include <string>
#include <vector>

namespace slick::trader {

// Subscription request from a strategy
struct SubscriptionRequest {
    BrokerId broker_id;                  // Which broker to subscribe to
    SubscriptionType type;                // Trade, Quote, or OrderBook
    std::vector<std::string> instruments; // List of instruments
    int depth = 10;                       // Order book depth (for orderbook subscriptions)

    SubscriptionRequest(BrokerId broker, SubscriptionType sub_type,
                       const std::vector<std::string>& instrs, int d = 10)
        : broker_id(broker), type(sub_type), instruments(instrs), depth(d) {}
};

} // namespace slick::trader
