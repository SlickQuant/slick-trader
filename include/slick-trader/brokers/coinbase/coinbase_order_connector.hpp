#pragma once

#include <slick-trader/brokers/iorder_connector.hpp>
#include <slick/stream_buffer_multiplexer.hpp>
#include <cstdint>
#include <memory>
#include <thread>
#include <atomic>
#include <string>
#include <nlohmann/json.hpp>

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

using producer_buffer = slick::stream_buffer_multiplexer::producer_buffer;
using json = nlohmann::json;

class CoinbaseOrderConnector : public IOrderConnector {
public:
    CoinbaseOrderConnector(
        const json& config,
        slick::stream_buffer_multiplexer &mux,
        slick::queue<uint8_t>& control_queue,
        slick::queue<uint8_t>& request_queue
    );
    ~CoinbaseOrderConnector() override;

    void register_order_connector() override;

    // IOrderConnector interface
    bool connect() override;
    void disconnect() override;
    bool is_connected() const override;

    void start() override;
    void stop() override;
    bool is_running() const override { return running_.load(std::memory_order_relaxed); }

    // Fetch taker fee rate from Coinbase REST API (e.g. 0.006 = 0.6%)?
    double get_taker_rate() const;

    std::shared_ptr<producer_buffer> event_buffer() noexcept {
        return event_buffer_;
    }

private:
    void run();
    void process_order_requests();
    void handle_order_request(const NewOrderEvent& request);
    void handle_modify_request(const ModifyOrderEvent& request);
    void handle_cancel_request(const CancelOrderEvent& request);

private:
    std::atomic_bool running_{false};
    bool enabled_{true};
    std::thread worker_thread_;

    std::unique_ptr<CoinbaseOrderCallbacks> callbacks_;
    std::unique_ptr<coinbase::WebSocketClient> ws_client_;
    std::unique_ptr<coinbase::CoinbaseRestClient> rest_client_;

    std::string shm_name_;

    std::shared_ptr<producer_buffer> event_buffer_;
    slick::queue<uint8_t>& control_queue_;
    slick::queue<uint8_t>& request_queue_;
    uint64_t request_cursor_;
};

} // namespace slick::trader
