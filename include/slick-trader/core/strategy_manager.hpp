#pragma once

#include <slick-trader/strategy/istrategy.hpp>
#include <slick-trader/strategy/strategy_base.hpp>
#include <slick-trader/events/event.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick-trader/events/level2_update_event.hpp>
#include <slick-trader/events/mbo_update_event.hpp>
#include <slick-trader/events/quote_event.hpp>
#include <slick-trader/events/strategy_config_event.hpp>
#include <slick-trader/events/instrument_definition_event.hpp>
#include <slick-trader/events/instrument_status_event.hpp>
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
#include <map>
#include <set>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <string>

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
    // The file extension of a strategy plugin: what CMake gives a shared library on this
    // platform, and what load_strategies() looks for in the plugins folder.
#if defined(_WIN32)
    static constexpr std::string_view PLUGIN_EXTENSION = ".dll";
#elif defined(__APPLE__)
    static constexpr std::string_view PLUGIN_EXTENSION = ".dylib";
#else
    static constexpr std::string_view PLUGIN_EXTENSION = ".so";
#endif

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
            // One observer per book, however many subscription types feed it: a second
            // one would hand every update to the strategies twice.
            if (!observed_books_.insert(instrument).second) {
                continue;
            }
            auto *book = book_manager_.getOrCreateOrderBook(get_or_create_symbol_id(instrument));
            auto observer = std::make_shared<OrderBookObserverImpl>(this, *book, instrument);
            book->addObserver(observer);

            // The L2 book is now this instrument's source of quotes.
            if (auto it = mbo_books_.find(instrument); it != mbo_books_.end()) {
                it->second->publishes_top = false;
            }
        }
    }

    // Dispatches one event exactly as the worker thread does. While the manager runs the
    // worker is the only caller; it is public so a replay or a test can drive the manager
    // without the thread.
    void process_event(const Event* event);

    // The market-by-order book of an instrument, or nullptr when none was subscribed or
    // published. Owned by the worker thread: read it from there (a strategy callback).
    const slick::orderbook::OrderBookL3* get_order_book_l3(std::string_view instrument) const {
        const auto it = mbo_books_.find(instrument);
        return it != mbo_books_.end() ? &it->second->book : nullptr;
    }

private:
    friend struct OrderBookObserverImpl;

    void run();

    void subscribe(const SubscriptionRequest& sub);
    void unsubscribe(const SubscriptionRequest& sub);

    void check_loss_limits();
    void check_broker_status();
    void dispatch_strategy_config(const StrategyConfigEvent& event);

    // Sends one SUBSCRIBE or UNSUBSCRIBE request for `instruments` to the request's broker.
    void publish_subscription(EventType type, const SubscriptionRequest& sub,
                              const std::vector<std::string>& instruments);

    void dispatch_trade(const TradeEvent& event);
    void dispatch_quote(const QuoteEvent& event);
    void dispatch_level2_updates(const Level2UpdateEvent& event);
    void dispatch_mbo_update(const MboUpdateEvent& event);
    void dispatch_instrument_status(const InstrumentStatusEvent& event);

    void on_top_of_book_update(const std::string &instrument, const slick::orderbook::TopOfBook& tob);
    void on_order_book_update(const slick::orderbook::OrderBookL2& book);
    void dispatch_order_book(const OrderBookEvent& event);

    slick::orderbook::SymbolId get_or_create_symbol_id(const std::string& instrument);

    // One instrument's market-by-order book. Held here rather than in an OrderBookManager,
    // whose lookup takes a shared lock: only the worker thread touches these.
    struct MboBook {
        MboBook(slick::orderbook::SymbolId symbol, std::string name, bool publishes_top_of_book)
            : book(symbol, TOP_LEVELS, INITIAL_ORDERS, INITIAL_LEVELS)
            , instrument(std::move(name))
            , publishes_top(publishes_top_of_book)
        {}

        // Sized so a liquid futures book never grows on the dispatch path.
        static constexpr std::size_t TOP_LEVELS = 10;
        static constexpr std::size_t INITIAL_ORDERS = 16384;
        static constexpr std::size_t INITIAL_LEVELS = 256;

        slick::orderbook::OrderBookL3 book;
        std::string instrument;
        slick::orderbook::TopOfBook top;    // the last top of book published from this book
        bool publishes_top;                 // false once an L2 book of the instrument publishes it
    };
    MboBook& get_or_create_mbo_book(std::string_view instrument);

    // Lets the per-event lookup take the instrument as it arrives, without building a string.
    struct InstrumentHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view instrument) const noexcept {
            return std::hash<std::string_view>{}(instrument);
        }
    };

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
    // How many strategies hold each subscription. Keyed by all three of broker, type and
    // instrument: trades and the book of one instrument are separate requests to the broker.
    using SubscriptionKey = std::tuple<BrokerId, SubscriptionType, std::string>;
    std::map<SubscriptionKey, uint32_t> subscriptions_;
    std::unordered_set<std::string> observed_books_;    // instruments whose L2 book has its observer
    std::unordered_map<std::string, std::unique_ptr<MboBook>, InstrumentHash, std::equal_to<>> mbo_books_;
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
