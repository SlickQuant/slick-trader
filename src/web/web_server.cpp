#include <slick-trader/web/web_server.hpp>
#include <slick-trader/core/position_tracker.hpp>
#include <slick-trader/core/pnl_calculator.hpp>
#include <slick-trader/core/risk_manager.hpp>
#include <slick-trader/strategy/strategy_manager.hpp>
#include <slick-trader/strategy/strategy_base.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <set>

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

                // Start polling market data after listen succeeds
                uWS::Loop::get()->defer([this]() {
                    process_market_data_events();
                });
            } else {
                LOG_ERROR("WebServer: Failed to listen on port {}", config_.port);
                running_.store(false, std::memory_order_release);
            }
        });

        // Run event loop (blocking)
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

    // POST: Submit manual order
    app.post("/api/orders", [this](auto* res, auto* req) {
        res->onData([this, res](std::string_view data, bool last) {
            if (last) {
                handle_post_order(res, nullptr, data);
            }
        });
        res->onAborted([]() {});
    });

    // POST: Strategy control (start/stop)
    app.post("/api/strategies/:strategy_id/control", [this](auto* res, auto* req) {
        std::string strategy_id(req->getParameter(0));
        res->onData([this, res, strategy_id](std::string_view data, bool last) {
            if (last) {
                handle_post_strategy_control(res, nullptr, strategy_id, data);
            }
        });
        res->onAborted([]() {});
    });

    // GET: Risk limits for strategy
    app.get("/api/strategies/:strategy_id/risk", [this](auto* res, auto* req) {
        std::string strategy_id(req->getParameter(0));
        handle_get_risk_limits(res, req, strategy_id);
    });

    // PUT: Update risk limits for strategy
    app.put("/api/strategies/:strategy_id/risk", [this](auto* res, auto* req) {
        std::string strategy_id(req->getParameter(0));
        res->onData([this, res, strategy_id](std::string_view data, bool last) {
            if (last) {
                handle_put_risk_limits(res, nullptr, strategy_id, data);
            }
        });
        res->onAborted([]() {});
    });

    // POST: Halt strategy
    app.post("/api/strategies/:strategy_id/halt", [this](auto* res, auto* req) {
        std::string strategy_id(req->getParameter(0));
        res->onData([this, res, strategy_id](std::string_view data, bool last) {
            if (last) {
                handle_post_halt_strategy(res, nullptr, strategy_id, data);
            }
        });
        res->onAborted([]() {});
    });

    // POST: Resume strategy
    app.post("/api/strategies/:strategy_id/resume", [this](auto* res, auto* req) {
        std::string strategy_id(req->getParameter(0));
        handle_post_resume_strategy(res, req, strategy_id);
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

    // Static file serving
    app.get("/*", [this](auto* res, auto* req) {
        std::string url(req->getUrl());

        // Default to index.html for root path
        if (url == "/" || url == "") {
            url = "/index.html";
        }

        // Construct file path
        std::filesystem::path file_path = std::filesystem::path(config_.static_files_path) / url.substr(1);

        // Security check: prevent directory traversal
        auto canonical_base = std::filesystem::weakly_canonical(config_.static_files_path);
        auto canonical_path = std::filesystem::weakly_canonical(file_path);

        if (canonical_path.string().find(canonical_base.string()) != 0) {
            send_error_response(res, "Invalid path", 403);
            return;
        }

        // Check if file exists
        if (!std::filesystem::exists(file_path) || !std::filesystem::is_regular_file(file_path)) {
            send_error_response(res, "File not found", 404);
            return;
        }

        // Read file
        std::ifstream file(file_path, std::ios::binary);
        if (!file) {
            send_error_response(res, "Failed to read file", 500);
            return;
        }

        std::ostringstream content;
        content << file.rdbuf();
        std::string file_content = content.str();

        // Determine content type
        std::string content_type = "application/octet-stream";
        std::string extension = file_path.extension().string();

        if (extension == ".html") content_type = "text/html";
        else if (extension == ".css") content_type = "text/css";
        else if (extension == ".js") content_type = "application/javascript";
        else if (extension == ".json") content_type = "application/json";
        else if (extension == ".png") content_type = "image/png";
        else if (extension == ".jpg" || extension == ".jpeg") content_type = "image/jpeg";
        else if (extension == ".svg") content_type = "image/svg+xml";
        else if (extension == ".ico") content_type = "image/x-icon";

        // Send response
        add_cors_headers(res);
        res->writeStatus("200 OK");
        res->writeHeader("Content-Type", content_type);
        res->writeHeader("Cache-Control", "public, max-age=3600");
        res->end(file_content);
    });
}

void WebServer::handle_get_strategies(uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
    try {
        json response = json::array();

        const auto& strategies = strategy_manager_.get_all_strategies();
        for (const auto& strategy : strategies) {
            // Collect unique instruments from all subscriptions
            std::set<std::string> instruments_set;
            auto subscriptions = strategy->get_subscriptions();
            for (const auto& sub : subscriptions) {
                for (const auto& instrument : sub.instruments) {
                    instruments_set.insert(instrument);
                }
            }

            // Convert to array
            json instruments_array = json::array();
            for (const auto& inst : instruments_set) {
                instruments_array.push_back(inst);
            }

            response.push_back({
                {"name", strategy->name()},
                {"id", strategy->name()},  // Use name as ID for now
                {"status", "running"},      // TODO: Add proper status tracking
                {"type", "plugin"},         // TODO: Add strategy type
                {"instruments", instruments_array}
            });
        }

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
            std::string channel = msg.value("channel", "");
            std::string instrument = msg.value("instrument", "");

            if (channel == "level2" && !instrument.empty()) {
                level2_subscriptions_[instrument].clients.insert(ws);
                LOG_INFO("WebServer: Client subscribed to level2 for {}", instrument);

                json response = {
                    {"type", "subscribed"},
                    {"channel", channel},
                    {"instrument", instrument}
                };
                ws->send(response.dump(), uWS::OpCode::TEXT);
            } else {
                LOG_WARN("WebServer: Invalid subscription request - channel: {}, instrument: {}", channel, instrument);
            }
        } else if (type == "unsubscribe") {
            std::string channel = msg.value("channel", "");
            std::string instrument = msg.value("instrument", "");

            if (channel == "level2" && !instrument.empty()) {
                auto it = level2_subscriptions_.find(instrument);
                if (it != level2_subscriptions_.end()) {
                    it->second.clients.erase(ws);
                    LOG_INFO("WebServer: Client unsubscribed from level2 for {}", instrument);
                }
            }
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
    // Remove from all subscriptions
    for (auto& [instrument, sub] : level2_subscriptions_) {
        sub.clients.erase(ws);
    }

    LOG_INFO("WebServer: WebSocket connection closed and cleaned up");
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

void WebServer::handle_post_order(uWS::HttpResponse<false>* res, uWS::HttpRequest* req, std::string_view body) {
    try {
        auto order_data = json::parse(body);

        // Validate required fields
        if (!order_data.contains("strategy_id") || !order_data.contains("instrument") ||
            !order_data.contains("side") || !order_data.contains("quantity") || !order_data.contains("broker_id")) {
            send_error_response(res, "Missing required fields: strategy_id, instrument, broker_id, side, quantity", 400);
            return;
        }

        std::string strategy_id = order_data["strategy_id"];
        std::string instrument = order_data["instrument"];
        uint8_t broker_id_raw = order_data["broker_id"];
        std::string side_str = order_data["side"];
        double quantity = order_data["quantity"];

        // Optional fields
        std::string order_type = order_data.value("type", "market");
        double price = order_data.value("price", std::numeric_limits<double>::quiet_NaN());
        uint8_t tif_raw = order_data.value("time_in_force", 0);  // Default to GTC

        // Validate broker_id
        BrokerId broker_id = static_cast<BrokerId>(broker_id_raw);
        if (broker_id == BrokerId::UNKNOWN) {
            send_error_response(res, "Invalid broker_id: must be 1 (Coinbase), 2 (Alpaca), or 3 (Simulated)", 400);
            return;
        }

        // Validate time_in_force
        TimeInForce tif = static_cast<TimeInForce>(tif_raw);
        if (tif_raw > 3) {  // Valid range is 0-3
            send_error_response(res, "Invalid time_in_force: must be 0 (GTC), 1 (IOC), 2 (FOK), or 3 (GTD)", 400);
            return;
        }

        // Validate side
        Side side;
        if (side_str == "buy" || side_str == "BUY") {
            side = Side::BUY;
        } else if (side_str == "sell" || side_str == "SELL") {
            side = Side::SELL;
        } else {
            send_error_response(res, "Invalid side: must be 'buy' or 'sell'", 400);
            return;
        }

        // Validate order type
        OrderType type;
        if (order_type == "market") {
            type = OrderType::MARKET;
        } else if (order_type == "limit") {
            type = OrderType::LIMIT;
            if (price <= 0.0) {
                send_error_response(res, "Limit orders require a valid price", 400);
                return;
            }
        } else {
            send_error_response(res, "Invalid order type: must be 'market' or 'limit'", 400);
            return;
        }

        // Submit the order through the strategy
        LOG_INFO("WebServer: Manual order request - Strategy: {}, Instrument: {}, Broker: {}, Side: {}, {}@{}, Type: {}, TIF: {}",
                 strategy_id, instrument, static_cast<int>(broker_id), side_str, quantity, price, order_type, tif_raw);

        auto strategy = strategy_manager_.get_strategy(strategy_id);
        if (!strategy) {
            send_error_response(res, "Strategy not found", 404);
            return;
        }
        reinterpret_pointer_cast<StrategyBase>(strategy)->submit_order(instrument, broker_id, side, to_qty_t(quantity), type, to_price_t(price), tif);

        json response = {
            {"success", true},
            {"message", "Order submitted successfully"},
            {"order", {
                {"strategy_id", strategy_id},
                {"instrument", instrument},
                {"broker_id", broker_id_raw},
                {"side", side_str},
                {"quantity", quantity},
                {"type", order_type},
                {"price", price},
                {"time_in_force", tif_raw}
            }}
        };

        send_json_response(res, response.dump(), 202);  // 202 Accepted
    } catch (const json::exception& e) {
        send_error_response(res, std::string("Invalid JSON: ") + e.what(), 400);
    } catch (const std::exception& e) {
        send_error_response(res, e.what(), 500);
    }
}

void WebServer::handle_post_strategy_control(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                               const std::string& strategy_id, std::string_view body) {
    try {
        auto control_data = json::parse(body);

        if (!control_data.contains("action")) {
            send_error_response(res, "Missing required field: action", 400);
            return;
        }

        std::string action = control_data["action"];

        // TODO: Implement actual strategy control
        // For now, just log and return
        LOG_INFO("WebServer: Strategy control request - Strategy: {}, Action: {}", strategy_id, action);

        if (action == "start" || action == "stop" || action == "pause" || action == "resume") {
            json response = {
                {"success", true},
                {"message", "Strategy control not yet implemented"},
                {"strategy_id", strategy_id},
                {"action", action}
            };
            send_json_response(res, response.dump());
        } else {
            send_error_response(res, "Invalid action: must be 'start', 'stop', 'pause', or 'resume'", 400);
        }

    } catch (const json::exception& e) {
        send_error_response(res, std::string("Invalid JSON: ") + e.what(), 400);
    } catch (const std::exception& e) {
        send_error_response(res, e.what(), 500);
    }
}

void WebServer::handle_get_risk_limits(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                         const std::string& strategy_id) {
    try {
        auto limits = risk_manager_.get_limits(strategy_id);

        json response = {
            {"strategy_id", strategy_id},
            {"max_position_size", from_qty_t(limits.max_position_size)},
            {"max_order_value", from_price_t(limits.max_order_value)},
            {"max_orders_per_second", limits.max_orders_per_second},
            {"max_loss_per_strategy", from_price_t(limits.max_loss_per_strategy)},
            {"halted", risk_manager_.is_halted(strategy_id)}
        };

        // Add per-instrument limits if any
        if (!limits.instrument_limits.empty()) {
            json inst_limits = json::object();
            for (const auto& [instrument, inst_limit] : limits.instrument_limits) {
                inst_limits[instrument] = {
                    {"max_position_size", from_qty_t(inst_limit.max_position_size)},
                    {"max_order_value", from_price_t(inst_limit.max_order_value)}
                };
            }
            response["instrument_limits"] = inst_limits;
        }

        send_json_response(res, response.dump());
    } catch (const std::exception& e) {
        send_error_response(res, e.what(), 500);
    }
}

void WebServer::handle_put_risk_limits(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                         const std::string& strategy_id, std::string_view body) {
    try {
        auto update_data = json::parse(body);

        // Get current limits
        auto limits = risk_manager_.get_limits(strategy_id);

        // Update provided fields
        if (update_data.contains("max_position_size")) {
            limits.max_position_size = to_qty_t(update_data["max_position_size"].get<double>());
        }

        if (update_data.contains("max_order_value")) {
            limits.max_order_value = to_price_t(update_data["max_order_value"].get<double>());
        }

        if (update_data.contains("max_orders_per_second")) {
            limits.max_orders_per_second = update_data["max_orders_per_second"].get<uint32_t>();
        }

        if (update_data.contains("max_loss_per_strategy")) {
            limits.max_loss_per_strategy = to_price_t(update_data["max_loss_per_strategy"].get<double>());
        }

        // Update per-instrument limits
        if (update_data.contains("instrument_limits") && update_data["instrument_limits"].is_object()) {
            for (const auto& [instrument, inst_data] : update_data["instrument_limits"].items()) {
                InstrumentRiskLimits inst_limits;

                if (inst_data.contains("max_position_size")) {
                    inst_limits.max_position_size = to_qty_t(inst_data["max_position_size"].get<double>());
                }

                if (inst_data.contains("max_order_value")) {
                    inst_limits.max_order_value = to_price_t(inst_data["max_order_value"].get<double>());
                }

                limits.instrument_limits[instrument] = inst_limits;
            }
        }

        // Apply updated limits
        risk_manager_.set_limits(strategy_id, limits);

        LOG_INFO("WebServer: Updated risk limits for strategy {}", strategy_id);

        json response = {
            {"success", true},
            {"message", "Risk limits updated successfully"},
            {"strategy_id", strategy_id}
        };

        send_json_response(res, response.dump());
    } catch (const json::exception& e) {
        send_error_response(res, std::string("Invalid JSON: ") + e.what(), 400);
    } catch (const std::exception& e) {
        send_error_response(res, e.what(), 500);
    }
}

void WebServer::handle_post_halt_strategy(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                            const std::string& strategy_id, std::string_view body) {
    try {
        auto halt_data = json::parse(body);
        std::string reason = halt_data.value("reason", "Manual halt via API");

        risk_manager_.halt_strategy(strategy_id, reason);

        LOG_INFO("WebServer: Halted strategy {} - Reason: {}", strategy_id, reason);

        json response = {
            {"success", true},
            {"message", "Strategy halted successfully"},
            {"strategy_id", strategy_id},
            {"reason", reason}
        };

        send_json_response(res, response.dump());
    } catch (const json::exception& e) {
        send_error_response(res, std::string("Invalid JSON: ") + e.what(), 400);
    } catch (const std::exception& e) {
        send_error_response(res, e.what(), 500);
    }
}

void WebServer::handle_post_resume_strategy(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                              const std::string& strategy_id) {
    try {
        risk_manager_.resume_strategy(strategy_id);

        LOG_INFO("WebServer: Resumed strategy {}", strategy_id);

        json response = {
            {"success", true},
            {"message", "Strategy resumed successfully"},
            {"strategy_id", strategy_id}
        };

        send_json_response(res, response.dump());
    } catch (const std::exception& e) {
        send_error_response(res, e.what(), 500);
    }
}

void WebServer::process_market_data_events() {
    if (!running_.load(std::memory_order_acquire)) {
        return;  // Stop polling if server is shutting down
    }

    if (!md_queue_) {
        // Re-schedule even if no queue yet
        uWS::Loop::get()->defer([this]() { process_market_data_events(); });
        return;
    }

    // Process up to 100 events per tick (avoid blocking WebSocket I/O)
    int events_processed = 0;
    for (int i = 0; i < 100; ++i) {
        auto [data, size] = md_queue_->read(ws_md_cursor_);
        if (!data) break;  // No more data available

        const Event* event = reinterpret_cast<const Event*>(data);

        if (event->type == EventType::LEVEL2_UPDATE) {
            const auto* level2_event = reinterpret_cast<const Level2UpdateEvent*>(data);
            broadcast_level2_update(*level2_event);
            events_processed++;
        }
    }

    if (events_processed > 1) {
        LOG_DEBUG("WebServer: Processed {} level2 events", events_processed);
    }

    // Re-schedule for next tick (~10ms interval via uWS event loop)
    uWS::Loop::get()->defer([this]() { process_market_data_events(); });
}

void WebServer::broadcast_level2_update(const Level2UpdateEvent& event) {
    std::string instrument(event.instrument);

    // Build JSON
    json level2_update;
    level2_update["type"] = "level2";
    level2_update["instrument"] = instrument;
    level2_update["timestamp"] = event.event_time_ns / 1000000; // Convert to ms
    level2_update["seq_num"] = event.sequence_num;
    level2_update["is_snapshot"] = event.is_snapshot;

    json levels = json::array();
    for (uint16_t i = 0; i < event.num_levels; ++i) {
        levels.push_back({
            from_price_t(event.levels[i].price),
            from_qty_t(event.levels[i].quantity),
            event.levels[i].side
        });
    }
    level2_update["levels"] = levels;

    std::string message = level2_update.dump();

    // Broadcast to subscribers
    auto it = level2_subscriptions_.find(instrument);
    if (it != level2_subscriptions_.end() && !it->second.clients.empty()) {
        // LOG_DEBUG("WebServer: Broadcasting {} orderbook update to {} client(s)",
        //           instrument, it->second.clients.size());
        for (auto* ws : it->second.clients) {
            ws->send(message, uWS::OpCode::TEXT);
        }
    } else {
        static uint64_t unsubscribed_count = 0;
        if (++unsubscribed_count % 100 == 0) {  // Log every 100th to avoid spam
            LOG_DEBUG("WebServer: No subscribers for {} orderbook (total unsubscribed events: {})",
                      instrument, unsubscribed_count);
        }
    }
}

void WebServer::broadcast_orderbook(const OrderBookEvent& event) {
    std::string instrument(event.instrument);

    // Build JSON
    json orderbook;
    orderbook["type"] = "orderbook";
    orderbook["instrument"] = instrument;
    orderbook["timestamp"] = event.event_time_ns / 1000000; // Convert to ms
    orderbook["is_snapshot"] = event.is_snapshot;

    // Bids
    json bids = json::array();
    for (uint16_t i = 0; i < event.num_bids; ++i) {
        bids.push_back({
            from_price_t(event.bids[i].price),
            from_qty_t(event.bids[i].quantity)
        });
    }
    orderbook["bids"] = bids;

    // Asks
    json asks = json::array();
    for (uint16_t i = 0; i < event.num_asks; ++i) {
        asks.push_back({
            from_price_t(event.asks[i].price),
            from_qty_t(event.asks[i].quantity)
        });
    }
    orderbook["asks"] = asks;

    std::string message = orderbook.dump();

    // Broadcast to subscribers
    auto it = level2_subscriptions_.find(instrument);
    if (it != level2_subscriptions_.end() && !it->second.clients.empty()) {
        // LOG_DEBUG("WebServer: Broadcasting {} orderbook update to {} client(s)",
        //           instrument, it->second.clients.size());
        for (auto* ws : it->second.clients) {
            ws->send(message, uWS::OpCode::TEXT);
        }
    } else {
        static uint64_t unsubscribed_count = 0;
        if (++unsubscribed_count % 100 == 0) {  // Log every 100th to avoid spam
            LOG_DEBUG("WebServer: No subscribers for {} orderbook (total unsubscribed events: {})",
                      instrument, unsubscribed_count);
        }
    }
}

} // namespace slick::trader
