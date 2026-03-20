#pragma once

#include <slick-trader/strategy/strategy_base.hpp>
#include <slick/logger.hpp>
#include <string>
#include <unordered_map>
#include <atomic>

namespace slick::trader {

// Simple market making strategy
// Places bid/ask orders around the mid-price with a fixed spread
// Demonstrates order submission and lifecycle management
class SimpleMarketMaker : public StrategyBase {
public:
    SimpleMarketMaker(const std::string& instrument, double spread_bps, double order_size)
        : StrategyBase("SimpleMarketMaker", "1.0.0")
        , instrument_(instrument)
        , spread_bps_(spread_bps)
        , order_size_(order_size)
        , mid_price_(0.0)
        , best_bid_(0.0)
        , best_ask_(0.0)
        , order_id_counter_(0)
        , active_buy_order_id_()
        , active_sell_order_id_() {}

    ~SimpleMarketMaker() override = default;

protected:
    bool on_initialize() override {
        LOG_INFO("SimpleMarketMaker: Initialized for {} with spread {} bps, size {}",
                 instrument_, spread_bps_, order_size_);
        return true;
    }

    void on_start() override {
        LOG_INFO("SimpleMarketMaker: Started trading {}", instrument_);
    }

    void on_stop() override {
        LOG_INFO("SimpleMarketMaker: Stopped");

        // Cancel any active orders
        if (!active_buy_order_id_.empty()) {
            cancel_order(active_buy_order_id_, "");
        }
        if (!active_sell_order_id_.empty()) {
            cancel_order(active_sell_order_id_, "");
        }
    }

    void on_shutdown() override {
        LOG_INFO("SimpleMarketMaker: Shutdown");
    }

    void on_trade(const TradeEvent& event) override {
        // Only process trades for our instrument
        if (std::string(event.instrument) != instrument_) {
            return;
        }

        double trade_price = from_price_t(event.price);

        // Update mid price estimate
        if (best_bid_ > 0.0 && best_ask_ > 0.0) {
            mid_price_ = (best_bid_ + best_ask_) / 2.0;
        } else {
            mid_price_ = trade_price;
        }

        LOG_DEBUG("SimpleMarketMaker: Trade {} {}@{}, mid={}",
                 event.instrument, from_qty_t(event.quantity), trade_price, mid_price_);
    }

    void on_quote(const QuoteEvent& event) override {
        // Only process quotes for our instrument
        if (std::string(event.instrument) != instrument_) {
            return;
        }

        best_bid_ = from_price_t(event.bid_price);
        best_ask_ = from_price_t(event.ask_price);
        mid_price_ = (best_bid_ + best_ask_) / 2.0;

        LOG_DEBUG("SimpleMarketMaker: Quote {} Bid: {}@{} Ask: {}@{} Mid: {}",
                 event.instrument,
                 from_qty_t(event.bid_quantity), best_bid_,
                 from_qty_t(event.ask_quantity), best_ask_,
                 mid_price_);

        // Update quotes if we have a valid mid price
        if (mid_price_ > 0.0 && active_buy_order_id_.empty() && active_sell_order_id_.empty()) {
            place_quotes();
        }
    }

    void on_order_ack(const OrderAckEvent& event) override {
        // Track our active orders
        std::string client_order_id(event.client_order_id);

        if (client_order_id.find("_BUY") != std::string::npos) {
            active_buy_order_id_ = event.order_id;
            LOG_INFO("SimpleMarketMaker: Buy order acknowledged: {}", event.order_id);
        } else if (client_order_id.find("_SELL") != std::string::npos) {
            active_sell_order_id_ = event.order_id;
            LOG_INFO("SimpleMarketMaker: Sell order acknowledged: {}", event.order_id);
        }
    }

    void on_order_fill(const OrderFillEvent& event) override {
        std::string order_id(event.order_id);

        LOG_INFO("SimpleMarketMaker: Order filled! {} Filled: {}@{} (cumulative: {})",
                 event.instrument,
                 from_qty_t(event.fill_quantity),
                 from_price_t(event.fill_price),
                 from_qty_t(event.cumulative_filled));

        // Clear the active order
        if (order_id == active_buy_order_id_) {
            active_buy_order_id_.clear();
        } else if (order_id == active_sell_order_id_) {
            active_sell_order_id_.clear();
        }

        // Place new quotes
        if (mid_price_ > 0.0) {
            place_quotes();
        }
    }

    void on_order_reject(const OrderRejectEvent& event) override {
        LOG_WARN("SimpleMarketMaker: Order rejected: {} Reason: {}",
                 event.client_order_id, event.reason);
    }

    void on_order_cancel(const OrderCancelEvent& event) override {
        std::string order_id(event.order_id);

        LOG_INFO("SimpleMarketMaker: Order cancelled: {}", order_id);

        // Clear the active order
        if (order_id == active_buy_order_id_) {
            active_buy_order_id_.clear();
        } else if (order_id == active_sell_order_id_) {
            active_sell_order_id_.clear();
        }
    }

private:
    void place_quotes() {
        if (mid_price_ <= 0.0) {
            return;
        }

        // Calculate bid/ask prices with spread
        double half_spread = mid_price_ * (spread_bps_ / 10000.0) / 2.0;
        double bid_price = mid_price_ - half_spread;
        double ask_price = mid_price_ + half_spread;

        // Place buy order
        if (active_buy_order_id_.empty()) {
            submit_order(instrument_, BrokerId::SIMULATED, Side::BUY, to_qty_t(order_size_), OrderType::LIMIT, to_price_t(bid_price), TimeInForce::GTC);
            LOG_INFO("SimpleMarketMaker: Placed BUY order {} @ {}", order_size_, bid_price);
        }

        // Place sell order
        if (active_sell_order_id_.empty()) {
            submit_order(instrument_, BrokerId::SIMULATED, Side::SELL, to_qty_t(order_size_), OrderType::LIMIT, to_price_t(ask_price), TimeInForce::GTC);
            LOG_INFO("SimpleMarketMaker: Placed SELL order {} @ {}", order_size_, ask_price);
        }
    }

    std::string instrument_;
    double spread_bps_;
    double order_size_;

    double mid_price_;
    double best_bid_;
    double best_ask_;

    std::atomic<uint64_t> order_id_counter_;
    std::string active_buy_order_id_;
    std::string active_sell_order_id_;
};

} // namespace slick::trader
