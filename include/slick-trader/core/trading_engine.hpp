#pragma once

#include <slick-trader/core/position_tracker.hpp>
#include <slick-trader/core/pnl_calculator.hpp>
#include <slick-trader/core/risk_manager.hpp>
#include <slick-trader/core/strategy_manager.hpp>
#include <slick/stream_buffer_multiplexer.hpp>
#include <slick-trader/brokers/ibroker.hpp>
#include <slick-trader/strategy/istrategy.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <atomic>
#include <nlohmann/json.hpp>
#include <chrono>
#include <vector>

namespace slick::trader {

class WebServer;

using json = nlohmann::json;

class TradingEngine {
public:
    static TradingEngine& instance() {
        static TradingEngine instance;
        return instance;
    }

    // Lifecycle
    bool initialize(const std::string& config_path);
    
    // Lifecycle
    void start();
    void shutdown();
    
    bool is_running() const { return running_.load(std::memory_order_relaxed); }

private:
    TradingEngine();
    ~TradingEngine();

    TradingEngine(const TradingEngine&) = delete;
    TradingEngine(TradingEngine&&) = delete;
    TradingEngine& operator=(const TradingEngine&) = delete;
    TradingEngine& operator=(TradingEngine&&) = delete;

    void publish_regiter_end(uint32_t num_producers);
    bool wait_for_brokers_ready(std::chrono::seconds timeout = std::chrono::seconds(30));

    std::atomic_bool running_{false};

    // Message queues
    std::unique_ptr<slick::stream_buffer_multiplexer> event_mux_;
    std::unique_ptr<slick::queue<uint8_t>> control_queue_;
    std::unique_ptr<slick::queue<uint8_t>> md_request_queue_;
    std::unique_ptr<slick::queue<uint8_t>> order_request_queue_;

    std::vector<std::shared_ptr<IBroker>> brokers_;

    // Positions on disk, when `position_store` is configured. Declared before the tracker
    // that writes to it, so it outlives it.
    std::unique_ptr<PositionStore> position_store_;
    PositionTracker position_tracker_;
    PnLCalculator pnl_calculator_;
    RiskManager risk_manager_;

    std::unique_ptr<StrategyManager> strategy_manager_;
    std::unique_ptr<WebServer> web_server_;
};

} // namespace slick::trader