#pragma once

#include <slick-trader/strategy/istrategy.h>
#include <slick-trader/events/event_serializer.h>
#include <slick/logger.hpp>
#include <atomic>
#include <string>

namespace slick::trader {

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

    // Default implementations - strategies can override if needed
    void on_trade(const TradeEvent& event) override {}
    void on_quote(const QuoteEvent& event) override {}
    void on_order_book(const OrderBookEvent& event, const slick::orderbook::OrderBookL2& book) override {}
    void on_order_ack(const OrderAckEvent& event) override {}
    void on_order_fill(const OrderFillEvent& event) override {}
    void on_order_reject(const OrderRejectEvent& event) override {}
    void on_order_cancel(const OrderCancelEvent& event) override {}

protected:
    // Lifecycle hooks for derived classes
    virtual bool on_initialize() { return true; }
    virtual void on_start() {}
    virtual void on_stop() {}
    virtual void on_shutdown() {}

    // Helper methods for order submission
    void submit_order(const NewOrderEvent& order) {
        if (!order_submission_queue_) {
            LOG_ERROR("Strategy {}: Order submission queue not set", name_);
            return;
        }

        EventSerializer::publish(*order_submission_queue_, order);
        LOG_DEBUG("Strategy {}: Submitted order {} for {} {}@{}",
                 name_, order.client_order_id, order.instrument,
                 from_qty_t(order.quantity), from_price_t(order.price));
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

private:
    std::string name_;
    std::string version_;
    std::atomic_bool running_;
    std::shared_ptr<slick::SlickQueue<uint8_t>> order_submission_queue_;
};

} // namespace slick::trader
