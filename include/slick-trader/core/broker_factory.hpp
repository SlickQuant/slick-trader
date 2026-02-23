#pragma once

#include <slick-trader/core/broker_config.hpp>
#include <slick-trader/brokers/ibroker.hpp>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>

namespace slick::trader {

class BrokerFactory {
public:
    // Create a broker from configuration
    static std::shared_ptr<IBroker> create_broker(const BrokerConfig& config);

    // Parse broker config from JSON
    static BrokerConfig parse_broker_config(const nlohmann::json& json);

private:
    static BrokerId parse_broker_id(const std::string& broker_name);
    static SubscriptionType parse_subscription_type(const std::string& type_str);
};

} // namespace slick::trader
