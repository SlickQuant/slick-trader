#include <slick-trader/web/web_server.hpp>
#include <slick-trader/core/position_tracker.hpp>
#include <slick-trader/core/pnl_calculator.hpp>
#include <slick-trader/strategy/strategy_manager.hpp>
#include <nlohmann/json.hpp>
#include <chrono>

using json = nlohmann::json;

namespace slick::trader {

bool WebServer::initialize(const Config& config) {
    config_ = config;
    LOG_INFO("WebServer: Initialized on {}:{}", config_.host, config_.port);
    return true;
}

void WebServer::start() {
    if (running_.load(std::memory_order_relaxed)) {
        LOG_WARN("WebServer: Already running");
        return;
    }

    LOG_INFO("WebServer: Starting on port {}", config_.port);
    running_.store(true, std::memory_order_release);
    worker_thread_ = std::thread(&WebServer::run, this);
}

void WebServer::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    LOG_INFO("WebServer: Stopping");
    running_.store(false, std::memory_order_release);

    // Close listen socket to unblock uWS loop
    if (listen_socket_) {
        us_listen_socket_close(0, listen_socket_);
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    LOG_INFO("WebServer: Stopped");
}

void WebServer::run() {
    LOG_INFO("WebServer: Worker thread started");

    try {
        uWS::App app;
        setup_routes(app);

        app.listen(config_.port, [this](auto* socket) {
            if (socket) {
                listen_socket_ = socket;
                LOG_INFO("WebServer: Listening on port {}", config_.port);
            } else {
                LOG_ERROR("WebServer: Failed to listen on port {}", config_.port);
                running_.store(false, std::memory_order_release);
            }
        });

        // Run event loop
        app.run();

    } catch (const std::exception& e) {
        LOG_ERROR("WebServer: Exception in worker thread: {}", e.what());
    }

    LOG_INFO("WebServer: Worker thread stopped");
}

void WebServer::setup_routes(uWS::App &app) {
    // Health check
    app.get("/api/health", [this](auto* res, auto* req) {
        send_json_response(res, R"({"status":"ok"})");
    });

    // Get all strategies
    app.get("/api/strategies", [this](auto* res, auto* req) {
        handle_get_strategies(res, req);
    });

    // Get all positions
    app.get("/api/positions", [this](auto* res, auto* req) {
        handle_get_positions(res, req);
    });

    // Get all PnL
    app.get("/api/pnl", [this](auto* res, auto* req) {
        handle_get_pnl(res, req);
    });

    // Get positions for specific strategy
    app.get("/api/positions/:strategy_id", [this](auto* res, auto* req) {
        std::string strategy_id(req->getParameter(0));
        handle_get_strategy_positions(res, req, strategy_id);
    });

    // Get PnL for specific strategy
    app.get("/api/pnl/:strategy_id", [this](auto* res, auto* req) {
        std::string strategy_id(req->getParameter(0));
        handle_get_strategy_pnl(res, req, strategy_id);
    });

    // WebSocket endpoint
    app.ws<void*>("/*", {
        .open = [this](auto* ws) {
            handle_ws_open(ws);
        },
        .message = [this](auto* ws, std::string_view message, uWS::OpCode opCode) {
            handle_ws_message(ws, message);
        },
        .close = [this](auto* ws, int code, std::string_view message) {
            handle_ws_close(ws);
        }
    });

    // Static file serving (fallback)
    app.get("/*", [this](auto* res, auto* req) {
        send_json_response(res, R"({"message":"SlickTrader API v1.0"})", 200);
    });
}

void WebServer::handle_get_strategies(uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
    try {
        json response = json::array();

        // TODO: Implement get_all_strategies() in StrategyManager
        // For now, return empty array

        send_json_response(res, response.dump());
    } catch (const std::exception& e) {
        send_error_response(res, e.what(), 500);
    }
}

void WebServer::handle_get_positions(uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
    try {
        auto all_positions = position_tracker_.get_all_positions();

        json response = json::object();
        for (const auto& [strategy_id, positions] : all_positions) {
            json strategy_positions = json::array();

            for (const auto& [instrument, position] : positions) {
                if (position.is_flat()) continue;  // Skip flat positions

                strategy_positions.push_back({
                    {"instrument", position.instrument},
                    {"quantity", from_qty_t(position.quantity)},
                    {"avg_price", from_price_t(position.avg_price)},
                    {"realized_pnl", from_price_t(position.realized_pnl)},
                    {"last_update_time", position.last_update_time}
                });
            }

            if (!strategy_positions.empty()) {
                response[strategy_id] = strategy_positions;
            }
        }

        send_json_response(res, response.dump());
    } catch (const std::exception& e) {
        send_error_response(res, e.what(), 500);
    }
}

void WebServer::handle_get_pnl(uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
    try {
        const uint64_t now_ns = std::chrono::system_clock::now().time_since_epoch().count();
        auto all_pnl = pnl_calculator_.calculate_all_pnl(now_ns);

        json response = json::object();
        for (const auto& [strategy_id, pnl] : all_pnl) {
            response[strategy_id] = {
                {"realized_pnl", pnl.get_realized()},
                {"unrealized_pnl", pnl.get_unrealized()},
                {"total_pnl", pnl.get_total()},
                {"timestamp_ns", pnl.timestamp_ns}
            };
        }

        send_json_response(res, response.dump());
    } catch (const std::exception& e) {
        send_error_response(res, e.what(), 500);
    }
}

void WebServer::handle_get_strategy_positions(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                                const std::string& strategy_id) {
    try {
        auto positions = position_tracker_.get_all_positions(strategy_id);

        json response = json::array();
        for (const auto& [instrument, position] : positions) {
            if (position.is_flat()) continue;  // Skip flat positions

            response.push_back({
                {"instrument", position.instrument},
                {"quantity", from_qty_t(position.quantity)},
                {"avg_price", from_price_t(position.avg_price)},
                {"realized_pnl", from_price_t(position.realized_pnl)},
                {"last_update_time", position.last_update_time}
            });
        }

        send_json_response(res, response.dump());
    } catch (const std::exception& e) {
        send_error_response(res, e.what(), 500);
    }
}

void WebServer::handle_get_strategy_pnl(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                         const std::string& strategy_id) {
    try {
        const uint64_t now_ns = std::chrono::system_clock::now().time_since_epoch().count();
        auto pnl = pnl_calculator_.calculate_pnl(strategy_id, now_ns);

        json response = {
            {"strategy_id", pnl.strategy_id},
            {"realized_pnl", pnl.get_realized()},
            {"unrealized_pnl", pnl.get_unrealized()},
            {"total_pnl", pnl.get_total()},
            {"timestamp_ns", pnl.timestamp_ns}
        };

        send_json_response(res, response.dump());
    } catch (const std::exception& e) {
        send_error_response(res, e.what(), 500);
    }
}

void WebServer::handle_ws_open(uWS::WebSocket<false, true, void*>* ws) {
    LOG_INFO("WebServer: WebSocket connection opened");

    // Send welcome message
    std::string welcome = R"({"type":"welcome","message":"Connected to SlickTrader"})";
    ws->send(welcome, uWS::OpCode::TEXT);
}

void WebServer::handle_ws_message(uWS::WebSocket<false, true, void*>* ws, std::string_view message) {
    try {
        auto msg = json::parse(message);
        std::string type = msg.value("type", "");

        if (type == "subscribe") {
            // TODO: Implement subscription management
            LOG_DEBUG("WebServer: WebSocket subscribe request");
            ws->send(R"({"type":"subscribed"})", uWS::OpCode::TEXT);
        } else if (type == "ping") {
            ws->send(R"({"type":"pong"})", uWS::OpCode::TEXT);
        } else {
            LOG_WARN("WebServer: Unknown WebSocket message type: {}", type);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("WebServer: Error handling WebSocket message: {}", e.what());
    }
}

void WebServer::handle_ws_close(uWS::WebSocket<false, true, void*>* ws) {
    LOG_INFO("WebServer: WebSocket connection closed");
}

void WebServer::send_json_response(uWS::HttpResponse<false>* res, const std::string& json, int status) {
    add_cors_headers(res);
    res->writeStatus(std::to_string(status));
    res->writeHeader("Content-Type", "application/json");
    res->end(json);
}

void WebServer::send_error_response(uWS::HttpResponse<false>* res, const std::string& error, int status) {
    json response = {
        {"error", error}
    };
    send_json_response(res, response.dump(), status);
}

void WebServer::add_cors_headers(uWS::HttpResponse<false>* res) {
    if (config_.enable_cors) {
        res->writeHeader("Access-Control-Allow-Origin", "*");
        res->writeHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res->writeHeader("Access-Control-Allow-Headers", "Content-Type");
    }
}

} // namespace slick::trader
