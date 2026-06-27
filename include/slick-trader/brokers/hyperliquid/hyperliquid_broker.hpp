#pragma once

#include <slick-trader/brokers/ibroker.hpp>
#include <slick-trader/brokers/hyperliquid/hyperliquid_md_connector.hpp>
#include <slick-trader/brokers/hyperliquid/hyperliquid_order_connector.hpp>
#include <memory>
#include <string>

namespace slick::trader {

class HyperliquidBroker : public IBroker {
public:
    HyperliquidBroker(
        const json& config,
        slick::stream_buffer_multiplexer &event_mux,
        slick::queue<uint8_t>& control_queue,
        slick::queue<uint8_t>& md_request_queue,
        slick::queue<uint8_t>& order_request_queue
    );
    ~HyperliquidBroker() override;

    BrokerId get_broker_id() const override { return BrokerId::HYPERLIQUID; }
    const char* get_broker_name() const override { return "Hyperliquid"; }

    void register_broker() override;

    bool start() override;
    void stop() override;

    // bool initialize(const std::string& config_json) override;
    // void shutdown() override;

    // std::shared_ptr<IMarketDataConnector> get_market_data_connector() override;
    // std::shared_ptr<IOrderConnector> get_order_connector() override;
    FeeStructure get_fees(std::string_view symbol) const override;

    // Configuration — called by BrokerFactory before start()
    // void set_md_api_url(const std::string& url);
    // void set_order_api_url(const std::string& url);
    // void set_private_key(const std::string& key);

private:
    HyperliquidMDConnector md_connector_;
    HyperliquidOrderConnector order_connector_;
};

} // namespace slick::trader
