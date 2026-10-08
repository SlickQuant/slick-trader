#pragma once

#include <slick-trader/events/event.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick-trader/events/trade_event.hpp>
#include <slick-trader/events/quote_event.hpp>
#include <slick-trader/events/order_book_event.hpp>
#include <slick-trader/market_data/order_book.hpp>
#include <slick-trader/utils/types.hpp>
#include <slick/queue.hpp>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace slick::trader {

using json = nlohmann::json;

class PositionTracker;
class PnLCalculator;
class RiskManager;

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
    virtual bool initialize(
        const json& config,
        PositionTracker& position_tracker,
        PnLCalculator& pnl_calculator,
        RiskManager& risk_manager,
        slick::queue<uint8_t>& md_request_queue,
        slick::queue<uint8_t>& order_request_queue
    ) = 0;

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
};

} // namespace slick::trader
