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

void CoinbaseOrderCallbacks::set_order_event_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> order_event_queue) {
    order_event_queue_ = std::move(order_event_queue);
}

void CoinbaseOrderCallbacks::onMarketDataConnected(WebSocketClient* /* client */) {
}

void CoinbaseOrderCallbacks::onUserDataConnected(WebSocketClient* client) {
    LOG_INFO("WebSocket {:p} user data connected", static_cast<void*>(client));
}

void CoinbaseOrderCallbacks::onMarketDataDisconnected(WebSocketClient* /* client */) {
}

void CoinbaseOrderCallbacks::onUserDataDisconnected(WebSocketClient* client) {
    LOG_INFO("WebSocket {:p} user data disconnected", static_cast<void*>(client));
    if (connector_.is_running()) {
        LOG_WARN("WebSocket {:p} user data disconnected while running - attempting to reconnect", static_cast<void*>(client));
        connector_.connect();
    }
}

void CoinbaseOrderCallbacks::onLevel2Snapshot(
    WebSocketClient* /* client */, uint64_t /* seq_num */, const coinbase::Level2UpdateBatch& /* snapshot */) {
}

void CoinbaseOrderCallbacks::onLevel2Updates(
    WebSocketClient* /* client */, uint64_t /* seq_num */, const coinbase::Level2UpdateBatch& /* updates */) {
}

void CoinbaseOrderCallbacks::onMarketTradesSnapshot(
    WebSocketClient* /* client */, uint64_t /* seq_num */, const std::vector<coinbase::MarketTrade>& /* snapshots */) {
}

void CoinbaseOrderCallbacks::onMarketTrades(
    WebSocketClient* /* client */, uint64_t /* seq_num */, const std::vector<coinbase::MarketTrade>& /* trades */) {
}

void CoinbaseOrderCallbacks::onTickerSnapshot(
    WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Ticker>& /* tickers */) {
}

void CoinbaseOrderCallbacks::onTickers(
    WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Ticker>& /* tickers */) {
}

void CoinbaseOrderCallbacks::onCandlesSnapshot(
    WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Candle>& /* candles */) {
}

void CoinbaseOrderCallbacks::onCandles(
    WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Candle>& /* candles */) {
}

void CoinbaseOrderCallbacks::onStatusSnapshot(
    WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Status>& /* status */) {
}

void CoinbaseOrderCallbacks::onStatus(
    WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Status>& /* status */) {
}

void CoinbaseOrderCallbacks::onMarketDataGap(WebSocketClient* /* client */) {
}

void CoinbaseOrderCallbacks::onUserDataGap(WebSocketClient* client) {
    LOG_WARN("WebSeocket {:p} user data gap detected - some order updates were lost", static_cast<void*>(client));
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

void CoinbaseOrderCallbacks::onMarketDataError(WebSocketClient* /* client */, std::string&& /* err */) {
}

void CoinbaseOrderCallbacks::onUserDataError(WebSocketClient* client, std::string&& err) {
    LOG_ERROR("WebSocket {:p} user data error: {}", static_cast<void*>(client), std::move(err));
}

void CoinbaseOrderCallbacks::process_order_update(const coinbase::Order& order) {
    assert(order_event_queue_ && "Order event queue not set in CoinbaseOrderCallbacks");

    switch (order.status) {
        case coinbase::OrderStatus::PENDING:
        case coinbase::OrderStatus::QUEUED:
            publish_order_ack(order);
            break;
        case coinbase::OrderStatus::OPEN:
            if (order.cumulative_quantity > 0) {
                publish_order_fill(order);
            } else {
                publish_order_ack(order);
            }
            break;
        case coinbase::OrderStatus::FILLED:
            publish_order_fill(order);
            break;
        case coinbase::OrderStatus::CANCELLED:
        case coinbase::OrderStatus::EXPIRED:
            publish_order_cancel(order);
            break;
        case coinbase::OrderStatus::FAILED:
            publish_order_reject(order);
            break;
        default:
            LOG_WARN("Unknown order status: {}({})", order.status, coinbase::to_string(order.status));
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
    }

    EventSerializer::publish<OrderAckEvent>(*order_event_queue_, [this, &order, price, stop_price, quantity](OrderAckEvent& event) {
        event.type = EventType::ORDER_ACK;
        event.broker_id = BrokerId::COINBASE;
        event.timestamp_ns = std::chrono::system_clock::now().time_since_epoch().count();
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
    EventSerializer::publish<OrderFillEvent>(*order_event_queue_, [&order](OrderFillEvent& event) {
        event.type = EventType::ORDER_FILL;
        event.broker_id = BrokerId::COINBASE;
        event.timestamp_ns = std::chrono::system_clock::now().time_since_epoch().count();
        event.event_time_ns = order.last_fill_time > 0 ? order.last_fill_time : order.created_time;
        event.set_instrument(order.product_id);
        event.set_order_id(order.order_id);
        event.set_client_order_id(order.client_order_id);
        event.set_fill_id(std::to_string(order.number_of_fills));
        event.side = static_cast<Side>(order.side);
        event.fill_price = to_price_t(order.avg_price);
        event.fill_quantity = to_qty_t(order.cumulative_quantity);
        event.cumulative_filled = to_qty_t(order.cumulative_quantity);
        event.remaining_quantity = to_qty_t(order.leaves_quantity);
    });
}

void CoinbaseOrderCallbacks::publish_order_reject(const coinbase::Order& order) {
    EventSerializer::publish<OrderRejectEvent>(*order_event_queue_, [&order](OrderRejectEvent& event) {
        event.type = EventType::ORDER_REJECT;
        event.broker_id = BrokerId::COINBASE;
        event.timestamp_ns = std::chrono::system_clock::now().time_since_epoch().count();
        event.event_time_ns = order.created_time;
        event.set_instrument(order.product_id);
        event.set_order_id(order.order_id);
        event.set_client_order_id(order.client_order_id);
        event.set_reason(order.reject_message.empty() ? order.reject_Reason : order.reject_message);
        event.side = static_cast<Side>(order.side);
    });
}

void CoinbaseOrderCallbacks::publish_order_cancel(const coinbase::Order& order) {
    EventSerializer::publish<OrderCancelEvent>(*order_event_queue_, [&order](OrderCancelEvent& event) {
        event.type = EventType::ORDER_CANCEL;
        event.broker_id = BrokerId::COINBASE;
        event.timestamp_ns = std::chrono::system_clock::now().time_since_epoch().count();
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
