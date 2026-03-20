#pragma once

#include <slick-trader/brokers/iorder_connector.hpp>
#include <cstdint>
#include <memory>
#include <thread>
#include <atomic>
#include <string>

// Forward declarations
namespace coinbase {
    class WebSocketClient;
    class CoinbaseRestClient;
}

namespace slick::trader {

struct NewOrderEvent;
struct ModifyOrderEvent;
struct CancelOrderEvent;
class CoinbaseOrderCallbacks;

class CoinbaseOrderConnector : public IOrderConnector {
public:
    CoinbaseOrderConnector();
    ~CoinbaseOrderConnector() override;

    // IOrderConnector interface
    bool connect() override;
    void disconnect() override;
    bool is_connected() const override;

    void start() override;
    void stop() override;
    bool is_running() const override { return running_.load(std::memory_order_relaxed); }

    void set_order_submission_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) override {
        order_submission_queue_ = queue;
    }

    void set_order_event_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) override;

    // Configuration
    void set_rest_url(const std::string& url);
    void set_websocket_url(const std::string& url) { ws_url_ = url; }
    void set_api_key(const std::string& key) { api_key_ = key; }
    void set_api_secret(const std::string& secret) { api_secret_ = secret; }

private:
    void run();
    void process_order_requests();
    void handle_order_request(const NewOrderEvent& request);
    void handle_modify_request(const ModifyOrderEvent& request);
    void handle_cancel_request(const CancelOrderEvent& request);

private:
    std::atomic_bool running_;
    std::atomic_bool connected_;
    std::thread worker_thread_;

    std::unique_ptr<CoinbaseOrderCallbacks> callbacks_;
    std::unique_ptr<coinbase::WebSocketClient> ws_client_;
    std::unique_ptr<coinbase::CoinbaseRestClient> rest_client_;

    std::shared_ptr<slick::SlickQueue<uint8_t>> order_submission_queue_;
    std::shared_ptr<slick::SlickQueue<uint8_t>> order_event_queue_;
    uint64_t order_cursor_;

    std::string rest_url_;
    std::string ws_url_;
    std::string api_key_;
    std::string api_secret_;
};

} // namespace slick::trader
