#pragma once

#include <slick-trader/brokers/imarket_data_connector.hpp>
#include <slick/stream_buffer_multiplexer.hpp>
#include <slick-trader/events/md_subscription.hpp>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

// Forward declarations
namespace coinbase {
    class WebSocketClient;
}

namespace slick::trader {

using WebSocketClient = coinbase::WebSocketClient;
using producer_buffer = slick::stream_buffer_multiplexer::producer_buffer;
using json = nlohmann::json;

class CoinbaseMDConnector : public IMarketDataConnector, public coinbase::WebsocketCallbacks {
public:
    CoinbaseMDConnector(
        const json& config,
        slick::stream_buffer_multiplexer &event_mux,
        slick::queue<uint8_t>& control_queue,
        slick::queue<uint8_t>& request_queue
    );

    ~CoinbaseMDConnector() override;

    void register_md_connector() override;

    // IMarketDataConnector interface
    bool connect() override;
    void disconnect() override;
    bool is_connected() const override;

    bool subscribe(const MDSubscription& request) override;
    bool unsubscribe(const MDSubscription& request) override;

    void start() override;
    void stop() override;
    bool is_running() const override { return running_.load(std::memory_order_relaxed); }

    std::string_view ws_url() const noexcept { return ws_url_; }

    // Coinbase Websocket Callbacks
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

private:
    void run();
    void process_md_requests();

    void publish_trade(const coinbase::MarketTrade& trade);
    void publish_ticker(const coinbase::Ticker& ticker, uint64_t timestamp);
    void publish_level2_update(const coinbase::Level2UpdateBatch& batch, uint64_t seq_num, bool is_snapshot);
    void publish_order_book(const coinbase::Level2UpdateBatch& batch, uint64_t seq_num, bool is_snapshot);

    std::atomic_bool running_{false};
    std::atomic_bool reconnecting_{false};
    bool enabled_{true};
    std::thread worker_thread_;
    std::string shm_name_;

    std::unique_ptr<coinbase::WebSocketClient> ws_client_;

    std::shared_ptr<producer_buffer> event_buffer_;
    slick::queue<uint8_t>& control_queue_;
    slick::queue<uint8_t>& request_queue_;
    uint64_t request_cursor_;

    std::string ws_url_;

    std::vector<std::string> trade_instruments_;
    std::vector<std::string> quote_instruments_;
    std::vector<std::string> level2_instruments_;
    std::vector<std::string> orderbook_instruments_;
    int orderbook_depth_;
};

} // namespace slick::trader
