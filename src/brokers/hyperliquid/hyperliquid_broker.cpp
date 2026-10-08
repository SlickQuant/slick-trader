#include <slick/logger.hpp>
#include <slick-trader/brokers/hyperliquid/hyperliquid_broker.hpp>
#include <slick-trader/core/broker_registry.hpp>

namespace {
    nlohmann::json empty_json;
}

namespace slick::trader {

HyperliquidBroker::HyperliquidBroker(
    const json& config,
    slick::stream_buffer_multiplexer &event_mux,
    slick::queue<uint8_t>& control_queue,
    slick::queue<uint8_t>& md_request_queue,
    slick::queue<uint8_t>& order_request_queue
)
    : md_connector_(config.contains("market_data") ? config["market_data"] : empty_json, event_mux, control_queue, md_request_queue)
    , order_connector_(config.contains("order") ? config["order"] : empty_json, event_mux, control_queue, order_request_queue)
{
}

HyperliquidBroker::~HyperliquidBroker() {
    stop();
}

void HyperliquidBroker::register_broker() {
    md_connector_.register_md_connector();
    order_connector_.register_order_connector();
}

bool HyperliquidBroker::start() {
    md_connector_.start();
    order_connector_.start();
    return true;
}

void HyperliquidBroker::stop() {
    LOG_INFO("Shutting down Hyperliquid broker");
    
    md_connector_.stop();
    order_connector_.stop();
    
    LOG_INFO("Hyperliquid broker shutdown complete");
}

FeeStructure HyperliquidBroker::get_fees(std::string_view /*symbol*/) const {
    // Hyperliquid default taker fee: 0.05%
    return FeeStructure{FeeType::PERCENTAGE, 0.0005};
}

SLICK_TRADER_REGISTER_BROKER("hyperliquid", HyperliquidBroker);

} // namespace slick::trader
