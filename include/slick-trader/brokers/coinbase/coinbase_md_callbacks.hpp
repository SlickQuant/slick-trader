#pragma once

#include <coinbase/websocket.hpp>
#include <slick/stream_buffer_multiplexer.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace slick::trader {

using WebSocketClient = coinbase::WebSocketClient;
using producer_buffer = slick::stream_buffer_multiplexer::producer_buffer;
using json = nlohmann::json;
class CoinbaseMDConnector;

class CoinbaseMDCallbacks : public coinbase::WebsocketCallbacks {
public:
    CoinbaseMDCallbacks(const json& config, CoinbaseMDConnector &connector, slick::stream_buffer_multiplexer &mux);
    ~CoinbaseMDCallbacks() override = default;

    void onMarketDataConnected(WebSocketClient* client) override;
    void onMarketDataDisconnected(WebSocketClient* client) override;
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
    void onMarketDataError(WebSocketClient* client, std::string &&err) override;
    
    void onUserDataConnected(WebSocketClient* client) override {}
    void onUserDataDisconnected(WebSocketClient* client) override {}
    void onUserDataSnapshot(WebSocketClient* /* client */, uint64_t /* seq_num */, const std::vector<coinbase::Order>& /* orders */,
        const std::vector<coinbase::PerpetualFuturePosition>& /* perpetual_future_positions */,
        const std::vector<coinbase::ExpiringFuturePosition>& /* expiring_future_positions */) override {}
    void onOrderUpdates(WebSocketClient* client, uint64_t seq_num, const std::vector<coinbase::Order>& orders) override {}
    void onUserDataGap(WebSocketClient* client) override {}
    void onUserDataError(WebSocketClient* client, std::string &&err) override {}

    uint32_t producer_id() const noexcept {
        return event_buffer_->producer_id();
    }

    std::string_view shm_name() const noexcept {
        return shm_name_;
    }

private:
    void publish_trade(const coinbase::MarketTrade& trade);
    void publish_ticker(const coinbase::Ticker& ticker, uint64_t timestamp);
    void publish_level2_update(const coinbase::Level2UpdateBatch& batch, uint64_t seq_num, bool is_snapshot);
    void publish_order_book(const coinbase::Level2UpdateBatch& batch, uint64_t seq_num, bool is_snapshot);

    std::string shm_name_;
    std::shared_ptr<producer_buffer> event_buffer_;
    CoinbaseMDConnector &connector_;
};

} // namespace slick::trader
