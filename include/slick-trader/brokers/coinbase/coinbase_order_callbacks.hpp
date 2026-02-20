#pragma once

#include <coinbase/websocket.hpp>
#include <slick-trader/utils/order_types.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace slick {
template<typename T>
class SlickQueue;
}

namespace slick::trader {

using WebSocketClient = coinbase::WebSocketClient;

class CoinbaseOrderCallbacks : public coinbase::WebsocketCallbacks {
public:
    CoinbaseOrderCallbacks() = default;
    ~CoinbaseOrderCallbacks() override = default;

    void set_order_event_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> order_event_queue);

    // Market data callbacks - not used for order connector
    void onMarketDataConnected(WebSocketClient* client) override;
    void onUserDataConnected(WebSocketClient* client) override;
    void onMarketDataDisconnected(WebSocketClient* client) override;
    void onUserDataDisconnected(WebSocketClient* client) override;
    void onLevel2Snapshot(WebSocketClient* client, uint64_t seq_num, const coinbase::Level2UpdateBatch& snapshot) override;
    void onLevel2Updates(WebSocketClient* client, uint64_t seq_num, const coinbase::Level2UpdateBatch& updates) override;
    void onMarketTradesSnapshot(WebSocketClient* client, uint64_t seq_num, const std::vector<coinbase::MarketTrade>& snapshots) override;
    void onMarketTrades(WebSocketClient* client, uint64_t seq_num, const std::vector<coinbase::MarketTrade>& trades) override;
    void onTickerSnapshot(WebSocketClient* client, uint64_t seq_num, uint64_t timestamp, const std::vector<coinbase::Ticker>& tickers) override;
    void onTickers(WebSocketClient* client, uint64_t seq_num, uint64_t timestamp, const std::vector<coinbase::Ticker>& tickers) override;
    void onCandlesSnapshot(WebSocketClient* client, uint64_t seq_num, uint64_t timestamp, const std::vector<coinbase::Candle>& candles) override;
    void onCandles(WebSocketClient* client, uint64_t seq_num, uint64_t timestamp, const std::vector<coinbase::Candle>& candles) override;
    void onStatusSnapshot(WebSocketClient* client, uint64_t seq_num, uint64_t timestamp, const std::vector<coinbase::Status>& status) override;
    void onStatus(WebSocketClient* client, uint64_t seq_num, uint64_t timestamp, const std::vector<coinbase::Status>& status) override;
    void onMarketDataGap(WebSocketClient* client) override;
    void onUserDataGap(WebSocketClient* client) override;
    void onUserDataSnapshot(WebSocketClient* client, uint64_t seq_num, const std::vector<coinbase::Order>& orders,
                           const std::vector<coinbase::PerpetualFuturePosition>& /* perpetual_future_positions */,
                           const std::vector<coinbase::ExpiringFuturePosition>& expiring_future_positions) override;
    void onOrderUpdates(WebSocketClient* client, uint64_t seq_num, const std::vector<coinbase::Order>& orders) override;
    void onMarketDataError(WebSocketClient* client, std::string &&err) override;
    void onUserDataError(WebSocketClient* client, std::string &&err) override;

private:
    void process_order_update(const coinbase::Order& order);
    void publish_order_ack(const coinbase::Order& order);
    void publish_order_fill(const coinbase::Order& order);
    void publish_order_reject(const coinbase::Order& order);
    void publish_order_cancel(const coinbase::Order& order);
    OrderStatus map_order_status(coinbase::OrderStatus status);

    std::shared_ptr<slick::SlickQueue<uint8_t>> order_event_queue_;
};

} // namespace slick::trader
