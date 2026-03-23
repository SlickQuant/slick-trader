#pragma once

#include <slick-trader/strategy/strategy_base.hpp>
#include <slick/logger.hpp>
#include <string>
#include <unordered_map>

namespace slick::trader {

// Simple logging strategy that prints market data and order events
// Demonstrates the full event flow through the system
class SimpleLoggerStrategy : public StrategyBase {
public:
    SimpleLoggerStrategy()
        : StrategyBase("SimpleLogger", "1.0.0")
        , trade_count_(0)
        , quote_count_(0)
        , order_count_(0) {}

    ~SimpleLoggerStrategy() override = default;

    // Declare what market data this strategy needs
    std::vector<SubscriptionRequest> get_subscriptions() const override {
        return {
            SubscriptionRequest(BrokerId::COINBASE, SubscriptionType::TRADE, {"BTC-USD", "ETH-USD"}),
            SubscriptionRequest(BrokerId::COINBASE, SubscriptionType::QUOTE, {"BTC-USD", "ETH-USD"}),
            SubscriptionRequest(BrokerId::COINBASE, SubscriptionType::LEVEL2, {"BTC-USD", "ETH-USD"}, 20)  // 20 levels
        };
    }

protected:
    bool on_initialize() override {
        LOG_INFO("SimpleLogger: Initialized");
        return true;
    }

    void on_start() override {
        LOG_INFO("SimpleLogger: Started - ready to receive events");
    }

    void on_stop() override {
        LOG_INFO("SimpleLogger: Stopped");
        LOG_INFO("SimpleLogger: Summary - Trades: {}, Quotes: {}, Orders: {}",
                 trade_count_, quote_count_, order_count_);
    }

    void on_shutdown() override {
        LOG_INFO("SimpleLogger: Shutdown");
    }

    void on_trade(const TradeEvent& event) override {
        trade_count_++;

        if (trade_count_ % 100 == 0) {
            LOG_INFO("SimpleLogger: [TRADE #{}] {} {} {}@{} (broker: {})",
                     trade_count_,
                     event.instrument,
                     event.side == Side::BUY ? "BUY" : "SELL",
                     from_qty_t(event.quantity),
                     from_price_t(event.price),
                     static_cast<int>(event.broker_id));
        }
    }

    void on_quote(const QuoteEvent& event) override {
        quote_count_++;

        if (quote_count_ % 100 == 0) {
            LOG_INFO("SimpleLogger: [QUOTE #{}] {} Bid: {}@{} Ask: {}@{}",
                     quote_count_,
                     event.instrument,
                     from_qty_t(event.bid_quantity),
                     from_price_t(event.bid_price),
                     from_qty_t(event.ask_quantity),
                     from_price_t(event.ask_price));
        }
    }

    void on_order_book(const OrderBookEvent& event, const slick::orderbook::OrderBookL2& book) override {
        auto tob = book.getTopOfBook();
        LOG_DEBUG("SimpleLogger: [ORDER_BOOK] {} bids: {} asks: {} mid: {}",
                 event.instrument, event.num_bids, event.num_asks, tob.midPrice());
    }

    void on_order_ack(const OrderAckEvent& event) override {
        order_count_++;
        LOG_INFO("SimpleLogger: [ORDER_ACK] order_id={} client_order_id={} {} {} {}@{} status={}",
                 event.order_id,
                 event.client_order_id,
                 event.instrument,
                 event.side == Side::BUY ? "BUY" : "SELL",
                 from_qty_t(event.quantity),
                 from_price_t(event.price),
                 static_cast<int>(event.status));
    }

    void on_order_fill(const OrderFillEvent& event) override {
        LOG_INFO("SimpleLogger: [ORDER_FILL] order_id={} fill_id={} {} Filled: {}@{} (cumulative: {})",
                 event.order_id,
                 event.fill_id,
                 event.instrument,
                 from_qty_t(event.fill_quantity),
                 from_price_t(event.fill_price),
                 from_qty_t(event.cumulative_filled));
    }

    void on_order_reject(const OrderRejectEvent& event) override {
        LOG_WARN("SimpleLogger: [ORDER_REJECT] client_order_id={} {} Reason: {}",
                 event.client_order_id,
                 event.instrument,
                 event.reason);
    }

    void on_order_cancel(const OrderCancelEvent& event) override {
        LOG_INFO("SimpleLogger: [ORDER_CANCEL] order_id={} {} Reason: {}",
                 event.order_id,
                 event.instrument,
                 event.reason);
    }

private:
    uint64_t trade_count_;
    uint64_t quote_count_;
    uint64_t order_count_;
};

} // namespace slick::trader
