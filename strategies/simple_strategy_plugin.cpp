// SimpleStrategy as a dynamically loadable plugin
// This demonstrates the plugin architecture for SlickTrader strategies

#include <slick-trader/strategy/strategy_base.hpp>
#include <slick-trader/strategy/strategy_export.hpp>
#include <slick-trader/strategy/subscription_request.hpp>
#include <slick-trader/core/position.hpp>
#include <slick-trader/core/pnl_calculator.hpp>
#include <slick-trader/utils/types.hpp>
#include <slick/logger.hpp>
#include <string>
#include <chrono>

using namespace slick::trader;

// Simple example strategy that demonstrates:
// - Market data consumption
// - Order submission with risk validation
// - Position tracking
// - PnL calculation
class SimpleStrategyPlugin : public StrategyBase {
public:
    SimpleStrategyPlugin()
        : StrategyBase("SimpleStrategyPlugin", "1.0.0")
        , target_instrument_("BTC-USD")
        , target_position_size_(to_qty_t(0.00001))
        , order_count_(0) {}

    ~SimpleStrategyPlugin() override = default;

protected:
    bool on_initialize() override {
        LOG_INFO("SimpleStrategyPlugin: Initializing");
        return true;
    }

    void on_start() override {
        LOG_INFO("SimpleStrategyPlugin: Started - will trade {} with target size {}",
                 target_instrument_, from_qty_t(target_position_size_));
    }

    void on_stop() override {
        LOG_INFO("SimpleStrategyPlugin: Stopped - submitted {} orders", order_count_);
    }

    void on_shutdown() override {
        LOG_INFO("SimpleStrategyPlugin: Shutting down");
    }

    // Declare what market data this strategy needs
    std::vector<SubscriptionRequest> get_subscriptions() const override {
        return {
            SubscriptionRequest(BrokerId::COINBASE, SubscriptionType::ORDER_BOOK, {"BTC-USD"}),
            SubscriptionRequest(BrokerId::COINBASE, SubscriptionType::TRADE, {"BTC-USD"})
        };
    }

    // Market data handlers
    void on_trade(const TradeEvent& event) override {
        // Log every 100th trade to avoid spam
        if (++trade_count_ % 100 == 0) {
            LOG_DEBUG("SimpleStrategyPlugin: Trade {} - {}@{} (count: {})",
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

    // Order event handlers
    void on_order_ack(const OrderAckEvent& event) override {
        LOG_INFO("SimpleStrategyPlugin: Order ACK - {} {} (order_id: {})",
                 event.instrument, event.client_order_id, event.order_id);
    }

    void on_fill(const OrderFillEvent& event) override {
        // Note: Position tracking already done by StrategyBase::on_order_fill()
        LOG_INFO("SimpleStrategyPlugin: FILL - {} {}@{} ({})",
                 event.instrument,
                 from_qty_t(event.fill_quantity),
                 from_price_t(event.fill_price),
                 event.side == Side::BUY ? "BUY" : "SELL");

        // Log current position and PnL
        if (get_position_tracker()) {
            Position pos = get_position_tracker()->get_position(get_name(), std::string(event.instrument));
            LOG_INFO("SimpleStrategyPlugin: Position after fill - qty: {}, avg: {}, realized_pnl: {}",
                     from_qty_t(pos.quantity),
                     from_price_t(pos.avg_price),
                     from_price_t(pos.realized_pnl));
        }

        if (get_pnl_calculator()) {
            auto pnl = get_pnl_calculator()->calculate_pnl(get_name(),
                std::chrono::system_clock::now().time_since_epoch().count());
            LOG_INFO("SimpleStrategyPlugin: PnL - realized: {}, unrealized: {}, total: {}",
                     pnl.get_realized(), pnl.get_unrealized(), pnl.get_total());
        }
    }

    void on_order_reject(const OrderRejectEvent& event) override {
        LOG_WARN("SimpleStrategyPlugin: Order REJECTED - {} {} - Reason: {}",
                 event.instrument, event.client_order_id, event.reason);
    }

private:
    void submit_buy_order(const std::string& instrument, price_t price, qty_t quantity) {
        submit_order(instrument.data(), BrokerId::SIMULATED, Side::BUY, quantity, OrderType::LIMIT, price);
        order_count_++;
    }

    void submit_sell_order(const std::string& instrument, price_t price, qty_t quantity) {
        submit_order(instrument.data(), BrokerId::SIMULATED, Side::SELL, quantity, OrderType::LIMIT, price);
        order_count_++;
    }

    std::string generate_order_id() override {
        return "simple_plugin_" + std::to_string(order_count_);
    }

    std::string target_instrument_;
    qty_t target_position_size_;
    uint64_t order_count_;
    uint64_t trade_count_{0};
};

// Export the strategy plugin
SLICK_TRADER_EXPORT_STRATEGY(SimpleStrategyPlugin)
