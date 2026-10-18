#pragma once

#include <slick-trader/brokers/iorder_connector.hpp>
#include <slick-trader/utils/order_types.hpp>
#include <slick/stream_buffer_multiplexer.hpp>
#include <cstdint>
#include <memory>
#include <thread>
#include <atomic>
#include <string>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <vector>

// Forward declarations
namespace coinbase {
    class WebSocketClient;
    class CoinbaseRestClient;
}

namespace slick::trader {

struct NewOrderEvent;
struct ModifyOrderEvent;
struct CancelOrderEvent;

using WebSocketClient = coinbase::WebSocketClient;
using producer_buffer = slick::stream_buffer_multiplexer::producer_buffer;
using json = nlohmann::json;

class CoinbaseOrderConnector : public IOrderConnector, public coinbase::UserThreadWebsocketCallbacks {
public:
    CoinbaseOrderConnector(
        const json& config,
        slick::stream_buffer_multiplexer &mux,
        slick::queue<uint8_t>& control_queue,
        slick::queue<uint8_t>& request_queue
    );
    ~CoinbaseOrderConnector() override;

    void register_order_connector() override;

    // IOrderConnector interface
    bool connect() override;
    void disconnect() override;
    bool is_connected() const override;

    void start() override;
    void stop() override;
    bool is_running() const override { return running_.load(std::memory_order_relaxed); }

    // Fetch taker fee rate from Coinbase REST API (e.g. 0.006 = 0.6%)?
    double get_taker_rate() const;

    // Market data callbacks - not used for order connector
    void onMarketDataConnected(WebSocketClient* /* client */) override {}
    void onMarketDataDisconnected(WebSocketClient* /* client */) override {}
    void onLevel2Snapshot(WebSocketClient* /* client */, uint64_t /* seq_num */, const coinbase::Level2UpdateBatch& /* snapshot */) override {}
    void onLevel2Updates(WebSocketClient* /* client */, uint64_t /* seq_num */, const coinbase::Level2UpdateBatch& /* updates */) override {}
    void onMarketTradesSnapshot(WebSocketClient* /* client */, uint64_t /* seq_num */, const std::vector<coinbase::MarketTrade>& /* snapshots */) override {}
    void onMarketTrades(WebSocketClient* /* client */, uint64_t /* seq_num */, const std::vector<coinbase::MarketTrade>& /* trades */) override {}
    void onTickerSnapshot(WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Ticker>& /* tickers */) override {}
    void onTickers(WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Ticker>& /* tickers */) override {}
    void onCandlesSnapshot(WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Candle>& /* candles */) override {}
    void onCandles(WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Candle>& /* candles */) override {}
    void onStatusSnapshot(WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Status>& /* status */) override {}
    void onStatus(WebSocketClient* /* client */, uint64_t /* seq_num */, uint64_t /* timestamp */, const std::vector<coinbase::Status>& /* status */) override {}
    void onMarketDataGap(WebSocketClient* /* client */) override {}
    void onMarketDataError(WebSocketClient* /* client */, std::string &&/* err */) override {}

    void onUserDataConnected(WebSocketClient* client) override;
    void onUserDataDisconnected(WebSocketClient* client) override;
    void onUserDataGap(WebSocketClient* client) override;
    void onUserDataSnapshot(WebSocketClient* client, uint64_t seq_num, const std::vector<coinbase::Order>& orders,
                           const std::vector<coinbase::PerpetualFuturePosition>& /* perpetual_future_positions */,
                           const std::vector<coinbase::ExpiringFuturePosition>& expiring_future_positions) override;
    void onOrderUpdates(WebSocketClient* client, uint64_t seq_num, const std::vector<coinbase::Order>& orders) override;
    void onUserDataError(WebSocketClient* client, std::string &&err) override;

private:
    void run();
    void process_order_requests();
    void handle_order_request(const NewOrderEvent& request);
    void handle_modify_request(const ModifyOrderEvent& request);
    void handle_cancel_request(const CancelOrderEvent& request);

    void process_order_update(const coinbase::Order& order);
    void publish_order_ack(const coinbase::Order& order);
    void publish_order_fill(const coinbase::Order& order);
    void publish_order_reject(const coinbase::Order& order);
    void publish_order_cancel(const coinbase::Order& order);
    OrderStatus map_order_status(coinbase::OrderStatus status);

private:
    std::atomic_bool running_{false};
    bool enabled_{true};
    std::thread worker_thread_;

    std::unique_ptr<coinbase::WebSocketClient> ws_client_;
    std::unique_ptr<coinbase::CoinbaseRestClient> rest_client_;

    std::string shm_name_;

    std::shared_ptr<producer_buffer> event_buffer_;
    slick::queue<uint8_t>& control_queue_;
    slick::queue<uint8_t>& request_queue_;
    uint64_t request_cursor_;

    struct FillState {
        double prev_cumulative_qty{0.0};
        double prev_avg_price{0.0};
        uint32_t prev_number_of_fills{0};
        double prev_total_fees{0.0};
    };

    std::unordered_map<std::string, FillState> fill_states_;  // keyed by order_id
};

} // namespace slick::trader
