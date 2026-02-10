#include <slick/logger.hpp>
#include <slick-trader/brokers/coinbase/coinbase_broker.h>
#include <slick-trader/brokers/coinbase/coinbase_md_connector.h>
#include <slick-trader/brokers/coinbase/coinbase_order_connector.h>
#include <nlohmann/json.hpp>

namespace slick::trader {

CoinbaseBroker::CoinbaseBroker()
    : md_connector_(std::make_shared<CoinbaseMDConnector>())
    , order_connector_(std::make_shared<CoinbaseOrderConnector>()) {
}

CoinbaseBroker::~CoinbaseBroker() {
    shutdown();
}

bool CoinbaseBroker::initialize(const std::string& config_json) {
    LOG_INFO("Initializing Coinbase broker");

    try {
        auto md_connector = std::dynamic_pointer_cast<CoinbaseMDConnector>(md_connector_);
        auto order_connector = std::dynamic_pointer_cast<CoinbaseOrderConnector>(order_connector_);
        if (!md_connector || !order_connector) {
            LOG_ERROR("Failed to initialize Coinbase broker: connector type mismatch");
            return false;
        }

        auto config = nlohmann::json::parse(config_json);

        std::string md_ws_url = config.value("md_websocket_url", "wss://advanced-trade-ws.coinbase.com");
        std::string order_ws_url = config.value("order_websocket_url", "wss://advanced-trade-ws-user.coinbase.com");
        std::string rest_url = config.value("rest_url", "https://api.coinbase.com");
        std::string api_key = config.value("api_key", "");
        std::string api_secret = config.value("api_secret", "");

        // Configure MD connector
        md_connector->set_websocket_url(md_ws_url);

        // Configure order connector
        order_connector->set_websocket_url(order_ws_url);
        order_connector->set_rest_url(rest_url);
        order_connector->set_api_key(api_key);
        order_connector->set_api_secret(api_secret);

        LOG_INFO("Coinbase broker initialized - MD WS: {}, Order WS: {}, REST: {}",
                 md_ws_url, order_ws_url, rest_url);
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR("Failed to initialize Coinbase broker: {}", e.what());
        return false;
    }
}

void CoinbaseBroker::shutdown() {
    LOG_INFO("Shutting down Coinbase broker");

    if (md_connector_) {
        md_connector_->stop();
        md_connector_->disconnect();
    }

    if (order_connector_) {
        order_connector_->stop();
        order_connector_->disconnect();
    }

    LOG_INFO("Coinbase broker shutdown complete");
}

} // namespace slick::trader
