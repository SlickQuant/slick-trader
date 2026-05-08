#pragma once

#include <slick-trader/strategy/istrategy.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/core/order.hpp>
#include <slick-trader/core/pnl_calculator.hpp>
#include <slick-trader/core/position_tracker.hpp>
#include <slick-trader/core/risk_manager.hpp>
#include <slick-trader/utils/time.hpp>
#include <slick/logger.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

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
    void on_order_book(const slick::orderbook::OrderBookL2& book) override {}

    // Maintains order tracking state, then calls on_ack() hook for derived classes
    void on_order_ack(const OrderAckEvent& event) override {
        const std::string client_id(event.client_order_id);
        const std::string order_id(event.order_id);
        LOG_INFO("Strategy {}: on_order_ack client_order_id={} order_id={}", name_, client_id, order_id);
        // Coinbase sends a second OPEN-status ACK with empty client_order_id; ignore it to
        // prevent a phantom "" entry from overwriting order_id_to_client_id_ mappings.
        if (client_id.empty()) {
            LOG_DEBUG("Strategy {}: ignoring ACK with empty client_order_id for order_id={}", name_, order_id);
            on_ack(event);
            return;
        }
        auto it = orders_by_client_id_.find(client_id);
        if (it == orders_by_client_id_.end()) {
            // Order not previously seen (e.g. arrived from reconnect snapshot)
            LOG_INFO("Strategy {}: ACK for unknown client_order_id={} - inserting new entry", name_, client_id);
            Order order;
            order.client_order_id  = client_id;
            order.order_id         = order_id;
            order.instrument       = std::string(event.instrument);
            order.side             = event.side;
            order.type             = event.order_type;
            order.time_in_force    = event.time_in_force;
            order.price            = event.price;
            order.stop_price       = event.stop_price;
            order.qty              = event.quantity;
            order.remaining_qty    = event.quantity;
            order.status           = event.status;
            order.created_time     = event.event_time_ns;
            order.last_update_time = event.timestamp_ns;
            order.post_only        = event.post_only;
            order_id_to_client_id_[order.order_id] = client_id;
            orders_by_client_id_.emplace(client_id, std::move(order));
        } else {
            // Update existing entry with broker-assigned order_id and any modified fields
            // (covers both initial new-order ACK and modify-ack with updated price/qty)
            LOG_INFO("Strategy {}: ACK updating existing entry client_order_id={} order_id={}", name_, client_id, order_id);
            it->second.order_id         = order_id;
            it->second.status           = event.status;
            it->second.price            = event.price;
            it->second.stop_price       = event.stop_price;
            it->second.qty              = event.quantity;
            it->second.remaining_qty    = event.quantity;
            it->second.time_in_force    = event.time_in_force;
            it->second.post_only        = event.post_only;
            it->second.last_update_time = event.timestamp_ns;
            order_id_to_client_id_[it->second.order_id] = client_id;
        }
        on_ack(event);
    }

    // Automatically track fills in position tracker and maintain order state
    void on_order_fill(const OrderFillEvent& event) override {
        const std::string order_id(event.order_id);
        LOG_INFO("Strategy {}: on_order_fill called order_id={} qty={} price={} side={}",
                 name_, order_id, from_qty_t(event.fill_quantity), from_price_t(event.fill_price),
                 event.side == Side::BUY ? "BUY" : "SELL");
        auto cid_it = order_id_to_client_id_.find(order_id);
        if (cid_it != order_id_to_client_id_.end()) {
            auto& order = orders_by_client_id_.at(cid_it->second);
            const qty_t new_filled = order.filled_qty + event.fill_quantity;
            if (new_filled > 0) {
                order.avg_fill_price = to_price_t(
                    (from_price_t(order.avg_fill_price) * from_qty_t(order.filled_qty)
                     + from_price_t(event.fill_price) * from_qty_t(event.fill_quantity))
                    / from_qty_t(new_filled));
            }
            order.filled_qty       = new_filled;
            order.remaining_qty    = event.remaining_quantity;
            order.last_update_time = event.timestamp_ns;
            order.status = (order.remaining_qty == 0)
                               ? OrderStatus::FILLED
                               : OrderStatus::PARTIALLY_FILLED;

            LOG_INFO("Strategy {}: order {} owned - tracking position", name_, order_id);
            if (position_tracker_) {
                position_tracker_->on_fill(name_, event);
            }
            on_fill(event);  // Call derived class hook
        } else {
            LOG_INFO("Strategy {}: fill for order_id={} not owned (map size={})",
                     name_, order_id, order_id_to_client_id_.size());
        }
    }

    void on_order_reject(const OrderRejectEvent& event) override {
        auto it = orders_by_client_id_.find(std::string(event.client_order_id));
        if (it != orders_by_client_id_.end()) {
            it->second.status           = OrderStatus::REJECTED;
            it->second.last_update_time = event.timestamp_ns;
        }
        on_reject(event);
    }

    void on_order_cancel(const OrderCancelEvent& event) override {
        auto cid_it = order_id_to_client_id_.find(std::string(event.order_id));
        if (cid_it != order_id_to_client_id_.end()) {
            auto& order = orders_by_client_id_.at(cid_it->second);
            order.status           = OrderStatus::CANCELLED;
            order.remaining_qty    = event.remaining_quantity;
            order.last_update_time = event.timestamp_ns;
        }
        on_cancel(event);
    }

protected:
    friend class WebServer;        // Allow WebServer to call submit_order
    friend class StrategyManager;  // Allow StrategyManager to call close_all_positions

    // Lifecycle hooks for derived classes
    virtual bool on_initialize() { return true; }
    virtual void on_start() {}
    virtual void on_stop() {}
    virtual void on_shutdown() {}

    // Configuration hook (called when config is set)
    virtual void on_config(const nlohmann::json& params) {}

    // Order lifecycle hooks — override these instead of on_order_ack/reject/cancel
    // Called after order state has been updated in the internal order map
    virtual void on_ack(const OrderAckEvent& event) {}
    virtual void on_reject(const OrderRejectEvent& event) {}
    virtual void on_cancel(const OrderCancelEvent& event) {}

    // Fill hook (called after position tracking and order state update)
    virtual void on_fill(const OrderFillEvent& event) {}

    virtual std::string generate_order_id() {
        return name_ + "_" + std::to_string(get_timestamp_ns());
    }

    // Helper methods for order submission
    std::string submit_order(std::string_view instrument, BrokerId broker_id, Side side, qty_t quantity, OrderType type, price_t price = NULL_PRICE, TimeInForce tif = TimeInForce::FOK, bool post_only = true) {
        if (!order_submission_queue_) {
            LOG_ERROR("Strategy {}: Order submission queue not set", name_);
            return {};
        }

        auto client_order_id = generate_order_id();

        // Track which broker handles each instrument (used by close_all_positions)
        instrument_broker_map_[std::string(instrument)] = broker_id;

        // Pre-trade risk validation
        if (risk_manager_) {
            std::string reject_reason;
            if (!risk_manager_->validate_order(name_, instrument, side, quantity, price, &reject_reason)) {
                LOG_WARN("Strategy {}: Order {} REJECTED by risk: {}",
                         name_, client_order_id, reject_reason);

                // Publish internal reject event back to strategy
                OrderRejectEvent reject;
                reject.timestamp_ns = get_timestamp_ns();
                reject.set_instrument(instrument);
                reject.set_client_order_id(client_order_id.data());
                reject.set_reason(reject_reason.c_str());
                on_order_reject(reject);
                return {};
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

        // Insert PENDING_NEW entry so get_order() works immediately after submit
        Order pending;
        pending.client_order_id  = client_order_id;
        pending.instrument       = std::string(instrument);
        pending.side             = side;
        pending.type             = type;
        pending.price            = price;
        pending.qty              = quantity;
        pending.remaining_qty    = quantity;
        pending.time_in_force    = tif;
        pending.post_only        = post_only;
        pending.status           = OrderStatus::PENDING_NEW;
        pending.created_time     = get_timestamp_ns();
        pending.last_update_time = pending.created_time;
        orders_by_client_id_.emplace(client_order_id, std::move(pending));

        LOG_DEBUG("Strategy {}: Submitted order {} for {} {}@{}",
                 name_, client_order_id, instrument,
                 from_qty_t(quantity), from_price_t(price));
        return client_order_id;
    }

    void cancel_order(const std::string& order_id, const std::string& client_order_id) {
        if (!order_submission_queue_) {
            LOG_ERROR("Strategy {}: Order submission queue not set", name_);
            return;
        }

        EventSerializer::publish<CancelOrderEvent>(*order_submission_queue_, [&](CancelOrderEvent& ev) {
            ev.set_order_id(order_id.c_str());
            ev.set_client_order_id(client_order_id.c_str());
        });
        LOG_DEBUG("Strategy {}: Cancelled order {}", name_, order_id);
    }

    // Cancel all open orders
    void cancel_all_orders() {
        for (const auto& [cid, order] : orders_by_client_id_) {
            if (order.is_open() && !order.order_id.empty()) {
                cancel_order(order.order_id, order.client_order_id);
            }
        }
    }

    void modify_order(Order& order, price_t new_price, qty_t new_quantity, price_t new_stop_price = NULL_PRICE) {
        if (!order_submission_queue_) {
            LOG_ERROR("Strategy {}: Order submission queue not set", name_);
            return;
        }
        
        EventSerializer::publish<ModifyOrderEvent>(*order_submission_queue_, [&order, new_price, new_quantity, new_stop_price](ModifyOrderEvent& ev) {
            ev.set_order_id(order.order_id);
            ev.set_client_order_id(order.client_order_id);
            ev.new_price          = new_price;
            ev.new_quantity       = new_quantity;
            ev.new_stop_price     = new_stop_price;
            ev.new_post_only      = order.post_only;
            ev.new_time_in_force  = order.time_in_force;
        });

        order.status = OrderStatus::PENDING_MODIFY;
        order.last_update_time = get_timestamp_ns();

        LOG_INFO("Strategy {}: Modify order {} new_price={} new_qty={}",
                  name_, order.order_id, from_price_t(new_price), from_qty_t(new_quantity));
    }

    void modify_order(const std::string& client_order_id,
                      price_t new_price,
                      qty_t   new_quantity,
                      price_t new_stop_price = NULL_PRICE) {
        auto it = orders_by_client_id_.find(client_order_id);
        if (it != orders_by_client_id_.end()) [[likely]] {
            modify_order(it->second, new_price, new_quantity, new_stop_price);
        }
    }

    // Emergency close: cancel open orders then submit aggressive limit orders to flatten all positions.
    // Bypasses risk manager validation — this is a safety operation triggered by risk breach.
    // Limit prices are set far into the market (10% away from mark price) to ensure execution.
    void close_all_positions() {
        cancel_all_orders();

        if (!position_tracker_ || !order_submission_queue_) return;

        const auto positions = position_tracker_->get_all_positions(name_);
        for (const auto& [instrument, pos] : positions) {
            if (pos.is_flat()) continue;

            auto broker_it = instrument_broker_map_.find(instrument);
            if (broker_it == instrument_broker_map_.end()) {
                LOG_WARN("Strategy {}: Cannot close position for {} — no broker mapping known",
                         name_, instrument);
                continue;
            }

            // Determine closing side and quantity
            const Side close_side = pos.is_long() ? Side::SELL : Side::BUY;
            const qty_t close_qty = pos.is_long() ? pos.quantity : -pos.quantity;

            // Use mark price if available, otherwise fall back to avg entry price
            price_t ref_price = pos.avg_price;
            if (pnl_calculator_ && pnl_calculator_->has_mark_price(instrument)) {
                ref_price = pnl_calculator_->get_mark_price(instrument);
            }

            // Place limit price 10% into the market to guarantee execution
            const double ref_double = from_price_t(ref_price);
            const price_t close_price = pos.is_long()
                ? to_price_t(ref_double * 0.90)  // Sell well below mark — crosses any bid
                : to_price_t(ref_double * 1.10); // Buy well above mark  — crosses any ask

            const BrokerId broker_id = broker_it->second;
            const auto client_order_id = generate_order_id();

            LOG_WARN("Strategy {}: Emergency close {} qty={} limit@{} (ref={})",
                     name_, instrument, from_qty_t(close_qty),
                     from_price_t(close_price), from_price_t(ref_price));

            // Submit directly — skip risk manager for safety closure
            EventSerializer::publish<NewOrderEvent>(*order_submission_queue_,
                [&](NewOrderEvent& order) {
                    order.broker_id     = broker_id;
                    order.set_instrument(instrument.c_str());
                    order.set_client_order_id(client_order_id.c_str());
                    order.side          = close_side;
                    order.order_type    = OrderType::LIMIT;
                    order.price         = close_price;
                    order.quantity      = close_qty;
                    order.time_in_force = TimeInForce::IOC;
                    order.post_only     = false;
                });

            // Track as PENDING_NEW so get_order() works immediately
            Order pending;
            pending.client_order_id  = client_order_id;
            pending.instrument       = instrument;
            pending.side             = close_side;
            pending.type             = OrderType::LIMIT;
            pending.price            = close_price;
            pending.qty              = close_qty;
            pending.remaining_qty    = close_qty;
            pending.time_in_force    = TimeInForce::IOC;
            pending.post_only        = false;
            pending.status           = OrderStatus::PENDING_NEW;
            pending.created_time     = get_timestamp_ns();
            pending.last_update_time = pending.created_time;
            orders_by_client_id_.emplace(client_order_id, std::move(pending));
        }
    }

    // Returns all orders (open and done) for this strategy
    const std::unordered_map<std::string, Order>& get_orders() const {
        return orders_by_client_id_;
    }

    // Lookup by client_order_id (available immediately after submit_order)
    const Order* get_order(const std::string& client_order_id) const {
        auto it = orders_by_client_id_.find(client_order_id);
        return it != orders_by_client_id_.end() ? &it->second : nullptr;
    }

    // Lookup by broker-assigned order_id (available after on_order_ack)
    const Order* get_order_by_id(const std::string& order_id) const {
        auto it = order_id_to_client_id_.find(order_id);
        if (it == order_id_to_client_id_.end()) return nullptr;
        auto oit = orders_by_client_id_.find(it->second);
        return oit != orders_by_client_id_.end() ? &oit->second : nullptr;
    }

    // Returns pointers to all open orders
    std::vector<const Order*> get_open_orders() const {
        std::vector<const Order*> result;
        for (const auto& [cid, order] : orders_by_client_id_) {
            if (order.is_open()) result.push_back(&order);
        }
        return result;
    }

    // Remove done (filled/cancelled/rejected) orders from the map
    void prune_done_orders() {
        for (auto it = orders_by_client_id_.begin(); it != orders_by_client_id_.end(); ) {
            if (it->second.is_done()) {
                order_id_to_client_id_.erase(it->second.order_id);
                it = orders_by_client_id_.erase(it);
            } else {
                ++it;
            }
        }
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

    // Order tracking — keyed by client_order_id; reverse map for broker order_id lookup
    std::unordered_map<std::string, Order> orders_by_client_id_;
    std::unordered_map<std::string, std::string> order_id_to_client_id_;

    // Maps instrument -> last used BrokerId (populated in submit_order, used by close_all_positions)
    std::unordered_map<std::string, BrokerId> instrument_broker_map_;
};

} // namespace slick::trader
