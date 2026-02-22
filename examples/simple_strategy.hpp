#pragma once

#include <slick-trader/strategy/strategy_base.hpp>
#include <slick-trader/core/position.hpp>
#include <slick-trader/core/pnl_calculator.hpp>
#include <slick-trader/utils/types.hpp>
#include <slick/logger.hpp>
#include <string>
#include <chrono>

namespace slick::trader::examples {

// Simple example strategy that demonstrates:
// - Market data consumption
// - Order submission with risk validation
// - Position tracking
// - PnL calculation
class SimpleStrategy : public StrategyBase {
public:
    SimpleStrategy()
        : StrategyBase("SimpleStrategy", "1.0.0")
        , target_instrument_("BTC-USD")
        , target_position_size_(to_qty_t(0.01))
        , order_count_(0) {}

    ~SimpleStrategy() override = default;

protected:
    bool on_initialize() override {
        LOG_INFO("SimpleStrategy: Initializing");
        return true;
    }

    void on_start() override {
        LOG_INFO("SimpleStrategy: Started - will trade {} with target size {}",
                 target_instrument_, from_qty_t(target_position_size_));
    }

    void on_stop() override {
        LOG_INFO("SimpleStrategy: Stopped - submitted {} orders", order_count_);
    }

    void on_shutdown() override {
        LOG_INFO("SimpleStrategy: Shutting down");
    }

    // Market data handlers
    void on_trade(const TradeEvent& event) override {
        // Log every 100th trade to avoid spam
        if (++trade_count_ % 100 == 0) {
            LOG_DEBUG("SimpleStrategy: Trade {} - {}@{} (count: {})",
                     event.instrument, from_qty_t(event.quantity),
                     from_price_t(event.price), trade_count_);
        }
    }

    void on_quote(const QuoteEvent& event) override {
        std::string instrument(event.instrument);

        // Only trade our target instrument
        if (instrument != target_instrument_) {
            return;
        }

        // Calculate mid price
        const price_t mid_price = to_price_t(
            (from_price_t(event.bid_price) + from_price_t(event.ask_price)) / 2.0
        );

        // Check current position
        if (!get_position_tracker()) {
            return;
        }

        Position pos = get_position_tracker()->get_position(get_name(), target_instrument_);

        // Simple logic: If flat, buy at mid. If long, sell to close.
        if (pos.is_flat()) {
            // Submit a small buy order
            submit_buy_order(target_instrument_, mid_price, target_position_size_);
        } else if (pos.is_long()) {
            // Close the position
            submit_sell_order(target_instrument_, mid_price, pos.quantity);
        }
    }

    void on_order_book(const OrderBookEvent& event, const slick::orderbook::OrderBookL2& book) override {
        // Optional: Use order book for more sophisticated strategies
        std::string instrument(event.instrument);

        if (instrument == target_instrument_ && orderbook_update_count_++ % 50 == 0) {
            const auto* best_bid = book.getBestBid();
            const auto* best_ask = book.getBestAsk();

            if (best_bid && best_ask) {
                LOG_DEBUG("SimpleStrategy: OrderBook {} - bid: {}@{}, ask: {}@{}",
                         instrument,
                         best_bid->price, best_bid->quantity,
                         best_ask->price, best_ask->quantity);
            }
        }
    }

    // Order event handlers
    void on_order_ack(const OrderAckEvent& event) override {
        LOG_INFO("SimpleStrategy: Order ACK - {} {} (order_id: {})",
                 event.instrument, event.client_order_id, event.order_id);
    }

    void on_fill(const OrderFillEvent& event) override {
        // Note: Position tracking already done by StrategyBase::on_order_fill()
        LOG_INFO("SimpleStrategy: FILL - {} {}@{} ({})",
                 event.instrument,
                 from_qty_t(event.fill_quantity),
                 from_price_t(event.fill_price),
                 event.side == Side::BUY ? "BUY" : "SELL");

        // Log current position and PnL
        if (get_position_tracker()) {
            Position pos = get_position_tracker()->get_position(get_name(), std::string(event.instrument));
            LOG_INFO("SimpleStrategy: Position after fill - qty: {}, avg: {}, realized_pnl: {}",
                     from_qty_t(pos.quantity),
                     from_price_t(pos.avg_price),
                     from_price_t(pos.realized_pnl));
        }

        if (get_pnl_calculator()) {
            auto pnl = get_pnl_calculator()->calculate_pnl(get_name(),
                std::chrono::system_clock::now().time_since_epoch().count());
            LOG_INFO("SimpleStrategy: PnL - realized: {}, unrealized: {}, total: {}",
                     pnl.get_realized(), pnl.get_unrealized(), pnl.get_total());
        }
    }

    void on_order_reject(const OrderRejectEvent& event) override {
        LOG_WARN("SimpleStrategy: Order REJECTED - {} {} - Reason: {}",
                 event.instrument, event.client_order_id, event.reason);
    }

    void on_order_cancel(const OrderCancelEvent& event) override {
        LOG_INFO("SimpleStrategy: Order CANCELLED - {} {}",
                 event.instrument, event.client_order_id);
    }

private:
    void submit_buy_order(const std::string& instrument, price_t price, qty_t quantity) {
        NewOrderEvent order;
        order.timestamp_ns = std::chrono::system_clock::now().time_since_epoch().count();
        order.set_instrument(instrument.c_str());
        order.set_client_order_id(generate_order_id().c_str());
        order.side = Side::BUY;
        order.order_type = OrderType::LIMIT;
        order.time_in_force = TimeInForce::GTC;
        order.price = price;
        order.quantity = quantity;

        submit_order(order);
        order_count_++;
    }

    void submit_sell_order(const std::string& instrument, price_t price, qty_t quantity) {
        NewOrderEvent order;
        order.timestamp_ns = std::chrono::system_clock::now().time_since_epoch().count();
        order.set_instrument(instrument.c_str());
        order.set_client_order_id(generate_order_id().c_str());
        order.side = Side::SELL;
        order.order_type = OrderType::LIMIT;
        order.time_in_force = TimeInForce::GTC;
        order.price = price;
        order.quantity = quantity;

        submit_order(order);
        order_count_++;
    }

    std::string generate_order_id() {
        return "simple_" + std::to_string(order_count_);
    }

    std::string target_instrument_;
    qty_t target_position_size_;
    uint64_t order_count_;
    uint64_t trade_count_{0};
    uint64_t orderbook_update_count_{0};
};

} // namespace slick::trader::examples
