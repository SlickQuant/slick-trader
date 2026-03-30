#pragma once

#include <slick/logger.hpp>
#include <slick/queue.h>
#include <uwebsockets/App.h>
#include <slick-trader/events/event.hpp>
#include <slick-trader/events/order_book_event.hpp>
#include <slick-trader/events/level2_update_event.hpp>
#include <atomic>
#include <thread>
#include <string>
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

    WebServer(StrategyManager& strategy_manager,
              PositionTracker& position_tracker,
              PnLCalculator& pnl_calculator,
              RiskManager& risk_manager)
        : strategy_manager_(strategy_manager)
        , position_tracker_(position_tracker)
        , pnl_calculator_(pnl_calculator)
        , risk_manager_(risk_manager)
        , running_(false) {}

    ~WebServer() {
        stop();
    }

    // Initialize with configuration
    bool initialize(const Config& config);
    bool initialize(const Config& config, const nlohmann::json& full_config);

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

    // Set market data queue for real-time WebSocket streaming
    void set_market_data_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) {
        md_queue_ = queue;
    }

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
    void handle_post_halt_strategy(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                     const std::string& strategy_id, std::string_view body);
    void handle_post_resume_strategy(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                       const std::string& strategy_id);
    void handle_get_ladder_config(uWS::HttpResponse<false>* res, uWS::HttpRequest* req);
    void handle_put_ladder_config(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                   const std::string& body);

    // WebSocket handlers
    void handle_ws_open(uWS::WebSocket<false, true, void*>* ws);
    void handle_ws_message(uWS::WebSocket<false, true, void*>* ws, std::string_view message);
    void handle_ws_close(uWS::WebSocket<false, true, void*>* ws);

    // Helper methods
    void send_json_response(uWS::HttpResponse<false>* res, const std::string& json, int status = 200);
    void send_error_response(uWS::HttpResponse<false>* res, const std::string& error, int status = 400);
    void add_cors_headers(uWS::HttpResponse<false>* res);

    // Market data streaming
    void process_market_data_events();
    void broadcast_level2_update(const Level2UpdateEvent& event);
    void broadcast_orderbook(const OrderBookEvent& event);

    // WebSocket subscription tracking
    struct Subscription {
        std::unordered_set<uWS::WebSocket<false, true, void*>*> clients;
    };

    StrategyManager& strategy_manager_;
    PositionTracker& position_tracker_;
    PnLCalculator& pnl_calculator_;
    RiskManager& risk_manager_;

    Config config_;
    std::atomic_bool running_;
    std::thread worker_thread_;
    us_listen_socket_t* listen_socket_{nullptr};

    // Market data queue (shared with StrategyManager)
    std::shared_ptr<slick::SlickQueue<uint8_t>> md_queue_;
    uint64_t ws_md_cursor_{0};  // Independent cursor for WebServer

    // Subscription management
    std::unordered_map<std::string, Subscription> level2_subscriptions_;

    // Ladder configuration
    nlohmann::json ladder_config_;
};

} // namespace slick::trader
