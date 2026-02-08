#include <slick-trader/brokers/coinbase/coinbase_order_connector.h>
#include <slick-trader/events/event_serializer.h>
#include <slick-trader/events/order_event.h>
#include <slick/logger.hpp>
#include <coinbase/websocket.hpp>
#include <coinbase/rest.hpp>
#include <coinbase/common.hpp>
#include <cassert>
#include <thread>

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
    , order_cursor_(0)
    , rest_url_("https://api.coinbase.com")
    , ws_url_("wss://advanced-trade-ws-user.coinbase.com") {
}

CoinbaseOrderConnector::~CoinbaseOrderConnector() {
    stop();
}

bool CoinbaseOrderConnector::connect() {
    LOG_INFO("Coinbase Order: Connecting to REST: {}, WS: {}", rest_url_, ws_url_);

    if (!order_submission_queue_ || !order_event_queue_) {
        LOG_ERROR("Coinbase Order: Queues not set");
        return false;
    }

    // Create REST client for order submission
    rest_client_ = std::make_unique<coinbase::CoinbaseRestClient>(rest_url_);

    // Only user_data_url is needed for order updates
    ws_client_ = std::make_unique<coinbase::WebSocketClient>(&callbacks_, "", ws_url_);

    // Subscribe to USER channel for order updates
    ws_client_->subscribe({}, {coinbase::WebSocketChannel::USER});
    ws_client_->subscribe({}, {coinbase::WebSocketChannel::HEARTBEAT});
    return true;
}

void CoinbaseOrderConnector::disconnect() {
    LOG_INFO("Coinbase Order: Disconnecting");

    if (ws_client_) {
        ws_client_.reset();
    }

    if (rest_client_) {
        rest_client_.reset();
    }

    LOG_INFO("Coinbase Order: Disconnected");
}

bool CoinbaseOrderConnector::is_connected() const { 
    return ws_client_ ? ws_client_->isUserDataConnected() : false;
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

    LOG_INFO("Coinbase Order: Stopped");
}

void CoinbaseOrderConnector::run() {
    LOG_INFO("Coinbase Order: Worker thread started");

    connect();

    while (running_.load(std::memory_order_acquire)) {
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
            coinbase::to_string(cb_tif), from_qty_t(request.quantity), from_price_t(request.price));

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
            request.stop_price > 0 ? std::optional<double>(from_price_t(request.stop_price)) : std::nullopt
        );

        if (response.success) {
            LOG_INFO("Order submitted successfully: order_id={}, client_order_id={}",
                        response.success_response.order_id,
                        response.success_response.client_order_id);
            EventSerializer::publish<OrderAckEvent>(*order_event_queue_, [&request, &response](OrderAckEvent &event) {
                event.type = EventType::ORDER_ACK;
                event.broker_id = BrokerId::COINBASE;
                event.timestamp_ns = std::chrono::system_clock::now().time_since_epoch().count();
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
                reject_event.timestamp_ns = std::chrono::system_clock::now().time_since_epoch().count();
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
            reject_event.timestamp_ns = std::chrono::system_clock::now().time_since_epoch().count();
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
    LOG_INFO("Processing modify request: order_id={}, client_order_id={}",
             request.order_id, request.client_order_id);

}

void CoinbaseOrderConnector::handle_cancel_request(const CancelOrderEvent& request) {
    LOG_INFO("Processing cancel request: order_id={}, client_order_id={}",
             request.order_id, request.client_order_id);

    try {
        // std::vector<std::string_view> order_ids = { request.order_id };
        auto responses = rest_client_->cancel_orders({ request.order_id });

        for (const auto& response : responses) {
            if (response.success) {
                LOG_INFO("Order cancelled successfully: order_id={}", response.order_id);
            } else {
                LOG_ERROR("Order cancellation failed: order_id={}, reason={}",
                          response.order_id, response.failure_reason);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception cancelling order: {}", e.what());
    }
}

} // namespace slick::trader
