#pragma once

#include <slick-trader/brokers/ibroker.hpp>
#include <slick-trader/brokers/coinbase/coinbase_md_connector.hpp>
#include <slick-trader/brokers/coinbase/coinbase_order_connector.hpp>
#include <nlohmann/json.hpp>
#include <memory>
#include <string>

namespace slick::trader {

using json = nlohmann::json;

class CoinbaseBroker : public IBroker {
public:
    CoinbaseBroker(
        const json& config,
        slick::stream_buffer_multiplexer &event_mux,
        slick::queue<uint8_t>& control_queue,
        slick::queue<uint8_t>& md_request_queue,
        slick::queue<uint8_t>& order_request_queue
    );
    ~CoinbaseBroker() override;

    // IBroker interface
    BrokerId get_broker_id() const override { return BrokerId::COINBASE; }
    const char* get_broker_name() const override { return "Coinbase"; }

    void register_broker() override;

    bool start() override;
    void stop() override;

    // std::shared_ptr<IMarketDataConnector> get_market_data_connector() override {
    //     return md_connector_;
    // }

    // std::shared_ptr<IOrderConnector> get_order_connector() override {
    //     return order_connector_;
    // }

    FeeStructure get_fees(std::string_view symbol) const override;

private:
    CoinbaseMDConnector md_connector_;
    CoinbaseOrderConnector order_connector_;

    slick::queue<uint8_t>& control_queue_;

    mutable FeeStructure cached_fees_;
    mutable bool fees_fetched_{false};
};

} // namespace slick::trader
