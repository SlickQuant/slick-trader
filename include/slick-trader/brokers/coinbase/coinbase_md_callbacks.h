#pragma once

#include <slick/logger.hpp>
#include <coinbase/websocket.hpp>
#include <slick-trader/events/trade_event.h>
#include <slick-trader/events/quote_event.h>
#include <slick-trader/events/order_book_event.h>
#include <slick-trader/events/event_serializer.h>
#include <slick/queue.h>
#include <memory>

namespace slick::trader {

using WebSocketClient = coinbase::WebSocketClient;

class CoinbaseMDCallbacks : public coinbase::WebsocketCallbacks {
public:
    CoinbaseMDCallbacks() = default;
    ~CoinbaseMDCallbacks() override = default;

    void set_md_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> md_queue) {
        md_queue_ = md_queue;
    }

    void onMarketDataConnected(WebSocketClient* client) override {
        LOG_INFO("WebSocket {:p} market data connected", (void*)client);
    }
    void onUserDataConnected(WebSocketClient* /* client */) override {}
    void onMarketDataDisconnected(WebSocketClient* client) override {
        LOG_INFO("WebSocket {:p} market data disconnected", (void*)client);
    }
    void onUserDataDisconnected(WebSocketClient* /* client */) override {}
    void onLevel2Snapshot(WebSocketClient* client, uint64_t seq_num, const coinbase::Level2UpdateBatch& snapshot) override;
    void onLevel2Updates(WebSocketClient* client, uint64_t seq_num, const coinbase::Level2UpdateBatch& updates) override;

    void onMarketTradesSnapshot(WebSocketClient* /* client */, uint64_t /* seq_num */, const std::vector<coinbase::MarketTrade>& snapshots) override {
        for (const auto& trade : snapshots) {
            publish_trade(trade);
        }
    }

    void onMarketTrades(WebSocketClient* /* client */, uint64_t /* seq_num */, const std::vector<coinbase::MarketTrade>& trades) override {
        for (const auto& trade : trades) {
            publish_trade(trade);
        }
    }

    void onTickerSnapshot(WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t timestamp, const std::vector<coinbase::Ticker>& tickers) override {
        for (const auto& ticker : tickers) {
            publish_ticker(ticker, timestamp);
        }
    }

    void onTickers(WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t timestamp, const std::vector<coinbase::Ticker>& tickers) override {
        for (const auto& ticker : tickers) {
            publish_ticker(ticker, timestamp);
        }
    }

    void onCandlesSnapshot(WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Candle>& /* candles */) override {
        // TODO: Implement candle handling
    }

    void onCandles(WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Candle>& /* candles */) override {
        // TODO: Implement candle handling
    }

    void onStatusSnapshot(WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Status>& /* status */) override {
        // TODO: Implement status handling
    }

    void onStatus(WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Status>& /* status */) override {
        // TODO: Implement status handling
    }

    void onMarketDataGap(WebSocketClient* client) override {
        LOG_WARN("WebSocket {:p} Market data gap detected - some messages were lost", client);
    }

    void onUserDataGap(WebSocketClient* /* client */) override {
        assert(false && "User data error should not occur in market data connector");
    }

    void onUserDataSnapshot(WebSocketClient* /* client */, uint64_t /* seq_num */, const std::vector<coinbase::Order>& /* orders */,
                           const std::vector<coinbase::PerpetualFuturePosition>& /* perpetual_future_positions */,
                           const std::vector<coinbase::ExpiringFuturePosition>& /* expiring_future_positions */) override {
        // Not used for market data connector
        assert(false && "User data error should not occur in market data connector");
    }

    void onOrderUpdates(WebSocketClient* /* client */, uint64_t /* seq_num */, const std::vector<coinbase::Order>& /* orders */) override {
        // Not used for market data connector
        assert(false && "User data error should not occur in market data connector");
    }

    void onMarketDataError(WebSocketClient* /* client */, std::string &&err) override {
        LOG_ERROR("Market data error: {}", std::move(err));
    }

    void onUserDataError(WebSocketClient* /* client */, std::string &&/* err */) override {
        assert(false && "User data error should not occur in market data connector");
    }

private:
    void publish_trade(const coinbase::MarketTrade& trade) {
        assert(md_queue_ && "Market data queue not set");
        EventSerializer::publish<TradeEvent>(*md_queue_, [&trade](TradeEvent& event) {
            event.type = EventType::TRADE;
            event.broker_id = BrokerId::COINBASE;
            event.timestamp_ns = std::chrono::system_clock::now().time_since_epoch().count();
            event.event_time_ns = trade.time;
            event.set_instrument(trade.product_id);

            event.price = to_price_t(trade.price);
            event.quantity = to_qty_t(trade.size);
            event.is_buyer_maker = (trade.side == coinbase::Side::SELL);  // If side is SELL, buyer is maker
            event.trade_id = std::stoull(trade.trade_id);
        });
    }

    void publish_ticker(const coinbase::Ticker& ticker, uint64_t timestamp) {
        assert(md_queue_ && "Market data queue not set");
        EventSerializer::publish<QuoteEvent>(*md_queue_, [&ticker, timestamp](QuoteEvent& event) {
            event.type = EventType::QUOTE;
            event.broker_id = BrokerId::COINBASE;
            event.timestamp_ns = std::chrono::system_clock::now().time_since_epoch().count();
            event.event_time_ns = timestamp;
            event.set_instrument(ticker.product_id);

            event.bid_price = to_price_t(ticker.best_bid);
            event.bid_quantity = to_qty_t(ticker.best_bid_quantity);
            event.ask_price = to_price_t(ticker.best_ask);
            event.ask_quantity = to_qty_t(ticker.best_ask_quantity);
        });
    }

    void publish_order_book(const coinbase::Level2UpdateBatch& batch, bool is_snapshot) {
        assert(md_queue_ && "Market data queue not set");

        EventSerializer::publish<OrderBookEvent>(*md_queue_, [&batch, is_snapshot](OrderBookEvent& event) {
            event.type = EventType::ORDER_BOOK;
            event.broker_id = BrokerId::COINBASE;
            event.timestamp_ns = std::chrono::system_clock::now().time_since_epoch().count();
            event.set_instrument(batch.product_id);
            event.is_snapshot = is_snapshot;

            // Separate updates into bids and asks
            size_t bid_count = 0;
            size_t ask_count = 0;

            for (const auto& update : batch.updates) {
                event.event_time_ns = update.event_time;
                event.sequence_num = update.seq_num;

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

    std::shared_ptr<slick::SlickQueue<uint8_t>> md_queue_;
};

// Inline implementations
inline void CoinbaseMDCallbacks::onLevel2Snapshot(WebSocketClient* /* client */, uint64_t /* seq_num */, const coinbase::Level2UpdateBatch& snapshot) {
    publish_order_book(snapshot, true);
}

inline void CoinbaseMDCallbacks::onLevel2Updates(WebSocketClient* /* client */, uint64_t /* seq_num */, const coinbase::Level2UpdateBatch& updates) {
    publish_order_book(updates, false);
}

} // namespace slick::trader
