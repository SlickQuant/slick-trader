#include <slick-trader/brokers/coinbase/coinbase_order_callbacks.hpp>
#include <slick-trader/brokers/coinbase/coinbase_order_connector.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick-trader/utils/types.hpp>
#include <slick/logger.hpp>
#include <cassert>
#include <chrono>
#include <utility>

namespace slick::trader {


CoinbaseOrderCallbacks::CoinbaseOrderCallbacks(CoinbaseOrderConnector &connector)
    : event_buffer_(connector.event_buffer())
    , connector_(connector) 
{}

void CoinbaseOrderCallbacks::onUserDataConnected(WebSocketClient* client) {
    LOG_INFO("Coinbase User WebSocket {:p} connected", static_cast<void*>(client));
}

void CoinbaseOrderCallbacks::onUserDataDisconnected(WebSocketClient* client) {
    LOG_INFO("Coinbase User WebSocket {:p} disconnected", static_cast<void*>(client));
    if (connector_.is_running()) {
        LOG_WARN("Coinbase User WebSocket {:p} disconnected while running - reconnecting", static_cast<void*>(client));
        connector_.connect();
    }
}

void CoinbaseOrderCallbacks::onUserDataGap(WebSocketClient* client) {
    LOG_WARN("Coinbase User WebSeocket {:p} gap detected - some order updates might be lost", static_cast<void*>(client));
}

void CoinbaseOrderCallbacks::onUserDataSnapshot(
    WebSocketClient* /* client */, uint64_t /* seq_num */, const std::vector<coinbase::Order>& orders,
    const std::vector<coinbase::PerpetualFuturePosition>& perpetual_future_positions,
    const std::vector<coinbase::ExpiringFuturePosition>& expiring_future_positions) {
    LOG_DEBUG("Received user data snapshot with {} orders, {} perpetual future positions, {} expiring future positions", orders.size(), perpetual_future_positions.size(), expiring_future_positions.size());
    for (const auto& order : orders) {
        process_order_update(order);
    }

    for (const auto& pos : expiring_future_positions) {
        LOG_DEBUG("Expiring future position: {} {} contracts, entry price {}, unrealized PnL {}", pos.product_id, pos.number_of_contracts, pos.entry_price, pos.unrealized_pnl);
    }
}

void CoinbaseOrderCallbacks::onOrderUpdates(
    WebSocketClient* /* client */, uint64_t /* seq_num */, const std::vector<coinbase::Order>& orders) {
    for (const auto& order : orders) {
        process_order_update(order);
    }
}

void CoinbaseOrderCallbacks::onUserDataError(WebSocketClient* client, std::string&& err) {
    LOG_ERROR("WebSocket {:p} user data error: {}", static_cast<void*>(client), std::move(err));
    connector_.disconnect();
    connector_.connect();
}

void CoinbaseOrderCallbacks::process_order_update(const coinbase::Order& order) {
    switch (order.status) {
        case coinbase::OrderStatus::PENDING:
        case coinbase::OrderStatus::QUEUED:
            LOG_INFO("{} Order {} pending/queued: {} @ {}", order.product_id, order.order_id, order.cumulative_quantity, order.limit_price);
            publish_order_ack(order);
            break;
        case coinbase::OrderStatus::OPEN: {
            LOG_INFO("{} Order {} acknowledged: {}@{}, cum_qty={}", order.product_id, order.order_id, order.leaves_quantity + order.cumulative_quantity, order.limit_price, order.cumulative_quantity);
            publish_order_ack(order);
            // Emit a fill event if a new partial fill has occurred since last update
            if (order.number_of_fills > fill_states_[order.order_id].prev_number_of_fills) {
                publish_order_fill(order);
            }
            break;
        }
        case coinbase::OrderStatus::FILLED:
            publish_order_fill(order);
            fill_states_.erase(order.order_id);
            break;
        case coinbase::OrderStatus::CANCELLED:
        case coinbase::OrderStatus::EXPIRED:
            LOG_INFO("{} Order {} cancelled/expired", order.product_id, order.order_id);
            publish_order_cancel(order);
            fill_states_.erase(order.order_id);
            break;
        case coinbase::OrderStatus::CANCEL_QUEUED:
            LOG_INFO("{} Order {} cancel queued", order.product_id, order.order_id);
            break;
        case coinbase::OrderStatus::EDIT_QUEUED:
            LOG_INFO("{} Order {} edit queued (modify in progress)", order.product_id, order.order_id);
            break;
        case coinbase::OrderStatus::FAILED:
            publish_order_reject(order);
            break;
        default:
            LOG_WARN("Unknown order status: {}", order.status);
            break;
    }
}

void CoinbaseOrderCallbacks::publish_order_ack(const coinbase::Order& order) {
    double price = 0.0;
    double stop_price = 0.0;
    double quantity = 0.0;

    if (order.order_configuration.limit_limit_gtc.has_value()) {
        price = order.order_configuration.limit_limit_gtc->limit_price;
        if (order.order_configuration.limit_limit_gtc->base_size.has_value()) {
            quantity = order.order_configuration.limit_limit_gtc->base_size.value();
        }
    } else if (order.order_configuration.stop_limit_stop_limit_gtc.has_value()) {
        price = order.order_configuration.stop_limit_stop_limit_gtc->limit_price;
        stop_price = order.order_configuration.stop_limit_stop_limit_gtc->stop_price;
        if (order.order_configuration.stop_limit_stop_limit_gtc->base_size.has_value()) {
            quantity = order.order_configuration.stop_limit_stop_limit_gtc->base_size.value();
        }
    } else {
        price = order.limit_price;
        quantity = order.cumulative_quantity + order.leaves_quantity;
    }

    EventSerializer::publish<OrderAckEvent>(*event_buffer_, [this, &order, price, stop_price, quantity](OrderAckEvent& event) {
        event.type = EventType::ORDER_ACK;
        event.broker_id = BrokerId::COINBASE;
        event.timestamp_ns = get_timestamp_ns();
        event.event_time_ns = order.created_time;
        event.set_instrument(order.product_id);
        event.set_order_id(order.order_id);
        event.set_client_order_id(order.client_order_id);
        event.side = static_cast<Side>(order.side);
        event.price = to_price_t(price);
        event.stop_price = to_price_t(stop_price);
        event.quantity = to_qty_t(quantity);
        event.order_type = static_cast<OrderType>(order.order_type);
        event.status = map_order_status(order.status);
    });
}

void CoinbaseOrderCallbacks::publish_order_fill(const coinbase::Order& order) {
    auto& state = fill_states_[order.order_id];

    // Coinbase user channel avg_price can be 0, stale, or unreliably scaled.
    // filled_value / cumulative_quantity is always the correct weighted average (total notional / total qty).
    double cum_avg_price = (order.cumulative_quantity > 0.0 && order.filled_value > 0.0)
        ? order.filled_value / order.cumulative_quantity
        : order.avg_price;
    double prev_filled_value = state.prev_avg_price * state.prev_cumulative_qty;

    // Compute the incremental fill qty and price from cumulative data.
    // Since avg_price is a weighted average, the last fill price can be recovered exactly:
    //   last_price = (avg_N * cum_N - avg_{N-1} * cum_{N-1}) / last_qty
    double last_qty = order.cumulative_quantity - state.prev_cumulative_qty;
    double last_price;
    if (last_qty > 0.0) {
        last_price = (cum_avg_price * order.cumulative_quantity - prev_filled_value) / last_qty;
    } else {
        last_price = cum_avg_price;
    }

    // Sanity check: skip publishing if no new quantity was filled or price is invalid.
    // Do NOT update state in this case — a bogus cum_avg_price (e.g. NULL_PRICE sentinel
    // sent by the exchange) must not corrupt prev_avg_price for subsequent fills.
    // Coinbase sends INT64_MAX/1e8 (~9.22e10) as a null sentinel when avg_price is unavailable;
    // cap at 1e9 ($1 billion per unit) to reject it while allowing any realistic asset price.
    constexpr double MAX_CREDIBLE_PRICE = 1.0e9;
    if (last_qty <= 0.0 || last_price <= 0.0 || last_price > MAX_CREDIBLE_PRICE) {
        LOG_WARN("{} Order {} skipping fill event: invalid last_qty={} or last_price={} (cum_qty={}, cum_avg={}, prev_filled_value={})",
                 order.product_id, order.order_id, last_qty, last_price,
                 order.cumulative_quantity, cum_avg_price, prev_filled_value);
        return;
    }

    // Compute incremental fee for this fill (Coinbase total_fees is cumulative per order)
    const double incremental_fee = order.total_fees - state.prev_total_fees;

    // Update state (captures this fill as the baseline for the next incremental fill)
    state.prev_cumulative_qty  = order.cumulative_quantity;
    state.prev_avg_price       = cum_avg_price;
    state.prev_number_of_fills = order.number_of_fills;
    state.prev_total_fees      = order.total_fees;

    LOG_INFO("{} Order {} filled: {}@{}, cum_qty={}, avg_price={}({}), fee={}", order.product_id, order.order_id, last_qty, last_price, order.cumulative_quantity, cum_avg_price, order.avg_price, incremental_fee);

    EventSerializer::publish<OrderFillEvent>(*event_buffer_, [&order, last_qty, last_price, incremental_fee](OrderFillEvent& event) {
        event.type = EventType::ORDER_FILL;
        event.broker_id = BrokerId::COINBASE;
        event.timestamp_ns = get_timestamp_ns();
        event.event_time_ns = order.last_fill_time > 0 ? order.last_fill_time : order.created_time;
        event.set_instrument(order.product_id);
        event.set_order_id(order.order_id);
        event.set_client_order_id(order.client_order_id);
        event.set_fill_id(std::to_string(order.number_of_fills));
        event.side = static_cast<Side>(order.side);
        event.fill_price         = to_price_t(last_price);                  // actual execution price
        event.fill_quantity      = to_qty_t(last_qty);                      // incremental fill qty
        event.cumulative_filled  = to_qty_t(order.cumulative_quantity);
        event.remaining_quantity = to_qty_t(order.leaves_quantity);
        event.fee_amount         = incremental_fee > 0.0 ? to_price_t(incremental_fee) : 0;
    });
}

void CoinbaseOrderCallbacks::publish_order_reject(const coinbase::Order& order) {
    EventSerializer::publish<OrderRejectEvent>(*event_buffer_, [&order](OrderRejectEvent& event) {
        event.type = EventType::ORDER_REJECT;
        event.broker_id = BrokerId::COINBASE;
        event.timestamp_ns = get_timestamp_ns();
        event.event_time_ns = order.created_time;
        event.set_instrument(order.product_id);
        event.set_order_id(order.order_id);
        event.set_client_order_id(order.client_order_id);
        event.set_reason(order.reject_message.empty() ? order.reject_Reason : order.reject_message);
        event.side = static_cast<Side>(order.side);
    });
}

void CoinbaseOrderCallbacks::publish_order_cancel(const coinbase::Order& order) {
    EventSerializer::publish<OrderCancelEvent>(*event_buffer_, [&order](OrderCancelEvent& event) {
        event.type = EventType::ORDER_CANCEL;
        event.broker_id = BrokerId::COINBASE;
        event.timestamp_ns = get_timestamp_ns();
        event.event_time_ns = order.created_time;
        event.set_instrument(order.product_id);
        event.set_order_id(order.order_id);
        event.set_client_order_id(order.client_order_id);
        event.set_reason(order.cancel_message);
        event.side = static_cast<Side>(order.side);
        event.remaining_quantity = to_qty_t(order.leaves_quantity);
    });
}

OrderStatus CoinbaseOrderCallbacks::map_order_status(coinbase::OrderStatus status) {
    switch (status) {
        case coinbase::OrderStatus::PENDING:
        case coinbase::OrderStatus::QUEUED:
            return OrderStatus::PENDING_NEW;
        case coinbase::OrderStatus::OPEN:
            return OrderStatus::ACKNOWLEDGED;
        case coinbase::OrderStatus::FILLED:
            return OrderStatus::FILLED;
        case coinbase::OrderStatus::CANCELLED:
        case coinbase::OrderStatus::EXPIRED:
            return OrderStatus::CANCELLED;
        case coinbase::OrderStatus::FAILED:
            return OrderStatus::REJECTED;
        default:
            return OrderStatus::REJECTED;
    }
}

} // namespace slick::trader
