#pragma once

#include <slick-trader/brokers/imarket_data_connector.hpp>
#include <slick/stream_buffer_multiplexer.hpp>
#include <slick-trader/events/md_subscription.hpp>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

// Forward declarations
namespace coinbase {
    class WebSocketClient;
}

namespace slick::trader {

using producer_buffer = slick::stream_buffer_multiplexer::producer_buffer;
using json = nlohmann::json;

class CoinbaseMDCallbacks;

class CoinbaseMDConnector : public IMarketDataConnector {
public:
    CoinbaseMDConnector(
        const json& config,
        slick::stream_buffer_multiplexer &event_mux,
        slick::queue<uint8_t>& control_queue,
        slick::queue<uint8_t>& request_queue
    );

    ~CoinbaseMDConnector() override;

    void register_md_connector() override;

    // IMarketDataConnector interface
    bool connect() override;
    void disconnect() override;
    bool is_connected() const override;

    bool subscribe(const MDSubscription& request) override;
    bool unsubscribe(const MDSubscription& request) override;

    void start() override;
    void stop() override;
    bool is_running() const override { return running_.load(std::memory_order_relaxed); }

    std::string_view ws_url() const noexcept { return ws_url_; }

private:
    void run();
    void process_md_requests();

    std::atomic_bool running_{false};
    bool enabled_{true};
    std::thread worker_thread_;

    std::unique_ptr<CoinbaseMDCallbacks> callbacks_;
    std::unique_ptr<coinbase::WebSocketClient> ws_client_;

    std::shared_ptr<producer_buffer> event_buffer_;
    slick::queue<uint8_t>& control_queue_;
    slick::queue<uint8_t>& request_queue_;
    uint64_t request_cursor_;

    std::string ws_url_;

    std::vector<std::string> trade_instruments_;
    std::vector<std::string> quote_instruments_;
    std::vector<std::string> level2_instruments_;
    std::vector<std::string> orderbook_instruments_;
    int orderbook_depth_;
};

} // namespace slick::trader
