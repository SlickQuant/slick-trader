#pragma once

#include <slick/queue.h>
#include <uwebsockets/App.h>
#include <slick-trader/events/event.hpp>
#include <slick-trader/events/order_book_event.hpp>
#include <slick-trader/events/level2_update_event.hpp>
#include <slick-trader/core/broker_status.hpp>
#include <slick/stream_buffer_multiplexer.hpp>
#include <atomic>
#include <array>
#include <thread>
#include <string>
#include <string_view>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>

namespace slick::trader {

// Forward declarations
class StrategyManager;
class PositionTracker;
class PnLCalculator;
class RiskManager;
struct OrderAckEvent;
struct OrderFillEvent;
struct OrderCancelEvent;
struct OrderRejectEvent;

using json = nlohmann::json;

// Web server for monitoring and control
// Provides REST API and WebSocket streaming
// Runs on dedicated thread using uWebSockets
class WebServer {
public:
    struct Config {
        std::string host{"0.0.0.0"};
        uint16_t port{8080};
        std::string static_files_path{"./web/static"};
        bool enable_cors{true};
    };

    WebServer(
        std::string_view config_path,
        StrategyManager& strategy_manager,
        PositionTracker& position_tracker,
        PnLCalculator& pnl_calculator,
        RiskManager& risk_manager,
        slick::stream_buffer_multiplexer& mux)
        : strategy_manager_(strategy_manager)
        , position_tracker_(position_tracker)
        , pnl_calculator_(pnl_calculator)
        , risk_manager_(risk_manager)
        , event_mux_(mux)
        , event_cursor_(event_mux_.initial_reading_index())
        , config_path_(config_path)
    {}

    ~WebServer() {
        stop();
    }

    // Initialize with configuration
    bool initialize(const json& config);

    // Start web server thread
    void start();

    // Stop web server
    void stop();

    // Check if running
    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

    // Get current port
    uint16_t get_port() const { return config_.port; }

private:
    void run();

    // REST API handlers
    void setup_routes(uWS::App &app);
    void handle_get_strategies(uWS::HttpResponse<false>* res, uWS::HttpRequest* req);
    void handle_get_positions(uWS::HttpResponse<false>* res, uWS::HttpRequest* req);
    void handle_get_pnl(uWS::HttpResponse<false>* res, uWS::HttpRequest* req);
    void handle_get_strategy_positions(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                        const std::string& strategy_id);
    void handle_get_strategy_pnl(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                  const std::string& strategy_id);
    void handle_post_order(uWS::HttpResponse<false>* res, uWS::HttpRequest* req, std::string_view body);
    void handle_post_strategy_control(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                       const std::string& strategy_id, std::string_view body);
    void handle_get_risk_limits(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                 const std::string& strategy_id);
    void handle_put_risk_limits(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                 const std::string& strategy_id, std::string_view body);
    void handle_get_strategy_config(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                     const std::string& strategy_id);
    void handle_put_strategy_config(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                     const std::string& strategy_id, std::string_view body);
    void handle_post_halt_strategy(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                     const std::string& strategy_id, std::string_view body);
    void handle_post_resume_strategy(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                       const std::string& strategy_id);
    void handle_get_brokers(uWS::HttpResponse<false>* res, uWS::HttpRequest* req);
    void handle_get_ladder_config(uWS::HttpResponse<false>* res, uWS::HttpRequest* req);
    void handle_put_ladder_config(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                   const std::string& body);
    void handle_get_orders(uWS::HttpResponse<false>* res, uWS::HttpRequest* req);
    void handle_delete_order(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                             const std::string& strategy_id, const std::string& order_id,
                             std::string_view body);
    void handle_put_order(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                          const std::string& strategy_id, const std::string& order_id,
                          std::string_view body);
    void handle_delete_all_orders(uWS::HttpResponse<false>* res, uWS::HttpRequest* req);

    // Persist updated strategy parameters to config.json on disk
    void save_strategy_parameters(const std::string& strategy_id, const nlohmann::json& params);

    // WebSocket handlers
    void handle_ws_open(uWS::WebSocket<false, true, void*>* ws);
    void handle_ws_message(uWS::WebSocket<false, true, void*>* ws, std::string_view message);
    void handle_ws_close(uWS::WebSocket<false, true, void*>* ws);

    // Helper methods
    void send_json_response(uWS::HttpResponse<false>* res, const std::string& json, int status = 200);
    void send_error_response(uWS::HttpResponse<false>* res, const std::string& error, int status = 400);
    void add_cors_headers(uWS::HttpResponse<false>* res);

    // Market data streaming
    void process_events();

    void handle_order_ack(const OrderAckEvent& ack);
    void handle_order_fill(const OrderFillEvent& fill);
    void handle_order_cancel(const OrderCancelEvent& cancel);
    void handle_order_reject(const OrderRejectEvent& reject);

    void broadcast_level2_update(const Level2UpdateEvent& event);
    void broadcast_orderbook(const OrderBookEvent& event);
    void broadcast_order_update(const std::string& message);

    // PnL broadcasting
    void broadcast_pnl_update();

    // Broker status broadcasting
    void check_and_broadcast_broker_status();

    // WebSocket subscription tracking
    struct Subscription {
        std::unordered_set<uWS::WebSocket<false, true, void*>*> clients;
    };

    StrategyManager& strategy_manager_;
    PositionTracker& position_tracker_;
    PnLCalculator& pnl_calculator_;
    RiskManager& risk_manager_;

    Config config_;
    std::atomic_bool running_{false};
    std::thread worker_thread_;
    us_listen_socket_t* listen_socket_{nullptr};

    slick::stream_buffer_multiplexer& event_mux_;
    uint64_t event_cursor_{0};

    // // Market data queue (shared with StrategyManager)
    // std::shared_ptr<slick::SlickQueue<uint8_t>> md_queue_;
    // uint64_t ws_md_cursor_{0};  // Independent cursor for WebServer

    // // Order event queue (shared with StrategyManager)
    // std::shared_ptr<slick::SlickQueue<uint8_t>> order_event_queue_;
    // uint64_t ws_order_cursor_{0};  // Independent cursor for WebServer

    // // Top-of-book queue (written by StrategyManager)
    // std::shared_ptr<slick::SlickQueue<uint8_t>> tob_queue_;
    // uint64_t ws_tob_cursor_{0};

    // All connected WebSocket clients (for broadcasting order updates)
    std::unordered_set<uWS::WebSocket<false, true, void*>*> all_ws_clients_;

    // Subscription management
    std::unordered_map<std::string, Subscription> level2_subscriptions_;

    // Ladder configuration
    nlohmann::json ladder_config_;

    // Path to config.json — used to persist runtime strategy parameter changes
    std::string config_path_;

    // WebServer-side order tracking — accessed ONLY from the uWS event loop thread (no mutex needed)
    struct TrackedOrder {
        std::string strategy_id;
        std::string client_order_id;
        std::string order_id;       // broker-assigned; may be empty until ACK
        std::string instrument;
        std::string side;           // "buy" / "sell"
        int         type{0};
        double      price{0.0};
        double      qty{0.0};
        double      filled_qty{0.0};
        double      remaining_qty{0.0};
        std::string status{"PENDING_NEW"};
        uint64_t    created_time{0};
        uint64_t    last_update_time{0};
    };

    // Keyed by client_order_id
    std::unordered_map<std::string, TrackedOrder> open_orders_;
    // Reverse map: broker order_id → client_order_id
    std::unordered_map<std::string, std::string> oid_to_cid_;

    // Broker status snapshot for change detection (plain bools, no atomics)
    struct BrokerSnapshot {
        bool md_enabled{false};
        bool md_connected{false};
        bool order_enabled{false};
        bool order_connected{false};
    };
    std::array<BrokerSnapshot, static_cast<size_t>(BrokerId::_BROKER_ID_COUNT_)> last_broker_status_{};
};

} // namespace slick::trader
