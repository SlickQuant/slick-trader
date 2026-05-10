#pragma once

#include <slick-trader/brokers/ibroker.hpp>
#include <memory>
#include <string>

namespace slick::trader {

class CoinbaseBroker : public IBroker {
public:
    CoinbaseBroker();
    ~CoinbaseBroker() override;

    // IBroker interface
    BrokerId get_broker_id() const override { return BrokerId::COINBASE; }
    const char* get_broker_name() const override { return "Coinbase"; }
    bool initialize(const std::string& config_json) override;
    void shutdown() override;

    std::shared_ptr<IMarketDataConnector> get_market_data_connector() override {
        return md_connector_;
    }

    std::shared_ptr<IOrderConnector> get_order_connector() override {
        return order_connector_;
    }

    FeeStructure get_fees(std::string_view symbol) const override;

private:
    std::shared_ptr<IMarketDataConnector> md_connector_;
    std::shared_ptr<IOrderConnector> order_connector_;

    mutable FeeStructure cached_fees_;
    mutable bool fees_fetched_{false};
};

} // namespace slick::trader
