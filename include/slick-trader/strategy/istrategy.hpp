#pragma once

#include <slick-trader/events/event.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick-trader/events/trade_event.hpp>
#include <slick-trader/events/quote_event.hpp>
#include <slick-trader/events/order_book_event.hpp>
#include <slick-trader/market_data/order_book.hpp>
#include <slick-trader/strategy/subscription_request.hpp>
#include <slick/queue.h>
#include <memory>
#include <string>
#include <vector>

namespace slick::trader {

// Strategy interface
// All strategies must implement this interface
// Strategies receive market data and order events, and can submit orders
class IStrategy {
public:
    virtual ~IStrategy() = default;

    // Strategy identification
    virtual const char* name() const = 0;
    virtual const char* version() const = 0;

    // Subscription requirements
    // Strategies declare what market data they need
    virtual std::vector<SubscriptionRequest> get_subscriptions() const = 0;

    // Lifecycle
    virtual bool initialize() = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void shutdown() = 0;

    // Market data event handlers
    virtual void on_trade(const TradeEvent& event) = 0;
    virtual void on_quote(const QuoteEvent& event) = 0;
    virtual void on_order_book(const slick::orderbook::OrderBookL2& book) = 0;

    // Order event handlers
    virtual void on_order_ack(const OrderAckEvent& event) = 0;
    virtual void on_order_fill(const OrderFillEvent& event) = 0;
    virtual void on_order_reject(const OrderRejectEvent& event) = 0;
    virtual void on_order_cancel(const OrderCancelEvent& event) = 0;

    // Queue access for order submission
    virtual void set_order_submission_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) = 0;
};

} // namespace slick::trader
