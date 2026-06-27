#include <slick-trader/brokers/coinbase/coinbase_md_callbacks.hpp>
#include <slick-trader/brokers/coinbase/coinbase_md_connector.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/events/order_book_event.hpp>
#include <slick-trader/events/level2_update_event.hpp>
#include <slick-trader/events/quote_event.hpp>
#include <slick-trader/events/trade_event.hpp>
#include <slick/logger.hpp>
#include <cassert>
#include <chrono>
#include <string>
#include <utility>

namespace slick::trader {
    
CoinbaseMDCallbacks::CoinbaseMDCallbacks(
    const json& config,
    CoinbaseMDConnector &connector,
    slick::stream_buffer_multiplexer &event_mux
)
    : shm_name_(config.value("shm_name", ""))
    , event_buffer_(event_mux.add_producer(
        event_mux.producer_count(),
        config.value("queue_size", 1u << 26),
        config.value("control_size", 1u << 24),
        shm_name_.empty() ? nullptr : shm_name_.c_str()))
    , connector_(connector)
{
}

void CoinbaseMDCallbacks::onMarketDataConnected(WebSocketClient* client) {
    LOG_INFO("Coinbase MD WebSocket {:p} connected", static_cast<void*>(client));
}

void CoinbaseMDCallbacks::onMarketDataDisconnected(WebSocketClient* client) {
    LOG_INFO("Coinbase MD WebSocket {:p} disconnected", static_cast<void*>(client));
    if (connector_.is_running()) {
        LOG_WARN("Coinbase MD WebSocket {:p} disconnected while running - reconnecting...", static_cast<void*>(client));
        connector_.connect();
    }
}

void CoinbaseMDCallbacks::onLevel2Snapshot(WebSocketClient* /* client */, uint64_t seq_num, const coinbase::Level2UpdateBatch& snapshot) {
    publish_level2_update(snapshot, seq_num, true);
}

void CoinbaseMDCallbacks::onLevel2Updates(WebSocketClient* /* client */, uint64_t seq_num, const coinbase::Level2UpdateBatch& updates) {
    publish_level2_update(updates, seq_num, false);
}

void CoinbaseMDCallbacks::onMarketTradesSnapshot(WebSocketClient* /* client */, uint64_t /* seq_num */, const std::vector<coinbase::MarketTrade>& snapshots) {
    for (const auto& trade : snapshots) {
        publish_trade(trade);
    }
}

void CoinbaseMDCallbacks::onMarketTrades(WebSocketClient* /* client */, uint64_t /* seq_num */, const std::vector<coinbase::MarketTrade>& trades) {
    for (const auto& trade : trades) {
        publish_trade(trade);
    }
}

void CoinbaseMDCallbacks::onTickerSnapshot(
    WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t timestamp, const std::vector<coinbase::Ticker>& tickers) {
    for (const auto& ticker : tickers) {
        publish_ticker(ticker, timestamp);
    }
}

void CoinbaseMDCallbacks::onTickers(
    WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t timestamp, const std::vector<coinbase::Ticker>& tickers) {
    for (const auto& ticker : tickers) {
        publish_ticker(ticker, timestamp);
    }
}

void CoinbaseMDCallbacks::onCandlesSnapshot(
    WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Candle>& /* candles */) {
    // TODO: Implement candle handling
}

void CoinbaseMDCallbacks::onCandles(
    WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Candle>& /* candles */) {
    // TODO: Implement candle handling
}

void CoinbaseMDCallbacks::onStatusSnapshot(
    WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Status>& /* status */) {
    // TODO: Implement status handling
}

void CoinbaseMDCallbacks::onStatus(
    WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Status>& /* status */) {
    // TODO: Implement status handling
}

void CoinbaseMDCallbacks::onMarketDataGap(WebSocketClient* client) {
    LOG_WARN("Coinbase MD WebSocket {:p} gap detected - some messages were lost", static_cast<void*>(client));
}

void CoinbaseMDCallbacks::onMarketDataError(WebSocketClient* client, std::string&& err) {
    LOG_ERROR("Coinbase MD WebSocket {:p} error: ", static_cast<void*>(client), err);
}

void CoinbaseMDCallbacks::publish_trade(const coinbase::MarketTrade& trade) {
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

void CoinbaseMDCallbacks::publish_ticker(const coinbase::Ticker& ticker, uint64_t timestamp) {
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

void CoinbaseMDCallbacks::publish_level2_update(const coinbase::Level2UpdateBatch& batch, uint64_t seq_num, bool is_snapshot) {
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

void CoinbaseMDCallbacks::publish_order_book(const coinbase::Level2UpdateBatch& batch, uint64_t seq_num, bool is_snapshot) {
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
