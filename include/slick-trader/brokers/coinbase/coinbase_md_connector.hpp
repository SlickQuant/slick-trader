#pragma once

#include <slick-trader/brokers/imarket_data_connector.hpp>
#include <memory>
#include <string>
#include <vector>

// Forward declarations
namespace coinbase {
    class WebSocketClient;
}

namespace slick::trader {

class CoinbaseMDCallbacks;

class CoinbaseMDConnector : public IMarketDataConnector {
public:
    CoinbaseMDConnector();
    ~CoinbaseMDConnector() override;

    // IMarketDataConnector interface
    bool connect() override;
    void disconnect() override;
    bool is_connected() const override;

    bool subscribe(SubscriptionType type, const std::vector<std::string>& instruments, int depth = 10) override;
    bool unsubscribe(SubscriptionType type, const std::vector<std::string>& instruments) override;

    void start() override;
    void stop() override;
    bool is_running() const override { return running_.load(std::memory_order_relaxed); }

    void set_md_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) override;

    // Configuration
    void set_websocket_url(const std::string& url) { ws_url_ = url; }

private:
    std::atomic_bool running_;
    
    std::unique_ptr<CoinbaseMDCallbacks> callbacks_;
    std::unique_ptr<coinbase::WebSocketClient> ws_client_;
    std::shared_ptr<slick::SlickQueue<uint8_t>> md_queue_;

    std::string ws_url_;

    std::vector<std::string> trade_instruments_;
    std::vector<std::string> quote_instruments_;
    std::vector<std::string> level2_instruments_;
    std::vector<std::string> orderbook_instruments_;
    int orderbook_depth_;
};

} // namespace slick::trader
