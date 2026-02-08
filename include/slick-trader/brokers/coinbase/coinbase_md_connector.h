#pragma once

#include <slick-trader/brokers/imarket_data_connector.h>
#include <slick-trader/brokers/coinbase/coinbase_md_callbacks.h>
#include <slick/queue.h>
#include <memory>
#include <thread>
#include <atomic>
#include <string>
#include <vector>

// Forward declarations
namespace coinbase {
    class WebSocketClient;
}

namespace slick::trader {

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
    bool is_running() const override { return is_connected(); }

    void set_md_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) override {
        md_queue_ = queue;
        callbacks_.set_md_queue(queue);
    }

    // Configuration
    void set_websocket_url(const std::string& url) { ws_url_ = url; }

private:
    CoinbaseMDCallbacks callbacks_;
    std::unique_ptr<coinbase::WebSocketClient> ws_client_;
    std::shared_ptr<slick::SlickQueue<uint8_t>> md_queue_;

    std::string ws_url_;

    std::vector<std::string> trade_instruments_;
    std::vector<std::string> quote_instruments_;
    std::vector<std::string> orderbook_instruments_;
    int orderbook_depth_;
};

} // namespace slick::trader
