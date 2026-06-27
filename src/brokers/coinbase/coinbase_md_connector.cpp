#include <slick/logger.hpp>
#include <slick-trader/brokers/coinbase/coinbase_md_connector.hpp>
#include <slick-trader/brokers/coinbase/coinbase_md_callbacks.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/events/register_event.hpp>
#include <coinbase/websocket.hpp>
#include <algorithm>
#include <chrono>
#include <utility>

namespace slick::trader {

using json = nlohmann::json;

CoinbaseMDConnector::CoinbaseMDConnector(
    const json& config,
    slick::stream_buffer_multiplexer &mux,
    slick::queue<uint8_t>& control_queue,
    slick::queue<uint8_t>& request_queue
)
    : enabled_(config.empty() ? false : config.value("enabled", true))
    , control_queue_(control_queue)
    , request_queue_(request_queue)
    , request_cursor_(request_queue.initial_reading_index())
    , ws_url_(config.value("websocket_url", "wss://advanced-trade-ws.coinbase.com"))
    , orderbook_depth_(10) {
    if (enabled_) {
        callbacks_ = std::make_unique<CoinbaseMDCallbacks>(config, *this, mux);
        ws_client_ = std::make_unique<coinbase::WebSocketClient>(
            callbacks_.get(),
            ws_url_,
            "",
            nullptr,
            config.value("queue_size", 1u << 24),
            config.value("control_size", 1u << 18)
        );
        if (config.value("recording", false)) {
            ws_client_->logData(config.value("log_data_path", "logs/coinbase"));
        }
    }
}

CoinbaseMDConnector::~CoinbaseMDConnector() {
    stop();
}

void CoinbaseMDConnector::register_md_connector() {
    if(enabled_) {
        EventSerializer::publish<RegisterEvent>(control_queue_, [=](RegisterEvent& event) {
            event.broker_id = BrokerId::COINBASE;
            event.component_type = ComponentType::MARKET_DATA;
            event.producer_id = callbacks_->producer_id();
            std::memcpy(event.shm_name, callbacks_->shm_name().data(), sizeof(event.shm_name));
        });
    }
}

bool CoinbaseMDConnector::connect() {
    if (!enabled_ || !ws_client_) {
        return false;
    }
    // Coinbase connection will be opened on subscription
    return true;
}

void CoinbaseMDConnector::disconnect() {
    if (ws_client_) {
        ws_client_->stop();
        LOG_INFO("Coinbase MD: Disconnecting");
    }
}

bool CoinbaseMDConnector::is_connected() const { 
    return ws_client_ ? ws_client_->isMarketDataConnected() : false;
}

bool CoinbaseMDConnector::subscribe(const MDSubscription& request) {
    LOG_INFO("Coinbase MD: Subscribing to {} instruments (type: {})",
             request.num_instruments, static_cast<int>(request.subscription_type));

    switch (request.subscription_type) {
        case SubscriptionType::TRADE:
            for (auto i = 0u; i < request.num_instruments; ++i) {
                trade_instruments_.emplace_back(request.instruments[i]);
            }
            if (ws_client_) {
                ws_client_->subscribe(trade_instruments_, {coinbase::WebSocketChannel::MARKET_TRADES});
            }
            break;
        case SubscriptionType::QUOTE:
            for (auto i = 0u; i < request.num_instruments; ++i) {
                quote_instruments_.emplace_back(request.instruments[i]);
            }
            if (ws_client_) {
                ws_client_->subscribe(quote_instruments_, {coinbase::WebSocketChannel::TICKER});
            }
            break;
        case SubscriptionType::LEVEL2:
        case SubscriptionType::ORDER_BOOK:
            for (auto i = 0u; i < request.num_instruments; ++i) {
                orderbook_instruments_.emplace_back(request.instruments[i]);
            }
            orderbook_depth_ = request.depth;
            if (ws_client_) {
                ws_client_->subscribe(orderbook_instruments_, {coinbase::WebSocketChannel::LEVEL2});
            }
            LOG_INFO("Coinbase MD: Order book depth set to {}", request.depth);
            break;
    }

    return true;
}

bool CoinbaseMDConnector::unsubscribe(const MDSubscription& request) {
    LOG_INFO("Coinbase MD: Unsubscribing from {} instruments (type: {})",
             request.num_instruments, static_cast<int>(request.subscription_type));

    // Remove instruments from the appropriate list
    for (auto i = 0u; i < request.num_instruments; ++i) {
        auto &instrument = request.instruments[i];
        switch (request.subscription_type) {
            case SubscriptionType::TRADE:
                trade_instruments_.erase(
                    std::remove(trade_instruments_.begin(), trade_instruments_.end(), instrument),
                    trade_instruments_.end());
                if (ws_client_) {
                    ws_client_->unsubscribe({instrument}, {coinbase::WebSocketChannel::MARKET_TRADES});
                }
                break;
            case SubscriptionType::QUOTE:
                quote_instruments_.erase(
                    std::remove(quote_instruments_.begin(), quote_instruments_.end(), instrument),
                    quote_instruments_.end());
                if (ws_client_) {
                    ws_client_->unsubscribe({instrument}, {coinbase::WebSocketChannel::TICKER});
                }
                break;
            case SubscriptionType::LEVEL2:
            case SubscriptionType::ORDER_BOOK:
                orderbook_instruments_.erase(
                    std::remove(orderbook_instruments_.begin(), orderbook_instruments_.end(), instrument),
                    orderbook_instruments_.end());
                if (ws_client_) {
                    ws_client_->unsubscribe({instrument}, {coinbase::WebSocketChannel::LEVEL2});
                }
                break;
        }
    }

    return true;
}

void CoinbaseMDConnector::start() {
    if (!enabled_) {
        return;
    }
    if (running_.load(std::memory_order_relaxed)) [[unlikely]] {
        LOG_WARN("Coinbase MD: Already running");
        return;
    }
    running_.store(true, std::memory_order_release);
    connect();
    worker_thread_ = std::thread(&CoinbaseMDConnector::run, this);
}

void CoinbaseMDConnector::stop() {
    if (!running_.load(std::memory_order_relaxed)) {
        return;
    }
    running_.store(false, std::memory_order_release);
    disconnect();
}

void CoinbaseMDConnector::run() {
    assert(enabled_);
    LOG_INFO("Coinbase MD: Worker thread started");

    while (running_.load(std::memory_order_acquire)) {
        process_md_requests();
    }

    LOG_INFO("Coinbase MD: Worker thread stopped");
}

void CoinbaseMDConnector::process_md_requests() {
    auto [data, size] = request_queue_.read(request_cursor_);

    if (!data) {
        std::this_thread::yield();
        return;
    }

    const auto *event = reinterpret_cast<const Event*>(data);
    switch (event->type) {
        case EventType::SUBSCRIBE: {
            auto *sub = reinterpret_cast<const MDSubscription*>(event);
            if (sub->broker_id == BrokerId::COINBASE && enabled_) {
                subscribe(*sub);
            } else if (sub->broker_id == BrokerId::COINBASE) {
                LOG_WARN("Coinbase MD skip subscription, not enabled");
            }
            break;
        }
        case EventType::UNSUBSCRIBE: {
            auto *sub = reinterpret_cast<const MDSubscription*>(event);
            if (sub->broker_id == BrokerId::COINBASE && enabled_) {
                unsubscribe(*sub);
            }
            break;
        }
        default:
            LOG_ERROR("Coinbase MD: Unknown event type: {}", static_cast<int>(event->type));
            break;
    }
}

} // namespace slick::trader
