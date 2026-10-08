#pragma once

#include <slick-trader/strategy/istrategy.hpp>
#include <slick-trader/strategy/strategy_base.hpp>
#include <slick-trader/events/event.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick-trader/events/level2_update_event.hpp>
#include <slick-trader/events/quote_event.hpp>
#include <slick-trader/events/strategy_config_event.hpp>
#include <slick-trader/events/instrument_definition_event.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/market_data/order_book.hpp>
#include <slick-trader/core/strategy_loader.hpp>
#include <slick-trader/core/broker_status.hpp>
#include <slick/queue.hpp>
#include <nlohmann/json.hpp>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <chrono>

namespace slick::trader {

// Forward declarations
class PositionTracker;
class PnLCalculator;
class RiskManager;
class StrategyManager;
using json = nlohmann::json;
using multiplex_record = slick::stream_buffer_multiplexer::multiplex_record;
using producer_buffer = slick::stream_buffer_multiplexer::producer_buffer;

struct OrderBookObserverImpl : public OrderBookObserver {
    OrderBookObserverImpl(StrategyManager* manager, slick::orderbook::OrderBookL2& order_book, const std::string& instrument) 
        : manager_(manager), order_book_(order_book), instrument_(instrument)
    {}
    ~OrderBookObserverImpl() override = default;
    
    void onPriceLevelUpdate(const slick::orderbook::PriceLevelUpdate& update) override;
    void onTopOfBookUpdate(const slick::orderbook::TopOfBook& tob) override;

private:
    StrategyManager* manager_;
    slick::orderbook::OrderBookL2& order_book_;
    std::string instrument_;
};

// Strategy Manager
// Manages multiple strategies, dispatches events to them
// Runs on its own thread, consuming from MD and order event queues
class StrategyManager {
public:
    StrategyManager(
        const json& config,
        PositionTracker& position_tracker,
        PnLCalculator& pnl_calculator,
        RiskManager& risk_manager,
        slick::stream_buffer_multiplexer& event_mux,
        slick::queue<uint8_t>& md_request_queue,
        slick::queue<uint8_t>& order_request_queue
    );

    ~StrategyManager() {
        stop();
    }

    void load_strategies(const json& config);

    // Strategy registration
    void add_strategy(std::shared_ptr<IStrategy> strategy);

    void start();
    void stop();

    void shutdown() {
        LOG_INFO("StrategyManager: Shutting down");

        for (auto& strategy : strategies_) {
            if (strategy) {
                strategy->shutdown();
            }
        }
    }

    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

    // Get all strategies (for subscription collection)
    const std::vector<std::shared_ptr<IStrategy>>& get_all_strategies() const {
        return strategies_;
    }

    std::shared_ptr<IStrategy> get_strategy(const std::string& strategy_id) const {
        auto it = strategy_map_.find(strategy_id);
        if (it != strategy_map_.end()) {
            return it->second;
        }
        return nullptr;
    }

    // Manual strategy control — clears auto-pause state so MD reconnect won't override
    bool start_strategy(const std::string& strategy_id);
    bool stop_strategy(const std::string& strategy_id);

    bool is_auto_paused(const std::string& strategy_id) const {
        return auto_paused_strategies_.count(strategy_id) > 0;
    }

    const std::string& get_auto_pause_reason(const std::string& strategy_id) const {
        static const std::string empty;
        auto it = auto_pause_reasons_.find(strategy_id);
        return it != auto_pause_reasons_.end() ? it->second : empty;
    }

    void subscribe_book_update(std::vector<std::string>& instruments) {
        for (const auto& instrument : instruments) {
            auto *book = book_manager_.getOrCreateOrderBook(get_or_create_symbol_id(instrument));
            auto observer = std::make_shared<OrderBookObserverImpl>(this, *book, instrument);
            book->addObserver(observer);
        }
    }

private:
    friend struct OrderBookObserverImpl;

    void run();

    void subscribe(const SubscriptionRequest& sub);
    void unsubscribe(const SubscriptionRequest& sub);

    void check_loss_limits();
    void check_broker_status();
    void dispatch_strategy_config(const StrategyConfigEvent& event);

    void process_event(const Event* event);

    void dispatch_trade(const TradeEvent& event);
    void dispatch_quote(const QuoteEvent& event);
    void dispatch_level2_updates(const Level2UpdateEvent& event);

    void on_top_of_book_update(const std::string &instrument, const slick::orderbook::TopOfBook& tob);
    void on_order_book_update(const slick::orderbook::OrderBookL2& book);
    void dispatch_order_book(const OrderBookEvent& event);

    slick::orderbook::SymbolId get_or_create_symbol_id(const std::string& instrument);

    void dispatch_order_ack(const OrderAckEvent& event);
    void dispatch_order_fill(const OrderFillEvent& event);
    void dispatch_order_reject(const OrderRejectEvent& event);
    void dispatch_order_cancel(const OrderCancelEvent& event);
    void dispatch_instrument_definition(const InstrumentDefinitionEvent& event);

private:
    std::vector<std::shared_ptr<IStrategy>> strategies_;
    std::unordered_map<std::string, std::shared_ptr<IStrategy>> strategy_map_;
    slick::orderbook::OrderBookManager<slick::orderbook::OrderBookL2> book_manager_;
    std::unordered_map<std::string, slick::orderbook::SymbolId> symbol_ids_;
    std::unordered_map<slick::orderbook::SymbolId, std::string> symbol_names_;
    std::unordered_map<std::string, uint8_t> subscriptions_;
    std::unordered_map<std::string, std::set<BrokerId>> strategy_md_brokers_;
    std::unordered_set<std::string> auto_paused_strategies_;
    std::unordered_map<std::string, std::string> auto_pause_reasons_;
    slick::orderbook::SymbolId next_symbol_id_{0};

    // Position & PnL tracking (non-owning pointers)
    PositionTracker& position_tracker_;
    PnLCalculator& pnl_calculator_;
    RiskManager& risk_manager_;

    slick::stream_buffer_multiplexer& event_mux_;
    slick::queue<uint8_t>& md_request_queue_;
    slick::queue<uint8_t>& order_request_queue_;
    std::shared_ptr<producer_buffer> tob_producer_buffer_;

    std::string strategy_plugins_folder_;
    std::string strategy_configs_folder_;
    StrategyLoader loader_;

    std::atomic_bool running_{false};
    std::thread worker_thread_;
    uint64_t event_cursor_{0};
};

inline void OrderBookObserverImpl::onPriceLevelUpdate(const slick::orderbook::PriceLevelUpdate& update) {
    manager_->on_order_book_update(order_book_);
}

inline void OrderBookObserverImpl::onTopOfBookUpdate(const slick::orderbook::TopOfBook& tob) {
    manager_->on_top_of_book_update(instrument_, tob);
}

} // namespace slick::trader
