#pragma once

#include <slick-trader/strategy/istrategy.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/core/position_tracker.hpp>
#include <slick-trader/core/risk_manager.hpp>
#include <slick/logger.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <string>

namespace slick::trader {

// Forward declarations
class PnLCalculator;

// Base class for strategies
// Provides common functionality and utilities
class StrategyBase : public IStrategy {
public:
    StrategyBase(const std::string& name, const std::string& version)
        : name_(name)
        , version_(version)
        , running_(false) {}

    virtual ~StrategyBase() = default;

    // IStrategy interface
    const char* name() const override { return name_.c_str(); }
    const char* version() const override { return version_.c_str(); }

    // Default: no subscriptions (strategies should override this)
    std::vector<SubscriptionRequest> get_subscriptions() const override {
        return {};
    }

    bool initialize() override {
        LOG_INFO("Strategy {}: Initializing", name_);
        return on_initialize();
    }

    void start() override {
        LOG_INFO("Strategy {}: Starting", name_);
        running_.store(true, std::memory_order_release);
        on_start();
    }

    void stop() override {
        LOG_INFO("Strategy {}: Stopping", name_);
        on_stop();
        running_.store(false, std::memory_order_release);
    }

    void shutdown() override {
        LOG_INFO("Strategy {}: Shutting down", name_);
        on_shutdown();
    }

    void set_order_submission_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) override {
        order_submission_queue_ = queue;
    }

    // Position & PnL tracking (set by TradingEngine)
    void set_position_tracker(PositionTracker* tracker) { position_tracker_ = tracker; }
    void set_pnl_calculator(PnLCalculator* calculator) { pnl_calculator_ = calculator; }
    void set_risk_manager(RiskManager* manager) { risk_manager_ = manager; }

    // Configuration (set by TradingEngine during strategy loading)
    void set_config(const nlohmann::json& params) {
        config_params_ = params;
        on_config(params);  // Call derived class hook
    }

    // Default implementations - strategies can override if needed
    void on_trade(const TradeEvent& event) override {}
    void on_quote(const QuoteEvent& event) override {}
    void on_order_book(const OrderBookEvent& event, const slick::orderbook::OrderBookL2& book) override {}
    void on_order_ack(const OrderAckEvent& event) override {}

    // Automatically track fills in position tracker
    void on_order_fill(const OrderFillEvent& event) override {
        if (position_tracker_) {
            position_tracker_->on_fill(name_, event);
        }
        on_fill(event);  // Call derived class hook
    }

    void on_order_reject(const OrderRejectEvent& event) override {}
    void on_order_cancel(const OrderCancelEvent& event) override {}

protected:
    friend class WebServer;  // Allow WebServer to call submit_order

    // Lifecycle hooks for derived classes
    virtual bool on_initialize() { return true; }
    virtual void on_start() {}
    virtual void on_stop() {}
    virtual void on_shutdown() {}

    // Configuration hook (called when config is set)
    virtual void on_config(const nlohmann::json& params) {}

    // Fill hook (called after position tracking)
    virtual void on_fill(const OrderFillEvent& event) {}

    virtual std::string generate_order_id() {
        return name_ + "_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    }

    // Helper methods for order submission
    void submit_order(std::string_view instrument, BrokerId broker_id, Side side, qty_t quantity, OrderType type, price_t price = NULL_PRICE, TimeInForce tif = TimeInForce::FOK, bool post_only = true) {
        if (!order_submission_queue_) {
            LOG_ERROR("Strategy {}: Order submission queue not set", name_);
            return;
        }

        auto client_order_id = generate_order_id();

        // Pre-trade risk validation
        if (risk_manager_) {
            std::string reject_reason;
            if (!risk_manager_->validate_order(name_, instrument, side, quantity, price, &reject_reason)) {
                LOG_WARN("Strategy {}: Order {} REJECTED by risk: {}",
                         name_, client_order_id, reject_reason);

                // Publish internal reject event back to strategy
                OrderRejectEvent reject;
                reject.timestamp_ns = std::chrono::system_clock::now().time_since_epoch().count();
                reject.set_instrument(instrument);
                reject.set_client_order_id(client_order_id.data());
                reject.set_reason(reject_reason.c_str());
                on_order_reject(reject);
                return;
            }
        }

        EventSerializer::publish<NewOrderEvent>(*order_submission_queue_, [=](NewOrderEvent& order) {
            order.broker_id = broker_id;
            order.set_instrument(instrument.data());
            order.set_client_order_id(client_order_id.data());
            order.side = side;
            order.order_type = type;
            order.price = price;
            order.quantity = quantity;
            order.time_in_force = tif;
            order.post_only = post_only;
        });
        LOG_DEBUG("Strategy {}: Submitted order {} for {} {}@{}",
                 name_, client_order_id, instrument,
                 from_qty_t(quantity), from_price_t(price));
    }

    void cancel_order(const std::string& order_id, const std::string& client_order_id) {
        if (!order_submission_queue_) {
            LOG_ERROR("Strategy {}: Order submission queue not set", name_);
            return;
        }

        CancelOrderEvent cancel;
        cancel.set_order_id(order_id);
        cancel.set_client_order_id(client_order_id);

        EventSerializer::publish(*order_submission_queue_, cancel);
        LOG_DEBUG("Strategy {}: Cancelled order {}", name_, order_id);
    }

    bool is_running() const { return running_.load(std::memory_order_acquire); }

    const std::string& get_name() const { return name_; }

    // Configuration access
    template<typename T>
    T get_parameter(const std::string& key, const T& default_value) const {
        if (config_params_.contains(key)) {
            try {
                return config_params_[key].get<T>();
            } catch (const std::exception&) {
                return default_value;
            }
        }
        return default_value;
    }

    const nlohmann::json& get_config() const { return config_params_; }

    // Position & PnL access
    PositionTracker* get_position_tracker() { return position_tracker_; }
    PnLCalculator* get_pnl_calculator() { return pnl_calculator_; }
    RiskManager* get_risk_manager() { return risk_manager_; }

private:
    std::string name_;
    std::string version_;
    std::atomic_bool running_;
    std::shared_ptr<slick::SlickQueue<uint8_t>> order_submission_queue_;

    // Configuration parameters
    nlohmann::json config_params_;

    // Position & PnL tracking (non-owning pointers, set by TradingEngine)
    PositionTracker* position_tracker_{nullptr};
    PnLCalculator* pnl_calculator_{nullptr};
    RiskManager* risk_manager_{nullptr};
};

} // namespace slick::trader
