#pragma once

#include <slick-trader/brokers/imarket_data_connector.hpp>
#include <slick/stream_buffer_multiplexer.hpp>
#include <slick-trader/events/md_subscription.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <set>

namespace hyperliquid { class Info; }

namespace slick::trader {

using producer_buffer = slick::stream_buffer_multiplexer::producer_buffer;

class HyperliquidMDConnector : public IMarketDataConnector {
public:
    HyperliquidMDConnector(
        const json& config,
        slick::stream_buffer_multiplexer &mux,
        slick::queue<uint8_t>& control_queue,
        slick::queue<uint8_t>& request_queue
    );
    ~HyperliquidMDConnector() override;

    void register_md_connector() override;

    bool connect() override;
    void disconnect() override;
    bool is_connected() const override { return connected_.load(std::memory_order_relaxed); }

    bool subscribe(const MDSubscription& request) override;
    bool unsubscribe(const MDSubscription& request) override;

    void start() override;
    void stop() override;
    bool is_running() const override { return running_.load(std::memory_order_relaxed); }

private:
    void run();
    void process_md_requests();
    void subscribe_all();
    void on_trades(const nlohmann::json& msg);
    void on_bbo(const nlohmann::json& msg);
    void on_l2(const nlohmann::json& msg);
    void on_l2book(const nlohmann::json& msg);

    std::atomic_bool running_{false};
    std::atomic_bool connected_{false};
    std::thread worker_thread_;

    bool enabled_{true};
    std::string shm_name_;

    std::unique_ptr<hyperliquid::Info> info_;

    std::shared_ptr<producer_buffer> event_buffer_;
    slick::queue<uint8_t>& control_queue_;
    slick::queue<uint8_t>& request_queue_;
    uint64_t request_cursor_;

    std::string api_url_{"https://api.hyperliquid.xyz"};

    std::vector<std::string> trade_instruments_;
    std::vector<std::string> quote_instruments_;
    std::vector<std::string> level2_instruments_;
    int level2_depth_{10};

    // subscription_ids returned by info_->subscribe(), for cleanup on reconnect
    std::vector<std::pair<std::string, int>> sub_ids_;  // channel_json_str → id

    std::unordered_map<std::string, std::string> coin_to_name_;

    std::set<price_t, std::greater<price_t>> bid_prices_;
    std::set<price_t> ask_prices_;
};

} // namespace slick::trader
