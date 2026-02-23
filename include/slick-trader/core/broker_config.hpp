#pragma once

#include <slick-trader/events/event.hpp>
#include <slick-trader/brokers/imarket_data_connector.hpp>
#include <string>
#include <vector>
#include <unordered_map>

namespace slick::trader {

// Subscription configuration
struct SubscriptionConfig {
    SubscriptionType type;
    std::vector<std::string> instruments;
    int depth = 10;  // For order book subscriptions
};

// Market data connector configuration
struct MDConnectorConfig {
    bool enabled = true;
    std::string websocket_url;
    std::vector<SubscriptionConfig> subscriptions;
};

// Order connector configuration
struct OrderConnectorConfig {
    bool enabled = true;
    std::string rest_url;
    std::string websocket_url;
    // Note: Coinbase automatically uses COINBASE_API_KEY and COINBASE_API_SECRET from environment
    // Other brokers may need explicit API key configuration via custom_params
};

// Complete broker configuration
struct BrokerConfig {
    std::string broker_name;  // "coinbase", "alpaca", etc.
    BrokerId broker_id;
    bool enabled = true;
    MDConnectorConfig md_config;
    OrderConnectorConfig order_config;
    std::unordered_map<std::string, std::string> custom_params;
};

} // namespace slick::trader
