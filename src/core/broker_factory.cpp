#include <slick-trader/core/broker_factory.hpp>
#include <slick-trader/brokers/coinbase/coinbase_broker.hpp>
#include <slick-trader/brokers/coinbase/coinbase_md_connector.hpp>
#include <slick-trader/brokers/coinbase/coinbase_order_connector.hpp>
#include <slick-trader/brokers/hyperliquid/hyperliquid_broker.hpp>
#include <slick-trader/brokers/hyperliquid/hyperliquid_md_connector.hpp>
#include <slick-trader/brokers/hyperliquid/hyperliquid_order_connector.hpp>
#include <slick/logger.hpp>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <algorithm>

namespace slick::trader {

std::shared_ptr<IBroker> BrokerFactory::create_broker(
    const json& config,
    slick::stream_buffer_multiplexer &mux,
    slick::queue<uint8_t>& control_queue,
    slick::queue<uint8_t>& md_request_queue,
    slick::queue<uint8_t>& order_request_queue
) {
    if (!config.value("enabled", true)) [[unlikely]] {
        LOG_DEBUG("Broker {} is disabled in config", config.value("name", "unknown"));
        return nullptr;
    }

    std::shared_ptr<IBroker> broker;

    auto broker_name = config.value("name", "unknown");
    auto broker_id = parse_broker_id(broker_name);

    // Create broker based on type
    switch (broker_id) {
        case BrokerId::COINBASE: {
            auto coinbase_broker = std::make_shared<CoinbaseBroker>(config, mux, control_queue, md_request_queue, order_request_queue);
            broker = coinbase_broker;
            break;
        }

        case BrokerId::HYPERLIQUID: {
            auto hl_broker = std::make_shared<HyperliquidBroker>(config, mux, control_queue, md_request_queue, order_request_queue);

            // if (config.order_config.enabled) {
            //     hl_broker->set_order_api_url(config.order_config.rest_url);

            //     const char* pk = std::getenv("HYPERLIQUID_WALLET_KEY");
            //     if (pk) {
            //         hl_broker->set_private_key(pk);
            //         LOG_DEBUG("Hyperliquid will use private key from HYPERLIQUID_WALLET_KEY env var");
            //     } else {
            //         LOG_WARN("HYPERLIQUID_WALLET_KEY not set — order submission will fail");
            //     }
            // }

            broker = hl_broker;
            break;
        }

        case BrokerId::ALPACA:
            LOG_ERROR("Alpaca broker not yet implemented");
            return nullptr;

        case BrokerId::SIMULATED:
            LOG_ERROR("Simulated broker not yet implemented");
            return nullptr;

        default:
            LOG_ERROR("Unknown broker ID: {}", static_cast<uint8_t>(broker_id));
            return nullptr;
    }

    LOG_INFO("Created {} broker", broker_name);
    return broker;
}

// BrokerConfig BrokerFactory::parse_broker_config(const nlohmann::json& json) {
//     BrokerConfig config;

//     // Parse broker identification
//     config.broker_name = json.value("name", "unknown");
//     config.broker_id = parse_broker_id(config.broker_name);
//     config.enabled = json.value("enabled", true);

//     // Parse MD connector config
//     if (json.contains("market_data")) {
//         const auto& md_json = json["market_data"];
//         config.md_config.enabled = md_json.value("enabled", true);
//         config.md_config.websocket_url = md_json.value("websocket_url", "");

//         // Parse subscriptions
//         if (md_json.contains("subscriptions")) {
//             for (const auto& sub_json : md_json["subscriptions"]) {
//                 SubscriptionConfig sub;
//                 sub.type = parse_subscription_type(sub_json.value("type", "trade"));

//                 if (sub_json.contains("instruments") && sub_json["instruments"].is_array()) {
//                     sub.instruments = sub_json["instruments"].get<std::vector<std::string>>();
//                 }

//                 sub.depth = sub_json.value("depth", 10);

//                 config.md_config.subscriptions.push_back(sub);
//             }
//         }
//     }

//     // Parse Order connector config
//     if (json.contains("order")) {
//         const auto& order_json = json["order"];
//         config.order_config.enabled = order_json.value("enabled", true);
//         config.order_config.rest_url = order_json.value("rest_url", "");
//         config.order_config.websocket_url = order_json.value("websocket_url", "");
//         // Note: API credentials handled via environment variables for Coinbase
//     }

//     // Parse custom parameters
//     if (json.contains("custom_params")) {
//         const auto& custom = json["custom_params"];
//         for (auto it = custom.begin(); it != custom.end(); ++it) {
//             if (it.value().is_string()) {
//                 config.custom_params[it.key()] = it.value().get<std::string>();
//             }
//         }
//     }

//     return config;
// }

BrokerId BrokerFactory::parse_broker_id(const std::string& broker_name) {
    std::string lower_name = broker_name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

    if (lower_name == "coinbase") return BrokerId::COINBASE;
    if (lower_name == "hyperliquid") return BrokerId::HYPERLIQUID;
    if (lower_name == "alpaca") return BrokerId::ALPACA;
    if (lower_name == "simulated" || lower_name == "sim") return BrokerId::SIMULATED;

    LOG_WARN("Unknown broker name: {}, defaulting to UNKNOWN", broker_name);
    return BrokerId::UNKNOWN;
}

SubscriptionType BrokerFactory::parse_subscription_type(const std::string& type_str) {
    std::string lower_type = type_str;
    std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(), ::tolower);

    if (lower_type == "trade" || lower_type == "trades") return SubscriptionType::TRADE;
    if (lower_type == "quote" || lower_type == "quotes" || lower_type == "ticker") return SubscriptionType::QUOTE;
    if (lower_type == "orderbook" || lower_type == "order_book" || lower_type == "level2") return SubscriptionType::LEVEL2;

    LOG_WARN("Unknown subscription type: {}, defaulting to TRADE", type_str);
    return SubscriptionType::TRADE;
}

} // namespace slick::trader
