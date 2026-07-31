#pragma once

#include <slick-trader/brokers/iorder_connector.hpp>
#include <slick/stream_buffer_multiplexer.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace hyperliquid {
class Info;
class Exchange;
}

namespace slick::trader {

struct NewOrderEvent;
struct ModifyOrderEvent;
struct CancelOrderEvent;
using producer_buffer = slick::stream_buffer_multiplexer::producer_buffer;

class HyperliquidOrderConnector : public IOrderConnector {
public:
    HyperliquidOrderConnector(
        const json& config,
        slick::stream_buffer_multiplexer &mux,
        slick::queue<uint8_t>& control_queue,
        slick::queue<uint8_t>& order_queue
    );
    ~HyperliquidOrderConnector() override;

    void register_order_connector() override;

    bool connect() override;
    void disconnect() override;
    bool is_connected() const override { return connected_.load(std::memory_order_relaxed); }

    void start() override;
    void stop() override;
    bool is_running() const override { return running_.load(std::memory_order_relaxed); }

private:
    void run();
    void process_order_requests();
    void handle_new_order(const NewOrderEvent& req);
    void handle_modify_order(const ModifyOrderEvent& req);
    void handle_cancel_order(const CancelOrderEvent& req);

    void publish_reject(const NewOrderEvent& req, const std::string& reason);
    void on_user_fill(const nlohmann::json& fill);
    void on_order_update(const nlohmann::json& update);

    std::atomic_bool running_{false};
    std::atomic_bool connected_{false};

    bool enabled_{true};

    std::shared_ptr<hyperliquid::Info> info_;
    std::unique_ptr<hyperliquid::Exchange> exchange_;
    std::thread worker_thread_;

    std::string shm_name_;

    std::shared_ptr<producer_buffer> event_buffer_;
    slick::queue<uint8_t>& control_queue_;
    slick::queue<uint8_t>& request_queue_;
    uint64_t request_cursor_;

    std::string api_url_{"https://api.hyperliquid.xyz"};
    std::string private_key_;
    std::string wallet_address_;

    std::atomic<uint64_t> cloid_counter_{0};

    mutable std::mutex order_map_mutex_;
    std::unordered_map<std::string, std::string> cloid_to_client_id_;  // cloid_raw → client_order_id
    std::unordered_map<int64_t, std::string> oid_to_client_id_;        // oid → client_order_id
    std::unordered_map<int64_t, std::string> oid_to_instrument_;       // oid → instrument
    std::unordered_set<uint64_t> seen_fill_tids_;                      // dedup by trade id
};

} // namespace slick::trader
