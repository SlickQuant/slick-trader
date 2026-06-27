#pragma once

#include <slick-trader/brokers/ibroker.hpp>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>

namespace slick::trader {

class BrokerFactory {
public:
    // Create a broker from configuration
    static std::shared_ptr<IBroker> create_broker(
        const json& config,
        slick::stream_buffer_multiplexer &mux,
        slick::queue<uint8_t>& control_queue,
        slick::queue<uint8_t>& md_request_queue,
        slick::queue<uint8_t>& order_request_queue
    );

private:
    static BrokerId parse_broker_id(const std::string& broker_name);
    static SubscriptionType parse_subscription_type(const std::string& type_str);
};

} // namespace slick::trader
