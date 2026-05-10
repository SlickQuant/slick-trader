#include <slick-trader/brokers/coinbase/coinbase_order_connector.hpp>
#include <slick-trader/brokers/coinbase/coinbase_order_callbacks.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/events/order_event.hpp>
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

CoinbaseOrderConnector::CoinbaseOrderConnector()
    : running_(false)
    , callbacks_(std::make_unique<CoinbaseOrderCallbacks>(*this))
    , order_cursor_(0)
    , rest_url_("https://api.coinbase.com")
    , ws_url_("wss://advanced-trade-ws-user.coinbase.com") {
    // Create REST client for order submission
    LOG_INFO("Coinbase Order REST: {}", ws_url_);
    rest_client_ = std::make_unique<coinbase::CoinbaseRestClient>(rest_url_);
}

CoinbaseOrderConnector::~CoinbaseOrderConnector() {
    stop();
}

bool CoinbaseOrderConnector::connect() {
    LOG_INFO("Coinbase Order: Connecting to WS: {}", ws_url_);

    if (!order_submission_queue_ || !order_event_queue_) {
        LOG_ERROR("Coinbase Order: Queues not set");
        return false;
    }

    // Only user_data_url is needed for order updates
    ws_client_ = std::make_unique<coinbase::WebSocketClient>(callbacks_.get(), "", ws_url_);

    // Subscribe to USER channel for order updates
    ws_client_->subscribe({}, {coinbase::WebSocketChannel::USER});
    return true;
}

void CoinbaseOrderConnector::disconnect() {
    if (ws_client_) {
        LOG_INFO("Coinbase Order: Disconnecting");
        ws_client_.reset();
        LOG_INFO("Coinbase Order: Disconnected");
    }
}

bool CoinbaseOrderConnector::is_connected() const { 
    return ws_client_ ? ws_client_->isUserDataConnected() : false;
}

void CoinbaseOrderConnector::set_order_event_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) {
    order_event_queue_ = std::move(queue);
    callbacks_->set_order_event_queue(order_event_queue_);
}

void CoinbaseOrderConnector::set_rest_url(const std::string& url) {
    rest_url_ = url;
    LOG_INFO("Coinbase Order REST URL set to {}", rest_url_);
    rest_client_ = std::make_unique<coinbase::CoinbaseRestClient>(rest_url_);
}

double CoinbaseOrderConnector::get_taker_rate() const {
    if (!rest_client_) return 0.0;
    return rest_client_->get_taker_fee_rate();
}

void CoinbaseOrderConnector::start() {
    if (running_.load(std::memory_order_relaxed)) {
        LOG_WARN("Coinbase Order: Already running");
        return;
    }

    LOG_INFO("Coinbase Order: Starting");
    running_.store(true, std::memory_order_release);
    worker_thread_ = std::thread(&CoinbaseOrderConnector::run, this);
    LOG_INFO("Coinbase Order: Started");
}

void CoinbaseOrderConnector::stop() {
    if (!running_.load(std::memory_order_acquire)) {
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
    LOG_INFO("Coinbase Order: Worker thread started");

    connect();

    while (running_.load(std::memory_order_acquire)) {
        callbacks_->processData();
        process_order_requests();
    }

    LOG_INFO("Coinbase Order: Worker thread stopped");
}

void CoinbaseOrderConnector::process_order_requests() {
    assert(order_submission_queue_ && "Order submission queue not set");
    assert(rest_client_ && "REST client not initialized");

    auto [data, size] = order_submission_queue_->read(order_cursor_);

    if (!data) {
        std::this_thread::yield();
        return;
    }

    const auto *event = reinterpret_cast<const Event*>(data);
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
    try {
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
            EventSerializer::publish<OrderAckEvent>(*order_event_queue_, [&request, &response](OrderAckEvent &event) {
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
                EventSerializer::publish<OrderRejectEvent>(*order_event_queue_, [&request, &response](OrderRejectEvent &reject_event) {
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
        EventSerializer::publish<OrderRejectEvent>(*order_event_queue_, [&request, &e](OrderRejectEvent &reject_event) {
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
        EventSerializer::publish<OrderRejectEvent>(*order_event_queue_, [&](OrderRejectEvent& ev) {
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
            EventSerializer::publish<OrderAckEvent>(*order_event_queue_, [&](OrderAckEvent& ev) {
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
        EventSerializer::publish<OrderCancelEvent>(*order_event_queue_, [&](OrderCancelEvent& ev) {
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

} // namespace slick::trader
