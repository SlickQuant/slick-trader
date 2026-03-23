#include <slick-trader/brokers/coinbase/coinbase_md_connector.hpp>
#include <slick-trader/brokers/coinbase/coinbase_md_callbacks.hpp>
#include <slick/logger.hpp>
#include <coinbase/websocket.hpp>
#include <algorithm>
#include <utility>

namespace slick::trader {

CoinbaseMDConnector::CoinbaseMDConnector()
    : callbacks_(std::make_unique<CoinbaseMDCallbacks>(*this))
    , orderbook_depth_(10) {
}

CoinbaseMDConnector::~CoinbaseMDConnector() {
    stop();
}

bool CoinbaseMDConnector::connect() {
    LOG_INFO("Coinbase MD: Connecting to {}", ws_url_);

    if (!md_queue_) {
        LOG_ERROR("Coinbase MD: MD queue not set");
        return false;
    }

    // Create callbacks and WebSocket client
    ws_client_ = std::make_unique<coinbase::WebSocketClient>(callbacks_.get(), ws_url_);

    // Subscribe to heartbeat channel to open connection
    ws_client_->subscribe({}, {coinbase::WebSocketChannel::HEARTBEATS});
    ws_client_->logData("logs/coinbase");
    return true;
}

void CoinbaseMDConnector::disconnect() {
    if (ws_client_) {
        LOG_INFO("Coinbase MD: Disconnecting");
        ws_client_.reset();
        LOG_INFO("Coinbase MD: Disconnected");
    }
}

bool CoinbaseMDConnector::is_connected() const { 
    return ws_client_ ? ws_client_->isMarketDataConnected() : false;
}

void CoinbaseMDConnector::set_md_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) {
    md_queue_ = std::move(queue);
    callbacks_->set_md_queue(md_queue_);
}

bool CoinbaseMDConnector::subscribe(SubscriptionType type, const std::vector<std::string>& instruments, int depth) {
    LOG_INFO("Coinbase MD: Subscribing to {} instruments (type: {})",
             instruments.size(), static_cast<int>(type));

    switch (type) {
        case SubscriptionType::TRADE:
            trade_instruments_.insert(trade_instruments_.end(), instruments.begin(), instruments.end());
            if (ws_client_) {
                ws_client_->subscribe(instruments, {coinbase::WebSocketChannel::MARKET_TRADES});
            }
            break;
        case SubscriptionType::QUOTE:
            quote_instruments_.insert(quote_instruments_.end(), instruments.begin(), instruments.end());
            if (ws_client_) {
                ws_client_->subscribe(instruments, {coinbase::WebSocketChannel::TICKER});
            }
            break;
        case SubscriptionType::LEVEL2:
        case SubscriptionType::ORDER_BOOK:
            orderbook_instruments_.insert(orderbook_instruments_.end(), instruments.begin(), instruments.end());
            orderbook_depth_ = depth;
            if (ws_client_) {
                ws_client_->subscribe(instruments, {coinbase::WebSocketChannel::LEVEL2});
            }
            LOG_INFO("Coinbase MD: Order book depth set to {}", depth);
            break;
    }

    return true;
}

bool CoinbaseMDConnector::unsubscribe(SubscriptionType type, const std::vector<std::string>& instruments) {
    LOG_INFO("Coinbase MD: Unsubscribing from {} instruments (type: {})",
             instruments.size(), static_cast<int>(type));

    // Remove instruments from the appropriate list
    for (const auto& instrument : instruments) {
        switch (type) {
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
    connect();
}

void CoinbaseMDConnector::stop() {
    disconnect();
}

} // namespace slick::trader
