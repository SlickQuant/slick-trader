#include <slick/logger.hpp>
#include <slick-trader/brokers/coinbase/coinbase_md_connector.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/events/register_event.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/events/order_book_event.hpp>
#include <slick-trader/events/level2_update_event.hpp>
#include <slick-trader/events/quote_event.hpp>
#include <slick-trader/events/trade_event.hpp>
#include <slick-trader/core/broker_status.hpp>
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
    , shm_name_(config.value("shm_name", ""))
    , event_buffer_(mux.add_producer(
        mux.producer_count(),
        config.value("queue_size", 1u << 26),
        config.value("control_size", 1u << 24),
        shm_name_.empty() ? nullptr : shm_name_.c_str()))
    , control_queue_(control_queue)
    , request_queue_(request_queue)
    , request_cursor_(request_queue.initial_reading_index())
    , ws_url_(config.value("websocket_url", "wss://advanced-trade-ws.coinbase.com"))
    , orderbook_depth_(10) {
    auto& reg = BrokerStatusRegistry::instance();
    reg.set_name(BrokerId::COINBASE, "Coinbase");
    reg.set_md_enabled(BrokerId::COINBASE, enabled_);
    if (enabled_) {
        ws_client_ = std::make_unique<coinbase::WebSocketClient>(
            this,
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
            event.producer_id = event_buffer_->producer_id();
            std::memcpy(event.shm_name, shm_name_.c_str(), sizeof(event.shm_name));
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
        case SubscriptionType::MBO:
            LOG_WARN("Coinbase MD: no market-by-order feed, MBO subscription ignored");
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
            case SubscriptionType::MBO:
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
    if (reconnecting_.load(std::memory_order_relaxed)) {
        // resubscribe
        if (ws_client_) [[likely]] {
            ws_client_->subscribe(trade_instruments_, {coinbase::WebSocketChannel::MARKET_TRADES});
            ws_client_->subscribe(quote_instruments_, {coinbase::WebSocketChannel::TICKER});
            ws_client_->subscribe(orderbook_instruments_, {coinbase::WebSocketChannel::LEVEL2});
        }
        reconnecting_.store(false, std::memory_order_release);
    }

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

void CoinbaseMDConnector::onMarketDataConnected(WebSocketClient* client) {
    LOG_INFO("Coinbase MD WebSocket {:p} connected", static_cast<void*>(client));
    BrokerStatusRegistry::instance().set_md_connected(BrokerId::COINBASE, true);
}

void CoinbaseMDConnector::onMarketDataDisconnected(WebSocketClient* client) {
    LOG_INFO("Coinbase MD WebSocket {:p} disconnected", static_cast<void*>(client));
    BrokerStatusRegistry::instance().set_md_connected(BrokerId::COINBASE, false);
    if (is_running()) {
        LOG_WARN("Coinbase MD WebSocket {:p} disconnected while running - reconnecting...", static_cast<void*>(client));
        reconnecting_.store(true, std::memory_order_release);
    }
}

void CoinbaseMDConnector::onLevel2Snapshot(WebSocketClient* /* client */, uint64_t seq_num, const coinbase::Level2UpdateBatch& snapshot) {
    publish_level2_update(snapshot, seq_num, true);
}

void CoinbaseMDConnector::onLevel2Updates(WebSocketClient* /* client */, uint64_t seq_num, const coinbase::Level2UpdateBatch& updates) {
    publish_level2_update(updates, seq_num, false);
}

void CoinbaseMDConnector::onMarketTradesSnapshot(WebSocketClient* /* client */, uint64_t /* seq_num */, const std::vector<coinbase::MarketTrade>& snapshots) {
    for (const auto& trade : snapshots) {
        publish_trade(trade);
    }
}

void CoinbaseMDConnector::onMarketTrades(WebSocketClient* /* client */, uint64_t /* seq_num */, const std::vector<coinbase::MarketTrade>& trades) {
    for (const auto& trade : trades) {
        publish_trade(trade);
    }
}

void CoinbaseMDConnector::onTickerSnapshot(
    WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t timestamp, const std::vector<coinbase::Ticker>& tickers) {
    for (const auto& ticker : tickers) {
        publish_ticker(ticker, timestamp);
    }
}

void CoinbaseMDConnector::onTickers(
    WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t timestamp, const std::vector<coinbase::Ticker>& tickers) {
    for (const auto& ticker : tickers) {
        publish_ticker(ticker, timestamp);
    }
}

void CoinbaseMDConnector::onCandlesSnapshot(
    WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Candle>& /* candles */) {
    // TODO: Implement candle handling
}

void CoinbaseMDConnector::onCandles(
    WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Candle>& /* candles */) {
    // TODO: Implement candle handling
}

void CoinbaseMDConnector::onStatusSnapshot(
    WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Status>& /* status */) {
    // TODO: Implement status handling
}

void CoinbaseMDConnector::onStatus(
    WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Status>& /* status */) {
    // TODO: Implement status handling
}

void CoinbaseMDConnector::onMarketDataGap(WebSocketClient* client) {
    LOG_WARN("Coinbase MD WebSocket {:p} gap detected - some messages were lost", static_cast<void*>(client));
}

void CoinbaseMDConnector::onMarketDataError(WebSocketClient* client, std::string&& err) {
    LOG_ERROR("Coinbase MD WebSocket {:p} error: {}", static_cast<void*>(client), std::move(err));
}

void CoinbaseMDConnector::publish_trade(const coinbase::MarketTrade& trade) {
    assert(event_buffer_ && "Market data queue not set");
    EventSerializer::publish<TradeEvent>(*event_buffer_, [&trade](TradeEvent& event) {
        event.type = EventType::TRADE;
        event.broker_id = BrokerId::COINBASE;
        event.timestamp_ns = get_timestamp_ns();
        event.event_time_ns = trade.time;
        event.set_instrument(trade.product_id);

        event.price = to_price_t(trade.price);
        event.quantity = to_qty_t(trade.size);
        event.is_buyer_maker = (trade.side == coinbase::Side::SELL);
        event.trade_id = std::stoull(trade.trade_id);
    });
}

void CoinbaseMDConnector::publish_ticker(const coinbase::Ticker& ticker, uint64_t timestamp) {
    assert(event_buffer_ && "Market data queue not set");
    EventSerializer::publish<QuoteEvent>(*event_buffer_, [&ticker, timestamp](QuoteEvent& event) {
        event.type = EventType::QUOTE;
        event.broker_id = BrokerId::COINBASE;
        event.timestamp_ns = get_timestamp_ns();
        event.event_time_ns = timestamp;
        event.set_instrument(ticker.product_id);

        event.bid_price = to_price_t(ticker.best_bid);
        event.bid_quantity = to_qty_t(ticker.best_bid_quantity);
        event.ask_price = to_price_t(ticker.best_ask);
        event.ask_quantity = to_qty_t(ticker.best_ask_quantity);
    });
}

void CoinbaseMDConnector::publish_level2_update(const coinbase::Level2UpdateBatch& batch, uint64_t seq_num, bool is_snapshot) {
    assert(event_buffer_ && "Market data queue not set");
    if (is_snapshot) {
        LOG_DEBUG("Publish lv2 snapshot. count={}", batch.updates.size());
    }
    uint32_t size = static_cast<uint32_t>(sizeof(Level2UpdateEvent) + batch.updates.size() * sizeof(PriceLevel));
    EventSerializer::publish<Level2UpdateEvent>(*event_buffer_, size, [&batch, seq_num, is_snapshot](Level2UpdateEvent& event) {
        event.type = EventType::LEVEL2_UPDATE;
        event.broker_id = BrokerId::COINBASE;
        event.timestamp_ns = get_timestamp_ns();
        event.set_instrument(batch.product_id);
        event.is_snapshot = is_snapshot;
        event.event_time_ns = batch.updates.empty() ?  get_timestamp_ns() : batch.updates[0].event_time;
        event.sequence_num = seq_num;
        event.num_levels = static_cast<uint32_t>(batch.updates.size());

        size_t level_count = 0;
        for (const auto& update : batch.updates) {
            event.levels[level_count].price = to_price_t(update.price_level);
            event.levels[level_count].quantity = to_qty_t(update.new_quantity);
            event.levels[level_count].side = static_cast<Side>(update.side);
            ++level_count;
        }
    });
}

void CoinbaseMDConnector::publish_order_book(const coinbase::Level2UpdateBatch& batch, uint64_t seq_num, bool is_snapshot) {
    assert(event_buffer_ && "Market data queue not set");
    EventSerializer::publish<OrderBookEvent>(*event_buffer_, [&batch, seq_num, is_snapshot](OrderBookEvent& event) {
        event.type = EventType::ORDER_BOOK;
        event.broker_id = BrokerId::COINBASE;
        event.timestamp_ns = get_timestamp_ns();
        event.set_instrument(batch.product_id);
        event.is_snapshot = is_snapshot;

        size_t bid_count = 0;
        size_t ask_count = 0;
        for (const auto& update : batch.updates) {
            event.event_time_ns = update.event_time;
            event.sequence_num = seq_num;

            if (update.side == coinbase::Side::BUY && bid_count < MAX_ORDER_BOOK_LEVELS) {
                event.bids[bid_count].price = to_price_t(update.price_level);
                event.bids[bid_count].quantity = to_qty_t(update.new_quantity);
                ++bid_count;
            } else if (update.side == coinbase::Side::SELL && ask_count < MAX_ORDER_BOOK_LEVELS) {
                event.asks[ask_count].price = to_price_t(update.price_level);
                event.asks[ask_count].quantity = to_qty_t(update.new_quantity);
                ++ask_count;
            }
        }

        event.num_bids = static_cast<uint16_t>(bid_count);
        event.num_asks = static_cast<uint16_t>(ask_count);
    });
}

} // namespace slick::trader
