#pragma once

#include <slick-trader/brokers/ibroker.hpp>
#include <slick-trader/strategy/istrategy.hpp>
#include <slick/queue.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <atomic>

namespace slick::trader {

// Forward declarations
class StrategyManager;
class PositionTracker;
class PnLCalculator;
class RiskManager;

class TradingEngine {
public:
    static TradingEngine& instance() {
        static TradingEngine instance;
        return instance;
    }

    // Lifecycle
    bool initialize(const std::string& config_path);
    void start();
    void stop();
    void shutdown();

    // Broker management
    bool register_broker(std::shared_ptr<IBroker> broker);
    std::shared_ptr<IBroker> get_broker(BrokerId broker_id);

    // Strategy management
    void add_strategy(std::shared_ptr<IStrategy> strategy);

    // Position & PnL access
    PositionTracker& get_position_tracker();
    PnLCalculator& get_pnl_calculator();
    RiskManager& get_risk_manager();

    // Queue access
    std::shared_ptr<slick::SlickQueue<uint8_t>> get_md_queue() { return md_queue_; }
    std::shared_ptr<slick::SlickQueue<uint8_t>> get_order_request_queue() { return order_request_queue_; }
    std::shared_ptr<slick::SlickQueue<uint8_t>> get_order_event_queue() { return order_event_queue_; }

    bool is_running() const { return running_.load(std::memory_order_relaxed); }

private:
    TradingEngine();
    ~TradingEngine();

    TradingEngine(const TradingEngine&) = delete;
    TradingEngine(TradingEngine&&) = delete;
    TradingEngine& operator=(const TradingEngine&) = delete;
    TradingEngine& operator=(TradingEngine&&) = delete;

    std::atomic_bool running_;

    // Brokers
    std::unordered_map<BrokerId, std::shared_ptr<IBroker>> brokers_;

    // Strategy manager
    std::unique_ptr<StrategyManager> strategy_manager_;

    // Position & PnL tracking
    std::unique_ptr<PositionTracker> position_tracker_;
    std::unique_ptr<PnLCalculator> pnl_calculator_;
    std::unique_ptr<RiskManager> risk_manager_;

    // Message queues
    std::shared_ptr<slick::SlickQueue<uint8_t>> md_queue_;
    std::shared_ptr<slick::SlickQueue<uint8_t>> order_request_queue_;
    std::shared_ptr<slick::SlickQueue<uint8_t>> order_event_queue_;
};

} // namespace slick::trader