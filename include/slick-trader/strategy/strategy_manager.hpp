#pragma once

#include <slick-trader/strategy/istrategy.hpp>
#include <slick-trader/strategy/strategy_base.hpp>
#include <slick-trader/events/event.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick-trader/events/level2_update_event.hpp>
#include <slick-trader/events/quote_event.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/market_data/order_book.hpp>
#include <slick/queue.h>
#include <slick/logger.hpp>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <chrono>

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
        strategy_map_[strategy->name()] = strategy;

        // Forward already-configured resources to the new strategy
        if (auto* base = dynamic_cast<StrategyBase*>(strategy.get())) {
            if (position_tracker_) base->set_position_tracker(position_tracker_);
            if (pnl_calculator_)   base->set_pnl_calculator(pnl_calculator_);
            if (risk_manager_)     base->set_risk_manager(risk_manager_);
        }
        if (order_submission_queue_) {
            strategy->set_order_submission_queue(order_submission_queue_);
        }

        LOG_INFO("StrategyManager: Added strategy '{}'", strategy->name());
    }

    // Queue setup
    void set_market_data_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) {
        md_queue_ = queue;
    }

    void set_order_event_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) {
        order_event_queue_ = queue;
    }

    void set_tob_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) {
        tob_queue_ = queue;
    }

    void set_order_submission_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) {
        order_submission_queue_ = queue;

        // Set order submission queue for all strategies
        for (auto& strategy : strategies_) {
            strategy->set_order_submission_queue(queue);
        }
    }

    // Position & PnL tracking
    void set_position_tracker(PositionTracker* tracker) {
        position_tracker_ = tracker;
        for (auto& s : strategies_) {
            if (auto* base = dynamic_cast<StrategyBase*>(s.get())) base->set_position_tracker(tracker);
        }
    }
    void set_pnl_calculator(PnLCalculator* calculator) {
        pnl_calculator_ = calculator;
        for (auto& s : strategies_) {
            if (auto* base = dynamic_cast<StrategyBase*>(s.get())) base->set_pnl_calculator(calculator);
        }
    }
    void set_risk_manager(RiskManager* manager) {
        risk_manager_ = manager;
        for (auto& s : strategies_) {
            if (auto* base = dynamic_cast<StrategyBase*>(s.get())) base->set_risk_manager(manager);
        }
    }

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

private:
    void run() {
        LOG_INFO("StrategyManager: Worker thread started");

        // Track last PnL check time
        auto last_pnl_check = std::chrono::steady_clock::now();
        constexpr auto pnl_check_interval = std::chrono::seconds(1);

        while (running_.load(std::memory_order_acquire)) {
            process_market_data();
            process_order_events();

            // Periodically check for loss limits
            auto now = std::chrono::steady_clock::now();
            if (now - last_pnl_check >= pnl_check_interval) {
                check_loss_limits();
                last_pnl_check = now;
            }

            std::this_thread::yield();
        }

        LOG_INFO("StrategyManager: Worker thread stopped");
    }

    void check_loss_limits() {
        if (!pnl_calculator_ || !risk_manager_) {
            return;
        }

        const auto now_ns = get_timestamp_ns();

        // Check each strategy
        for (auto& strategy : strategies_) {
            const std::string strategy_name(strategy->name());

            // Skip if already halted
            if (risk_manager_->is_halted(strategy_name)) {
                continue;
            }

            // Get risk limits
            const auto limits = risk_manager_->get_limits(strategy_name);

            // Calculate current PnL
            const auto pnl = pnl_calculator_->calculate_pnl(strategy_name, now_ns);

            // Check if total PnL has breached the loss limit
            if (pnl.total_pnl < limits.max_loss_per_strategy) {
                const double loss = from_price_t(pnl.total_pnl);
                const double max_loss = from_price_t(limits.max_loss_per_strategy);

                const std::string reason = "Loss limit breached: PnL=" +
                    std::to_string(loss) + " < max_loss=" + std::to_string(max_loss);

                LOG_ERROR("StrategyManager: {} - {}", strategy_name, reason);

                // Close all open positions before halting — leaves no unmanaged exposure
                if (auto* base = dynamic_cast<StrategyBase*>(strategy.get())) {
                    base->close_all_positions();
                }

                risk_manager_->halt_strategy(strategy_name, reason);

                // Stop the strategy
                strategy->stop();
            }
        }
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
            case EventType::LEVEL2_UPDATE:
                dispatch_level2_updates(*reinterpret_cast<const Level2UpdateEvent*>(data));
                break;
            case EventType::ORDER_BOOK:
                dispatch_order_book(*reinterpret_cast<const OrderBookEvent*>(data));
                break;
            default:
                LOG_WARN("StrategyManager: Unknown MD event type: {}", static_cast<int>(event->type));
                break;
        }

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
    }

    void dispatch_trade(const TradeEvent& event) {
        if (pnl_calculator_) {
            pnl_calculator_->update_mark_price(std::string(event.instrument),
                                               event.price,
                                               event.timestamp_ns);
        }
        for (auto& strategy : strategies_) {
            strategy->on_trade(event);
        }
    }

    void dispatch_quote(const QuoteEvent& event) {
        if (pnl_calculator_ && event.bid_price > 0 && event.ask_price > 0) {
            const price_t mid = (event.bid_price / 2) + (event.ask_price / 2);
            pnl_calculator_->update_mark_price(std::string(event.instrument),
                                               mid,
                                               event.timestamp_ns);
        }
        for (auto& strategy : strategies_) {
            strategy->on_quote(event);
        }
    }

    void dispatch_level2_updates(const Level2UpdateEvent& event) {
        const std::string instrument(event.instrument);
        slick::orderbook::SymbolId sid = get_or_create_symbol_id(instrument);
        slick::orderbook::OrderBookL2* book = book_manager_.getOrCreateOrderBook(sid);

        if (event.is_snapshot) {
            book->clear();
        }

        auto last = event.num_levels - 1;
        for (uint16_t i = 0; i < event.num_levels; ++i) {
            if (event.levels[i].side == Side::BUY) {
                book->updateLevel(slick::orderbook::Side::Buy,
                                  static_cast<slick::orderbook::Price>(event.levels[i].price),
                                  static_cast<slick::orderbook::Quantity>(event.levels[i].quantity),
                                  event.event_time_ns,
                                  event.sequence_num,
                                  i == last);
            }
            else {
                book->updateLevel(slick::orderbook::Side::Sell,
                                  static_cast<slick::orderbook::Price>(event.levels[i].price),
                                  static_cast<slick::orderbook::Quantity>(event.levels[i].quantity),
                                  event.event_time_ns,
                                  event.sequence_num,
                                  i == last);
            }
        }

        // Check if top-of-book changed; if so update mark price and notify WebServer
        const auto tob = book->getTopOfBook();
        const price_t new_bid = static_cast<price_t>(tob.best_bid);
        const price_t new_ask = static_cast<price_t>(tob.best_ask);

        auto& cached = tob_cache_[instrument];
        if (new_bid != cached.first || new_ask != cached.second) {
            cached = {new_bid, new_ask};

            if (pnl_calculator_ && new_bid > 0 && new_ask > 0) {
                const price_t mid = (new_bid / 2) + (new_ask / 2);
                pnl_calculator_->update_mark_price(instrument, mid, event.event_time_ns);
            }

            if (tob_queue_ && new_bid > 0 && new_ask > 0) {
                EventSerializer::publish<QuoteEvent>(*tob_queue_, [&](QuoteEvent& q) {
                    q.set_instrument(instrument);
                    q.bid_price = new_bid;
                    q.bid_quantity = static_cast<qty_t>(tob.bid_quantity);
                    q.ask_price = new_ask;
                    q.ask_quantity = static_cast<qty_t>(tob.ask_quantity);
                    q.event_time_ns = event.event_time_ns;
                });
            }
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
        LOG_INFO("StrategyManager: dispatch_order_ack order_id={} client_order_id={} to {} strategies",
                 std::string(event.order_id), std::string(event.client_order_id), strategies_.size());
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
    std::unordered_map<std::string, std::shared_ptr<IStrategy>> strategy_map_;
    slick::orderbook::OrderBookManager<slick::orderbook::OrderBookL2> book_manager_;
    std::unordered_map<std::string, slick::orderbook::SymbolId> symbol_ids_;
    slick::orderbook::SymbolId next_symbol_id_{0};
    std::shared_ptr<slick::SlickQueue<uint8_t>> md_queue_;
    std::shared_ptr<slick::SlickQueue<uint8_t>> order_event_queue_;
    std::shared_ptr<slick::SlickQueue<uint8_t>> order_submission_queue_;
    std::shared_ptr<slick::SlickQueue<uint8_t>> tob_queue_;
    std::unordered_map<std::string, std::pair<price_t, price_t>> tob_cache_; // instrument -> {best_bid, best_ask}

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
