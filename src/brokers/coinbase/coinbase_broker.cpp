#include <slick/logger.hpp>
#include <slick-trader/brokers/coinbase/coinbase_broker.hpp>
#include <slick-trader/core/broker_registry.hpp>

namespace {
    json empty_json;
}

namespace slick::trader {

CoinbaseBroker::CoinbaseBroker(
    const json& config,
    slick::stream_buffer_multiplexer &event_mux,
    slick::queue<uint8_t>& control_queue,
    slick::queue<uint8_t>& md_request_queue,
    slick::queue<uint8_t>& order_request_queue
)
    : md_connector_(config.contains("market_data") ? config["market_data"] : empty_json, event_mux, control_queue, md_request_queue)
    , order_connector_(config.contains("order") ? config["order"] : empty_json, event_mux, control_queue, order_request_queue)
    , control_queue_(control_queue)
{
}

CoinbaseBroker::~CoinbaseBroker() {
    stop();
}

void CoinbaseBroker::register_broker() {
    md_connector_.register_md_connector();
    order_connector_.register_order_connector();
}

bool CoinbaseBroker::start() {
    md_connector_.start();
    order_connector_.start();
    return true;
}

void CoinbaseBroker::stop() {
    LOG_INFO("Shutting down Coinbase broker");
    
    md_connector_.stop();
    order_connector_.stop();
    
    LOG_INFO("Coinbase broker shutdown complete");
}

FeeStructure CoinbaseBroker::get_fees(std::string_view) const {
    if (!fees_fetched_) {
        double rate = order_connector_.get_taker_rate();
        cached_fees_ = FeeStructure{FeeType::PERCENTAGE, rate};
        LOG_INFO("CoinbaseBroker: taker fee rate = {:.4f}", rate);
        fees_fetched_ = true;
    }
    return cached_fees_;
}

SLICK_TRADER_REGISTER_BROKER("coinbase", CoinbaseBroker);

} // namespace slick::trader
