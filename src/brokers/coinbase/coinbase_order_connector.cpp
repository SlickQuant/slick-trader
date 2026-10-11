#include <slick-trader/brokers/coinbase/coinbase_order_connector.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick-trader/events/register_event.hpp>
#include <slick-trader/core/broker_status.hpp>
#include <slick/logger.hpp>
#include <coinbase/websocket.hpp>
#include <coinbase/rest.hpp>
#include <coinbase/common.hpp>
#include <cassert>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace slick::trader {

inline coinbase::OrderType to_coinbase_order_type(OrderType type) {
    switch (type) {
        case OrderType::MARKET:
            return coinbase::OrderType::MARKET;
        case OrderType::LIMIT:
            return coinbase::OrderType::LIMIT;
        case OrderType::STOP_LIMIT:
            return coinbase::OrderType::STOP_LIMIT;
        case OrderType::STOP:
            return coinbase::OrderType::STOP;
        case OrderType::BRACKET:
            return coinbase::OrderType::BRACKET;
        default:
            throw std::invalid_argument("Unsupported order type");
    }
    // return coinbase::OrderType::UNKNOWN_ORDER_TYPE;
}

inline coinbase::TimeInForce to_coinbase_time_in_force(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTD:
            return coinbase::TimeInForce::GOOD_UNTIL_DATE_TIME;
        case TimeInForce::GTC:
            return coinbase::TimeInForce::GOOD_UNTIL_CANCELLED;
        case TimeInForce::IOC:
            return coinbase::TimeInForce::IMMEDIATE_OR_CANCEL;
        case TimeInForce::FOK:
            return coinbase::TimeInForce::FILL_OR_KILL;
        default:
            throw std::invalid_argument("Unsupported time in force");
    }
    // return coinbase::TimeInForce::UNKNOWN_TIME_IN_FORCE;
}

CoinbaseOrderConnector::CoinbaseOrderConnector(
    const json& config,
    slick::stream_buffer_multiplexer &mux,
    slick::queue<uint8_t>& control_queue,
    slick::queue<uint8_t>& request_queue
)
    : enabled_(config.empty() ? false : config.value("enabled", true))
    , shm_name_(config.value("callback_shm_name", ""))
    , event_buffer_(mux.add_producer(
        mux.producer_count(),
        config.value("queue_size", 1u << 24),
        config.value("control_size", 1u << 18),
        shm_name_.empty() ? nullptr : shm_name_.c_str()))
    , control_queue_(control_queue)
    , request_queue_(request_queue)
    , request_cursor_(request_queue.initial_reading_index())
{
    BrokerStatusRegistry::instance().set_order_enabled(BrokerId::COINBASE, enabled_);
    if (!enabled_) {
        LOG_INFO("Coinbase Order Connector disabled");
    } else {
        auto rest_url = config.value("rest_url", "https://api.coinbase.com");
        LOG_INFO("Coinbase Order REST: {}", rest_url);
        rest_client_ = std::make_unique<coinbase::CoinbaseRestClient>(rest_url);
        ws_client_ = std::make_unique<coinbase::WebSocketClient>(
            this,
            "",
            config.value("websocket_url", "wss://advanced-trade-ws-user.coinbase.com"),
            nullptr, // not using shm
            config.value("queue_size", 1u << 24),
            config.value("control_size", 32)
        );
    }
}

CoinbaseOrderConnector::~CoinbaseOrderConnector() {
    stop();
}

void CoinbaseOrderConnector::register_order_connector() {
    if (enabled_) {
        auto taker_rate = get_taker_rate();
        EventSerializer::publish<OrderConnectorRegisterEvent>(control_queue_, [=](OrderConnectorRegisterEvent& event) {
            event.broker_id = BrokerId::COINBASE;
            event.producer_id = event_buffer_->producer_id();
            event.fee_structure.type = FeeType::PERCENTAGE;
            event.fee_structure.rate = taker_rate;
            std::memcpy(event.shm_name, shm_name_.data(), sizeof(event.shm_name));
        });
    }
}

bool CoinbaseOrderConnector::connect() {
    if (!enabled_ || !ws_client_) {
        return false;
    }

    LOG_INFO("Coinbase Order: Connecting to WS: {}", ws_client_->userDataUrl());

    // Subscribe to USER channel for order updates
    ws_client_->subscribe({}, {coinbase::WebSocketChannel::USER});
    return true;
}

void CoinbaseOrderConnector::disconnect() {
    if (ws_client_) {
        ws_client_->stop();
    }
    LOG_INFO("Coinbase Order: Disconnecting");
}

bool CoinbaseOrderConnector::is_connected() const { 
    return ws_client_ ? ws_client_->isUserDataConnected() : false;
}

double CoinbaseOrderConnector::get_taker_rate() const {
    if (!rest_client_) return 0.0;
    return rest_client_->get_taker_fee_rate();
}

void CoinbaseOrderConnector::start() {
    if (!enabled_) {
        return;
    }

    if (running_.load(std::memory_order_relaxed)) [[unlikely]] {
        LOG_WARN("Coinbase Order: Already running");
        return;
    }

    LOG_INFO("Coinbase Order: Starting");
    running_.store(true, std::memory_order_release);
    connect();
    worker_thread_ = std::thread(&CoinbaseOrderConnector::run, this);
    LOG_INFO("Coinbase Order: Started");
}

void CoinbaseOrderConnector::stop() {
    if (!running_.load(std::memory_order_relaxed)) {
        return;
    }

    LOG_INFO("Coinbase Order: Stopping");
    disconnect();

    running_.store(false, std::memory_order_release);

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    if (rest_client_) {
        rest_client_.reset();
    }

    LOG_INFO("Coinbase Order: Stopped");
}

void CoinbaseOrderConnector::run() {
    assert(enabled_);

    LOG_INFO("Coinbase Order: Worker thread started");

    while (running_.load(std::memory_order_acquire)) {
        processData();
        process_order_requests();
    }

    LOG_INFO("Coinbase Order: Worker thread stopped");
}

void CoinbaseOrderConnector::process_order_requests() {
    assert(rest_client_ && "REST client not initialized");

    auto [data, size] = request_queue_.read(request_cursor_);

    if (!data) {
        std::this_thread::yield();
        return;
    }

    const auto *event = reinterpret_cast<const Event*>(data);
    if (!is_request_for(*event, BrokerId::COINBASE)) {
        return;   // another broker's order
    }
    switch (event->type) {
        case EventType::NEW_ORDER:
            handle_order_request(*reinterpret_cast<NewOrderEvent*>(data));
            break;
        case EventType::MODIFY_ORDER:
            handle_modify_request(*reinterpret_cast<ModifyOrderEvent*>(data));
            break;
        case EventType::CANCEL_ORDER:
            handle_cancel_request(*reinterpret_cast<CancelOrderEvent*>(data));
            break;
        default:
            LOG_ERROR("Coinbase Order: Unknown event type: {}", static_cast<int>(event->type));
            break;
    }
}

void CoinbaseOrderConnector::handle_order_request(const NewOrderEvent& request) {
    // Converted inside the try: an order type or time in force Coinbase does not support
    // (DAY) throws, and must become a reject rather than escape the order thread.
    try {
        // Convert SlickTrader order types to Coinbase types
        coinbase::Side cb_side = static_cast<coinbase::Side>(request.side);
        coinbase::OrderType cb_order_type = to_coinbase_order_type(request.order_type);
        coinbase::TimeInForce cb_tif = to_coinbase_time_in_force(request.time_in_force);

        LOG_INFO("Processing new order request: client_order_id={}, instrument={}, side={}, type={}, tif={}, {}@{}",
                request.client_order_id, request.instrument,
                coinbase::to_string(cb_side), coinbase::to_string(cb_order_type),
                coinbase::to_string(cb_tif), from_qty_t(request.quantity), 
                request.price == NULL_PRICE ? "NULL_PRICE" : std::to_string(from_price_t(request.price)));

        // Submit order via REST API
        auto response = rest_client_->create_order(
            std::string(request.client_order_id),
            std::string(request.instrument),
            cb_side,
            cb_order_type,
            cb_tif,
            from_qty_t(request.quantity),
            from_price_t(request.price),
            request.post_only,
            false, // size_in_quote
            request.stop_price != NULL_PRICE ? std::optional<double>(from_price_t(request.stop_price)) : std::nullopt
        );

        if (response.success) {
            LOG_INFO("Order submitted successfully: order_id={}, client_order_id={}",
                        response.success_response.order_id,
                        response.success_response.client_order_id);
            EventSerializer::publish<OrderAckEvent>(*event_buffer_, [&request, &response](OrderAckEvent &event) {
                event.type = EventType::ORDER_ACK;
                event.broker_id = BrokerId::COINBASE;
                event.timestamp_ns = get_timestamp_ns();
                event.set_instrument(request.instrument);
                event.set_client_order_id(response.success_response.client_order_id);
                event.set_order_id(response.success_response.order_id);
                event.side = static_cast<Side>(response.success_response.side);
                event.price = request.price;
                event.stop_price = request.stop_price;
                event.quantity = request.quantity;
                event.order_type = request.order_type;
                event.time_in_force = request.time_in_force;
                event.post_only = request.post_only;
                event.status = OrderStatus::PENDING_NEW;
            });
        } else {
            LOG_ERROR("Order submission failed: {}", response.error_response.message);
                EventSerializer::publish<OrderRejectEvent>(*event_buffer_, [&request, &response](OrderRejectEvent &reject_event) {
                reject_event.type = EventType::ORDER_REJECT;
                reject_event.broker_id = BrokerId::COINBASE;
                reject_event.timestamp_ns = get_timestamp_ns();
                reject_event.set_instrument(request.instrument);
                reject_event.set_client_order_id(request.client_order_id);
                reject_event.set_reason(response.error_response.message);
                reject_event.side = request.side;
                reject_event.price = request.price;
                reject_event.stop_price = request.stop_price;
                reject_event.quantity = request.quantity;
            });
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception submitting order: {}", e.what());
        // Publish reject event
        EventSerializer::publish<OrderRejectEvent>(*event_buffer_, [&request, &e](OrderRejectEvent &reject_event) {
            reject_event.type = EventType::ORDER_REJECT;
            reject_event.broker_id = BrokerId::COINBASE;
            reject_event.timestamp_ns = get_timestamp_ns();
            reject_event.set_instrument(request.instrument);
            reject_event.set_client_order_id(request.client_order_id);
            reject_event.set_reason(e.what());
            reject_event.side = request.side;
            reject_event.price = request.price;
            reject_event.stop_price = request.stop_price;
            reject_event.quantity = request.quantity;
        });
    }
}

void CoinbaseOrderConnector::handle_modify_request(const ModifyOrderEvent& request) {
    LOG_INFO("Processing modify request: order_id={}, client_order_id={}, new_price={}, new_qty={}",
             request.order_id, request.client_order_id,
             from_price_t(request.new_price), from_qty_t(request.new_quantity));

    auto publish_reject = [&](std::string_view reason) {
        EventSerializer::publish<OrderRejectEvent>(*event_buffer_, [&](OrderRejectEvent& ev) {
            ev.type         = EventType::ORDER_REJECT;
            ev.broker_id    = BrokerId::COINBASE;
            ev.timestamp_ns = get_timestamp_ns();
            ev.set_instrument(request.instrument);
            ev.set_client_order_id(request.client_order_id);
            ev.set_order_id(request.order_id);
            ev.set_reason(reason.data());
            ev.side         = request.side;
            ev.price        = request.new_price;
            ev.stop_price   = request.new_stop_price;
            ev.quantity     = request.new_quantity;
        });
    };

    try {
        std::optional<double> stop_opt = (request.new_stop_price != NULL_PRICE)
            ? std::optional<double>(from_price_t(request.new_stop_price))
            : std::nullopt;

        auto response = rest_client_->modify_order(
            std::string(request.order_id),
            std::string(request.instrument),
            from_price_t(request.new_price),
            from_qty_t(request.new_quantity),
            stop_opt
        );

        if (response.success) {
            LOG_INFO("Order modified successfully: order_id={}", request.order_id);
            EventSerializer::publish<OrderAckEvent>(*event_buffer_, [&](OrderAckEvent& ev) {
                ev.type          = EventType::ORDER_ACK;
                ev.broker_id     = BrokerId::COINBASE;
                ev.timestamp_ns  = get_timestamp_ns();
                ev.set_instrument(request.instrument);
                ev.set_client_order_id(request.client_order_id);
                ev.set_order_id(request.order_id);
                ev.side          = request.side;
                ev.price         = request.new_price;
                ev.stop_price    = request.new_stop_price;
                ev.quantity      = request.new_quantity;
                ev.time_in_force = request.new_time_in_force;
                ev.post_only     = request.new_post_only;
                ev.status        = OrderStatus::ACKNOWLEDGED;
            });
        } else {
            std::string reason = !response.errors.empty()
                ? response.errors[0].dump()
                : "Modify order rejected by exchange";
            LOG_ERROR("Order modify failed: order_id={}, reason={}", request.order_id, reason);
            publish_reject(reason);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception modifying order {}: {}", request.order_id, e.what());
        publish_reject(e.what());
    }
}

void CoinbaseOrderConnector::handle_cancel_request(const CancelOrderEvent& request) {
    LOG_INFO("Processing cancel request: order_id={}, client_order_id={}",
             request.order_id, request.client_order_id);

    auto publish_cancel = [&](std::string_view reason) {
        EventSerializer::publish<OrderCancelEvent>(*event_buffer_, [&](OrderCancelEvent& ev) {
            ev.broker_id    = BrokerId::COINBASE;
            ev.timestamp_ns = get_timestamp_ns();
            ev.set_order_id(request.order_id);
            ev.set_client_order_id(request.client_order_id);
            ev.set_reason(reason.data());
            ev.remaining_quantity = 0;
        });
    };

    try {
        auto responses = rest_client_->cancel_orders({ request.order_id });

        for (const auto& response : responses) {
            if (response.success) {
                LOG_INFO("Order cancelled successfully: order_id={}", response.order_id);
                publish_cancel("CANCELLED");
            } else {
                LOG_ERROR("Order cancellation failed: order_id={}, reason={}",
                          response.order_id, response.failure_reason);
                // Coinbase responded but says the order is gone (filled, expired, not found).
                // Publish a cancel event so WebServer & strategy remove the order locally.
                // Note: we do NOT publish on network/parse exceptions since the order may still
                // be live on the exchange in that case.
                publish_cancel(response.failure_reason);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception cancelling order {}: {}", request.order_id, e.what());
    }
}

void CoinbaseOrderConnector::onUserDataConnected(WebSocketClient* client) {
    LOG_INFO("Coinbase User WebSocket {:p} connected", static_cast<void*>(client));
    BrokerStatusRegistry::instance().set_order_connected(BrokerId::COINBASE, true);
}

void CoinbaseOrderConnector::onUserDataDisconnected(WebSocketClient* client) {
    LOG_INFO("Coinbase User WebSocket {:p} disconnected", static_cast<void*>(client));
    BrokerStatusRegistry::instance().set_order_connected(BrokerId::COINBASE, false);
    if (is_running()) {
        LOG_WARN("Coinbase User WebSocket {:p} disconnected while running - reconnecting", static_cast<void*>(client));
        connect();
    }
}

void CoinbaseOrderConnector::onUserDataGap(WebSocketClient* client) {
    LOG_WARN("Coinbase User WebSeocket {:p} gap detected - some order updates might be lost", static_cast<void*>(client));
}

void CoinbaseOrderConnector::onUserDataSnapshot(
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

void CoinbaseOrderConnector::onOrderUpdates(
    WebSocketClient* /* client */, uint64_t /* seq_num */, const std::vector<coinbase::Order>& orders) {
    for (const auto& order : orders) {
        process_order_update(order);
    }
}

void CoinbaseOrderConnector::onUserDataError(WebSocketClient* client, std::string&& err) {
    LOG_ERROR("WebSocket {:p} user data error: {}", static_cast<void*>(client), std::move(err));
    disconnect();
    connect();
}

void CoinbaseOrderConnector::process_order_update(const coinbase::Order& order) {
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

void CoinbaseOrderConnector::publish_order_ack(const coinbase::Order& order) {
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

void CoinbaseOrderConnector::publish_order_fill(const coinbase::Order& order) {
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

void CoinbaseOrderConnector::publish_order_reject(const coinbase::Order& order) {
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

void CoinbaseOrderConnector::publish_order_cancel(const coinbase::Order& order) {
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

OrderStatus CoinbaseOrderConnector::map_order_status(coinbase::OrderStatus status) {
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
