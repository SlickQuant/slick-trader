#pragma once

#include <slick-trader/strategy/istrategy.hpp>
#include <slick-trader/events/event.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick-trader/market_data/order_book.hpp>
#include <slick/queue.h>
#include <slick/logger.hpp>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <unordered_map>

namespace slick::trader {

// Forward declarations
class PositionTracker;
class PnLCalculator;
class RiskManager;

// Strategy Manager
// Manages multiple strategies, dispatches events to them
// Runs on its own thread, consuming from MD and order event queues
class StrategyManager {
public:
    StrategyManager()
        : running_(false)
        , md_cursor_(0)
        , order_cursor_(0)
        , position_tracker_(nullptr)
        , pnl_calculator_(nullptr)
        , risk_manager_(nullptr) {}

    ~StrategyManager() {
        stop();
    }

    // Strategy registration
    void add_strategy(std::shared_ptr<IStrategy> strategy) {
        strategies_.push_back(strategy);
        LOG_INFO("StrategyManager: Added strategy '{}'", strategy->name());
    }

    // Queue setup
    void set_market_data_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) {
        md_queue_ = queue;
    }

    void set_order_event_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) {
        order_event_queue_ = queue;
    }

    void set_order_submission_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) {
        order_submission_queue_ = queue;

        // Set order submission queue for all strategies
        for (auto& strategy : strategies_) {
            strategy->set_order_submission_queue(queue);
        }
    }

    // Position & PnL tracking
    void set_position_tracker(PositionTracker* tracker) { position_tracker_ = tracker; }
    void set_pnl_calculator(PnLCalculator* calculator) { pnl_calculator_ = calculator; }
    void set_risk_manager(RiskManager* manager) { risk_manager_ = manager; }

    // Lifecycle
    bool initialize() {
        LOG_INFO("StrategyManager: Initializing {} strategies", strategies_.size());

        for (auto& strategy : strategies_) {
            if (!strategy->initialize()) {
                LOG_ERROR("StrategyManager: Failed to initialize strategy '{}'", strategy->name());
                return false;
            }
        }

        return true;
    }

    void start() {
        if (running_.load(std::memory_order_relaxed)) {
            LOG_WARN("StrategyManager: Already running");
            return;
        }

        LOG_INFO("StrategyManager: Starting");

        // Start all strategies
        for (auto& strategy : strategies_) {
            strategy->start();
        }

        running_.store(true, std::memory_order_release);
        worker_thread_ = std::thread(&StrategyManager::run, this);

        LOG_INFO("StrategyManager: Started");
    }

    void stop() {
        if (!running_.load(std::memory_order_acquire)) {
            return;
        }

        LOG_INFO("StrategyManager: Stopping");

        running_.store(false, std::memory_order_release);

        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }

        // Stop all strategies
        for (auto& strategy : strategies_) {
            strategy->stop();
        }

        LOG_INFO("StrategyManager: Stopped");
    }

    void shutdown() {
        LOG_INFO("StrategyManager: Shutting down");

        for (auto& strategy : strategies_) {
            strategy->shutdown();
        }
    }

    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

private:
    void run() {
        LOG_INFO("StrategyManager: Worker thread started");

        while (running_.load(std::memory_order_acquire)) {
            process_market_data();
            process_order_events();
            std::this_thread::yield();
        }

        LOG_INFO("StrategyManager: Worker thread stopped");
    }

    void process_market_data() {
        if (!md_queue_) return;

        auto [data, size] = md_queue_->read(md_cursor_);
        if (!data) return;

        const Event* event = reinterpret_cast<const Event*>(data);

        switch (event->type) {
            case EventType::TRADE:
                dispatch_trade(*reinterpret_cast<const TradeEvent*>(data));
                break;
            case EventType::QUOTE:
                dispatch_quote(*reinterpret_cast<const QuoteEvent*>(data));
                break;
            case EventType::ORDER_BOOK:
                dispatch_order_book(*reinterpret_cast<const OrderBookEvent*>(data));
                break;
            default:
                LOG_WARN("StrategyManager: Unknown MD event type: {}", static_cast<int>(event->type));
                break;
        }

        md_cursor_ += size;
    }

    void process_order_events() {
        if (!order_event_queue_) return;

        auto [data, size] = order_event_queue_->read(order_cursor_);
        if (!data) return;

        const Event* event = reinterpret_cast<const Event*>(data);

        switch (event->type) {
            case EventType::ORDER_ACK:
                dispatch_order_ack(*reinterpret_cast<const OrderAckEvent*>(data));
                break;
            case EventType::ORDER_FILL:
                dispatch_order_fill(*reinterpret_cast<const OrderFillEvent*>(data));
                break;
            case EventType::ORDER_REJECT:
                dispatch_order_reject(*reinterpret_cast<const OrderRejectEvent*>(data));
                break;
            case EventType::ORDER_CANCEL:
                dispatch_order_cancel(*reinterpret_cast<const OrderCancelEvent*>(data));
                break;
            default:
                LOG_WARN("StrategyManager: Unknown order event type: {}", static_cast<int>(event->type));
                break;
        }

        order_cursor_ += size;
    }

    void dispatch_trade(const TradeEvent& event) {
        for (auto& strategy : strategies_) {
            strategy->on_trade(event);
        }
    }

    void dispatch_quote(const QuoteEvent& event) {
        for (auto& strategy : strategies_) {
            strategy->on_quote(event);
        }
    }

    void dispatch_order_book(const OrderBookEvent& event) {
        const std::string instrument(event.instrument);
        slick::orderbook::SymbolId sid = get_or_create_symbol_id(instrument);
        slick::orderbook::OrderBookL2* book = book_manager_.getOrCreateOrderBook(sid);

        if (event.is_snapshot) {
            book->clear();
        }

        const bool is_last_bid = (event.num_asks == 0);
        for (uint16_t i = 0; i < event.num_bids; ++i) {
            book->updateLevel(slick::orderbook::Side::Buy,
                              static_cast<slick::orderbook::Price>(event.bids[i].price),
                              static_cast<slick::orderbook::Quantity>(event.bids[i].quantity),
                              event.event_time_ns,
                              event.sequence_num,
                              is_last_bid && (i == event.num_bids - 1));
        }
        for (uint16_t i = 0; i < event.num_asks; ++i) {
            book->updateLevel(slick::orderbook::Side::Sell,
                              static_cast<slick::orderbook::Price>(event.asks[i].price),
                              static_cast<slick::orderbook::Quantity>(event.asks[i].quantity),
                              event.event_time_ns,
                              event.sequence_num,
                              i == event.num_asks - 1);
        }

        for (auto& strategy : strategies_) {
            strategy->on_order_book(event, *book);
        }
    }

    slick::orderbook::SymbolId get_or_create_symbol_id(const std::string& instrument) {
        auto it = symbol_ids_.find(instrument);
        if (it == symbol_ids_.end()) {
            slick::orderbook::SymbolId sid = next_symbol_id_++;
            symbol_ids_.emplace(instrument, sid);
            LOG_INFO("StrategyManager: Assigned symbol_id={} for {}", sid, instrument);
            return sid;
        }
        return it->second;
    }

    void dispatch_order_ack(const OrderAckEvent& event) {
        for (auto& strategy : strategies_) {
            strategy->on_order_ack(event);
        }
    }

    void dispatch_order_fill(const OrderFillEvent& event) {
        // Update mark price for PnL calculation
        if (pnl_calculator_) {
            pnl_calculator_->update_mark_price(std::string(event.instrument),
                                                event.fill_price,
                                                event.timestamp_ns);
        }

        // Dispatch to strategies (they track their own fills)
        for (auto& strategy : strategies_) {
            strategy->on_order_fill(event);
        }
    }

    void dispatch_order_reject(const OrderRejectEvent& event) {
        for (auto& strategy : strategies_) {
            strategy->on_order_reject(event);
        }
    }

    void dispatch_order_cancel(const OrderCancelEvent& event) {
        for (auto& strategy : strategies_) {
            strategy->on_order_cancel(event);
        }
    }

    std::vector<std::shared_ptr<IStrategy>> strategies_;
    slick::orderbook::OrderBookManager<slick::orderbook::OrderBookL2> book_manager_;
    std::unordered_map<std::string, slick::orderbook::SymbolId> symbol_ids_;
    slick::orderbook::SymbolId next_symbol_id_{0};
    std::shared_ptr<slick::SlickQueue<uint8_t>> md_queue_;
    std::shared_ptr<slick::SlickQueue<uint8_t>> order_event_queue_;
    std::shared_ptr<slick::SlickQueue<uint8_t>> order_submission_queue_;

    // Position & PnL tracking (non-owning pointers)
    PositionTracker* position_tracker_;
    PnLCalculator* pnl_calculator_;
    RiskManager* risk_manager_;

    std::atomic_bool running_;
    std::thread worker_thread_;
    uint64_t md_cursor_;
    uint64_t order_cursor_;
};

} // namespace slick::trader
