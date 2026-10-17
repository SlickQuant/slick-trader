#include <slick-trader/web/web_server.hpp>
#include <slick-trader/core/position_tracker.hpp>
#include <slick-trader/core/pnl_calculator.hpp>
#include <slick-trader/core/risk_manager.hpp>
#include <slick-trader/core/strategy_manager.hpp>
#include <slick-trader/strategy/strategy_base.hpp>
#include <slick-trader/strategy/manual_strategy.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick-trader/events/quote_event.hpp>
#include <slick-trader/events/strategy_config_event.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <chrono>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <set>

namespace slick::trader {

bool WebServer::initialize(const json& config) {
    config_.port = config.value("web_server_port", 8080);
    config_.enable_cors = config.value("web_server_cors", true);
    config_.static_files_path = config.value("web_server_static_path", "web/static");

    // Load ladder config if present
    if (config.contains("ladder_config")) {
        ladder_config_ = config["ladder_config"];
        LOG_INFO("WebServer: Loaded ladder config");
    } else {
        // Set defaults if not in config
        ladder_config_ = {
            {"default_qty", 0.01},
            {"per_instrument", json::object()}
        };
        LOG_INFO("WebServer: Using default ladder config");
    }

    // Register our producer on the event bus. Strategy parameter updates go out this way so
    // the StrategyManager worker applies them on its own thread. add_producer() is setup-only
    // and no bus thread has started yet, so here is the place to do it.{

    // The id follows the same convention StrategyManager uses for its top-of-book producer:
    // one past whatever is already registered. A collision is only possible if a broker
    // claimed a sparse id, so scan for a free one rather than letting the throw escape.
    const auto config_queue_size   = config.value("config_queue_size", 1u << 16 /* 64K */);
    const auto config_control_size = config.value("config_control_size", 4096u);
    try {
        uint32_t producer_id = static_cast<uint32_t>(event_mux_.producer_count());
        while (event_mux_.has_producer(producer_id)) {
            ++producer_id;
        }
        config_producer_buffer_ = event_mux_.add_producer(producer_id, config_queue_size,
                                                            config_control_size);
        LOG_INFO("WebServer: Publishing strategy config updates as producer {}", producer_id);
    } catch (const std::exception& e) {
        // Not fatal: everything except live parameter updates still works.
        LOG_ERROR("WebServer: Could not register the config producer: {}", e.what());
    }

    LOG_INFO("WebServer: Initialized on {}:{}", config_.host, config_.port);
    return true;
}

void WebServer::start() {
    if (running_.load(std::memory_order_relaxed)) [[unlikely]] {
        LOG_WARN("WebServer: Already running");
        return;
    }

    LOG_INFO("WebServer: Starting on port {}", config_.port);
    running_.store(true, std::memory_order_release);
    worker_thread_ = std::thread(&WebServer::run, this);

    LOG_INFO("");
    LOG_INFO("Available endpoints:");
    LOG_INFO("  GET /api/health          - Health check");
    LOG_INFO("  GET /api/positions       - All positions");
    LOG_INFO("  GET /api/positions/:id   - Strategy positions");
    LOG_INFO("  GET /api/pnl             - All PnL snapshots");
    LOG_INFO("  GET /api/pnl/:id         - Strategy PnL");
    LOG_INFO("  GET /api/strategies      - Strategy list");
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

                // Start polling market data and order events after listen succeeds
                uWS::Loop::get()->defer([this]() {
                    process_events();
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

    // Broker status
    app.get("/api/brokers", [this](auto* res, auto* req) {
        handle_get_brokers(res, req);
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

    // GET: All open orders across all strategies
    app.get("/api/orders", [this](auto* res, auto* req) {
        handle_get_orders(res, req);
    });

    // DELETE: Cancel all open orders (must be registered before the parameterized route)
    app.del("/api/orders/cancel-all", [this](auto* res, auto* req) {
        res->onData([this, res](std::string_view, bool last) {
            if (last) handle_delete_all_orders(res, nullptr);
        });
        res->onAborted([]() {});
    });

    // PUT: Modify a specific order
    app.put("/api/orders/:strategy_id/:order_id", [this](auto* res, auto* req) {
        std::string sid(req->getParameter(0)), oid(req->getParameter(1));
        res->onData([this, res, sid, oid](std::string_view data, bool last) {
            if (last) handle_put_order(res, nullptr, sid, oid, data);
        });
        res->onAborted([]() {});
    });

    // DELETE: Cancel a specific order
    app.del("/api/orders/:strategy_id/:order_id", [this](auto* res, auto* req) {
        std::string sid(req->getParameter(0)), oid(req->getParameter(1));
        res->onData([this, res, sid, oid](std::string_view data, bool last) {
            if (last) handle_delete_order(res, nullptr, sid, oid, data);
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

    // GET: Current strategy parameters
    app.get("/api/strategies/:strategy_id/config", [this](auto* res, auto* req) {
        std::string strategy_id(req->getParameter(0));
        handle_get_strategy_config(res, req, strategy_id);
    });

    // PUT: Update strategy parameters at runtime and persist to config.json
    app.put("/api/strategies/:strategy_id/config", [this](auto* res, auto* req) {
        std::string strategy_id(req->getParameter(0));
        res->onData([this, res, strategy_id](std::string_view data, bool last) {
            if (last) {
                handle_put_strategy_config(res, nullptr, strategy_id, data);
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

    // GET: Ladder configuration
    app.get("/api/ladder-config", [this](auto* res, auto* req) {
        handle_get_ladder_config(res, req);
    });

    // PUT: Update ladder configuration
    app.put("/api/ladder-config", [this](auto* res, auto* req) {
        auto body_ptr = std::make_shared<std::string>();
        res->onData([this, res, body_ptr](std::string_view data, bool last) mutable {
            body_ptr->append(data.data(), data.size());
            if (last) {
                handle_put_ladder_config(res, nullptr, *body_ptr);
            }
        });
        res->onAborted([]() {});
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

void WebServer::handle_get_brokers(uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
    try {
        auto& reg = BrokerStatusRegistry::instance();
        json brokers = json::array();
        const auto& all = reg.all();
        for (size_t i = 1; i < all.size(); ++i) {
            const auto& s = all[i];
            if (s.name[0] == '\0') continue;
            brokers.push_back({
                {"id", i},
                {"name", s.name},
                {"md", {
                    {"enabled", s.md_enabled.load(std::memory_order_acquire)},
                    {"connected", s.md_connected.load(std::memory_order_acquire)}
                }},
                {"order", {
                    {"enabled", s.order_enabled.load(std::memory_order_acquire)},
                    {"connected", s.order_connected.load(std::memory_order_acquire)}
                }}
            });
        }
        send_json_response(res, json{{"brokers", brokers}}.dump());
    } catch (const std::exception& e) {
        send_error_response(res, e.what(), 500);
    }
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

            const std::string sname = strategy->name();
            std::string status;
            std::string pause_reason;
            if (risk_manager_.is_halted(sname)) {
                status = "halted";
                pause_reason = risk_manager_.get_halt_reason(sname);
            } else if (strategy_manager_.is_auto_paused(sname)) {
                status = "paused";
                pause_reason = strategy_manager_.get_auto_pause_reason(sname);
            } else if (auto* base = dynamic_cast<StrategyBase*>(strategy.get()); base && base->is_running()) {
                status = "running";
            } else {
                status = "stopped";
            }

            response.push_back({
                {"name", sname},
                {"id", sname},
                {"status", status},
                {"halt_reason", pause_reason},
                {"type", "plugin"},
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

                const price_t mark = pnl_calculator_.get_mark_price(instrument);
                const double unrealized = position.get_unrealized_pnl(mark);

                strategy_positions.push_back({
                    {"instrument", position.instrument},
                    {"quantity", from_qty_t(position.quantity)},
                    {"avg_price", from_price_t(position.avg_price)},
                    {"realized_pnl", from_price_t(position.realized_pnl)},
                    {"unrealized_pnl", unrealized},
                    {"mark_price", from_price_t(mark)},
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
        const uint64_t now_ns = get_timestamp_ns();
        auto all_pnl = pnl_calculator_.calculate_all_pnl(now_ns);

        json response = json::object();
        for (const auto& [strategy_id, pnl] : all_pnl) {
            response[strategy_id] = {
                {"realized_pnl", pnl.get_realized()},
                {"unrealized_pnl", pnl.get_unrealized()},
                {"total_fees", pnl.get_fees()},
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
        const uint64_t now_ns = get_timestamp_ns();
        auto pnl = pnl_calculator_.calculate_pnl(strategy_id, now_ns);

        json response = {
            {"strategy_id", pnl.strategy_id},
            {"realized_pnl", pnl.get_realized()},
            {"unrealized_pnl", pnl.get_unrealized()},
            {"total_fees", pnl.get_fees()},
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
    all_ws_clients_.insert(ws);

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
    // Remove from all connected clients and subscriptions
    all_ws_clients_.erase(ws);
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
        if (!order_data.contains("instrument") ||
            !order_data.contains("side") || !order_data.contains("quantity") || !order_data.contains("broker_id")) {
            send_error_response(res, "Missing required fields: instrument, broker_id, side, quantity", 400);
            return;
        }

        const std::string strategy_id = ManualStrategy::ID;
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
        if (broker_id == BrokerId::UNKNOWN || broker_id >= BrokerId::_BROKER_ID_COUNT_) {
            send_error_response(res, "Invalid broker_id: must be 1 (Coinbase), 2 (Alpaca), 3 (Hyperliquid), 4 (Simulated) or 5 (CME)", 400);
            return;
        }

        // Validate time_in_force
        TimeInForce tif = static_cast<TimeInForce>(tif_raw);
        if (tif_raw > static_cast<uint8_t>(TimeInForce::DAY)) {  // Valid range is 0-4
            send_error_response(res, "Invalid time_in_force: must be 0 (GTC), 1 (IOC), 2 (FOK), 3 (GTD) or 4 (DAY)", 400);
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
            LOG_ERROR("WebServer: strategy {} not found", strategy_id);
            send_error_response(res, "Strategy not found", 404);
            return;
        }
        std::string client_order_id = reinterpret_pointer_cast<StrategyBase>(strategy)->submit_order(
            instrument, broker_id, side, to_qty_t(quantity), type, to_price_t(price), tif);

        // Seed WebServer-side tracking so handle_get_orders() can return it immediately
        if (!client_order_id.empty()) {
            TrackedOrder t;
            t.strategy_id     = strategy_id;
            t.client_order_id = client_order_id;
            t.instrument      = instrument;
            t.side            = side_str;
            t.type            = static_cast<int>(type);
            t.price           = price;
            t.qty             = quantity;
            t.remaining_qty   = quantity;
            t.status          = "PENDING_NEW";
            t.created_time    = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            t.last_update_time = t.created_time;
            open_orders_[client_order_id] = std::move(t);
        }

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

        if (action == "start" || action == "resume") {
            bool ok = strategy_manager_.start_strategy(strategy_id);
            if (!ok) {
                send_error_response(res, "Strategy not found: " + strategy_id, 404);
                return;
            }
            send_json_response(res, json{{"success", true}, {"strategy_id", strategy_id}, {"action", action}}.dump());
        } else if (action == "stop" || action == "pause") {
            bool ok = strategy_manager_.stop_strategy(strategy_id);
            if (!ok) {
                send_error_response(res, "Strategy not found: " + strategy_id, 404);
                return;
            }
            send_json_response(res, json{{"success", true}, {"strategy_id", strategy_id}, {"action", action}}.dump());
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

void WebServer::handle_get_strategy_config(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                             const std::string& strategy_id) {
    auto strategy = strategy_manager_.get_strategy(strategy_id);
    if (!strategy) {
        send_error_response(res, "Strategy not found: " + strategy_id, 404);
        return;
    }
    auto* base = dynamic_cast<StrategyBase*>(strategy.get());
    if (!base) {
        send_error_response(res, "Strategy does not support config", 400);
        return;
    }
    send_json_response(res, base->get_config().dump(2));
}

void WebServer::handle_put_strategy_config(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                            const std::string& strategy_id, std::string_view body) {
    try {
        auto params = json::parse(body);
        if (!params.is_object()) {
            send_error_response(res, "Parameters must be a JSON object", 400);
            return;
        }

        auto strategy = strategy_manager_.get_strategy(strategy_id);
        if (!strategy) {
            send_error_response(res, "Strategy not found: " + strategy_id, 404);
            return;
        }
        auto* base = dynamic_cast<StrategyBase*>(strategy.get());
        if (!base) {
            send_error_response(res, "Strategy does not support config", 400);
            return;
        }

        // Hand the update to the worker thread rather than calling set_config() from here:
        // a strategy's on_config() may touch state that only its own thread owns.
        if (!publish_strategy_config(strategy_id, params)) {
            send_error_response(res, "Could not publish the config update", 503);
            return;
        }
        save_strategy_parameters(strategy_id, params);

        LOG_INFO("WebServer: Published config update for strategy {}", strategy_id);

        json response = {
            {"success", true},
            {"message", "Config updated successfully"},
            {"strategy_id", strategy_id}
        };
        send_json_response(res, response.dump());
    } catch (const json::exception& e) {
        send_error_response(res, std::string("Invalid JSON: ") + e.what(), 400);
    } catch (const std::exception& e) {
        send_error_response(res, e.what(), 500);
    }
}

bool WebServer::publish_strategy_config(const std::string& strategy_id, const nlohmann::json& params) {
    if (!config_producer_buffer_) {
        LOG_ERROR("WebServer: no config producer registered — cannot publish an update for {}",
                  strategy_id);
        return false;
    }

    const std::string text = params.dump();
    if (text.size() > MAX_STRATEGY_CONFIG_LENGTH) {
        LOG_ERROR("WebServer: config for {} is {} bytes, over the {} byte limit",
                  strategy_id, text.size(), MAX_STRATEGY_CONFIG_LENGTH);
        return false;
    }
    if (strategy_id.size() >= MAX_STRATEGY_NAME_LENGTH) {
        LOG_ERROR("WebServer: strategy name '{}' is too long to publish", strategy_id);
        return false;
    }

    // Trailing flexible array, so the published size covers the header plus the JSON text.
    const uint32_t size = StrategyConfigEvent::size_for(text.size());
    return EventSerializer::publish<StrategyConfigEvent>(
        *config_producer_buffer_, size, [&](StrategyConfigEvent& event) {
            event.timestamp_ns = get_timestamp_ns();
            event.set_strategy(strategy_id);
            event.set_config(text);
        });
}

// Mirrors StrategyManager::load_strategies(): the per-strategy files live in
// strategy_configs_folder as *.config.json and are keyed by the "name" INSIDE the
// file, not by the filename, so that is what we match on here.
bool WebServer::save_to_strategy_config_file(const std::string& configs_folder,
                                             const std::string& strategy_id,
                                             const nlohmann::json& params) {
    std::error_code ec;
    if (configs_folder.empty() || !std::filesystem::exists(configs_folder, ec)) {
        return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(configs_folder, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (!entry.path().filename().string().ends_with(".config.json")) continue;

        json strategy_json;
        {
            std::ifstream in(entry.path());
            if (!in.is_open()) continue;
            try {
                strategy_json = json::parse(in);
            } catch (const json::exception& e) {
                LOG_WARN("WebServer: skipping unparsable strategy config {}: {}",
                         entry.path().filename().string(), e.what());
                continue;
            }
        }
        if (strategy_json.value("name", "") != strategy_id) continue;

        // Only the parameters block is ours; enabled and risk_limits stay as written.
        strategy_json["parameters"] = params;

        const auto path = entry.path().string();
        std::ofstream out(path, std::ios::trunc);
        if (!out.is_open()) {
            LOG_ERROR("WebServer: cannot open {} for writing", path);
            return false;
        }
        out << strategy_json.dump(4) << "\n";
        LOG_INFO("WebServer: Persisted config for strategy {} to {}", strategy_id, path);
        return true;
    }
    return false;
}

void WebServer::save_strategy_parameters(const std::string& strategy_id, const nlohmann::json& params) {
    if (config_path_.empty()) {
        LOG_WARN("WebServer: config_path not set — cannot persist strategy config");
        return;
    }
    try {
        std::ifstream in(config_path_);
        auto config = json::parse(in);
        in.close();

        // Preferred layout: one <name>.config.json per strategy in the configs folder.
        // Resolve the folder exactly as StrategyManager::load_strategies() does, so we
        // write the file that will actually be read back at startup.
        if (config.contains("strategies") && config["strategies"].is_object()) {
            const auto& strategies = config["strategies"];
            const auto plugins_folder = strategies.value("strategy_plugins_folder",
                                                         std::string("strategies"));
            const auto configs_folder = strategies.value("strategy_configs_folder",
                                                         plugins_folder);
            if (save_to_strategy_config_file(configs_folder, strategy_id, params)) {
                return;
            }
        }

        // Legacy layout: the parameters live inline in a "strategies" array in the
        // main config. Only rewrite the file when the strategy is actually in it —
        // an unconditional rewrite reports success while persisting nothing.
        if (config.contains("strategies") && config["strategies"].is_array()) {
            for (auto& s : config["strategies"]) {
                if (s.value("name", "") == strategy_id) {
                    s["parameters"] = params;
                    std::ofstream out(config_path_);
                    out << config.dump(2);
                    LOG_INFO("WebServer: Persisted config for strategy {} to {}",
                             strategy_id, config_path_);
                    return;
                }
            }
        }

        LOG_WARN("WebServer: no config file found for strategy {} — the running strategy "
                 "has the new parameters, but they will not survive a restart", strategy_id);
    } catch (const std::exception& e) {
        LOG_ERROR("WebServer: Failed to save config for strategy {}: {}", strategy_id, e.what());
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

void WebServer::handle_get_orders(uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
    // Use WebServer-side open_orders_ (accessed only from uWS thread — no data race)
    json response = json::array();
    for (const auto& [cid, t] : open_orders_) {
        response.push_back({
            {"strategy_id",      t.strategy_id},
            {"order_id",         t.order_id},
            {"client_order_id",  t.client_order_id},
            {"instrument",       t.instrument},
            {"side",             t.side},
            {"type",             t.type},
            {"price",            t.price},
            {"qty",              t.qty},
            {"filled_qty",       t.filled_qty},
            {"remaining_qty",    t.remaining_qty},
            {"status",           t.status},
            {"created_time",     t.created_time},
            {"last_update_time", t.last_update_time}
        });
    }
    send_json_response(res, response.dump());
}

void WebServer::handle_delete_order(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                     const std::string& strategy_id, const std::string& order_id,
                                     std::string_view body) {
    try {
        auto data = json::parse(body);
        if (!data.contains("client_order_id")) {
            send_error_response(res, "Missing required field: client_order_id", 400);
            return;
        }
        std::string client_order_id = data["client_order_id"];

        auto strategy = strategy_manager_.get_strategy(strategy_id);
        if (!strategy) {
            send_error_response(res, "Strategy not found", 404);
            return;
        }

        reinterpret_pointer_cast<StrategyBase>(strategy)->cancel_order(order_id, client_order_id);

        // Optimistically remove from WebServer-side tracking; ORDER_CANCEL event will confirm
        auto it = open_orders_.find(client_order_id);
        if (it != open_orders_.end()) {
            if (!it->second.order_id.empty()) oid_to_cid_.erase(it->second.order_id);
            open_orders_.erase(it);
        }

        // Notify WebSocket clients immediately so the UI updates without waiting for the event
        {
            json msg;
            msg["type"]             = "order_update";
            msg["event"]            = "cancel";
            msg["order_id"]         = order_id;
            msg["client_order_id"]  = client_order_id;
            broadcast_order_update(msg.dump());
        }

        LOG_INFO("WebServer: Cancel order {} for strategy {}", order_id, strategy_id);
        send_json_response(res, R"({"success":true,"message":"Cancel request submitted"})", 202);
    } catch (const json::exception& e) {
        send_error_response(res, std::string("Invalid JSON: ") + e.what(), 400);
    } catch (const std::exception& e) {
        send_error_response(res, e.what(), 500);
    }
}

void WebServer::handle_put_order(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                  const std::string& strategy_id, const std::string& order_id,
                                  std::string_view body) {
    try {
        auto data = json::parse(body);
        if (!data.contains("client_order_id")) {
            send_error_response(res, "Missing required field: client_order_id", 400);
            return;
        }

        std::string client_order_id = data["client_order_id"];
        double new_price    = data.value("new_price",    std::numeric_limits<double>::quiet_NaN());
        double new_quantity = data.value("new_quantity", std::numeric_limits<double>::quiet_NaN());

        if (std::isnan(new_price) && std::isnan(new_quantity)) {
            send_error_response(res, "At least one of new_price or new_quantity is required", 400);
            return;
        }

        auto strategy = strategy_manager_.get_strategy(strategy_id);
        if (!strategy) {
            send_error_response(res, "Strategy not found", 404);
            return;
        }

        auto* base = reinterpret_pointer_cast<StrategyBase>(strategy).get();
        const Order* existing = base->get_order(client_order_id);
        if (!existing) {
            send_error_response(res, "Order not found", 404);
            return;
        }

        price_t final_price = std::isnan(new_price)    ? existing->price : to_price_t(new_price);
        qty_t   final_qty   = std::isnan(new_quantity)  ? existing->qty : to_qty_t(new_quantity);

        base->modify_order(client_order_id, final_price, final_qty);

        LOG_INFO("WebServer: Modify order {} for strategy {} price={} qty={}",
                 order_id, strategy_id, from_price_t(final_price), from_qty_t(final_qty));
        send_json_response(res, R"({"success":true,"message":"Modify request submitted"})", 202);
    } catch (const json::exception& e) {
        send_error_response(res, std::string("Invalid JSON: ") + e.what(), 400);
    } catch (const std::exception& e) {
        send_error_response(res, e.what(), 500);
    }
}

void WebServer::handle_delete_all_orders(uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
    try {
        const auto& strategies = strategy_manager_.get_all_strategies();
        int strategy_count = 0;

        for (const auto& strategy : strategies) {
            reinterpret_pointer_cast<StrategyBase>(strategy)->cancel_all_orders();
            ++strategy_count;
        }

        // Clear WebServer-side tracking optimistically
        open_orders_.clear();
        oid_to_cid_.clear();

        LOG_INFO("WebServer: Cancel all orders across {} strategies", strategy_count);
        json response = {
            {"success", true},
            {"message", "Cancel all request submitted"},
            {"strategies_affected", strategy_count}
        };
        send_json_response(res, response.dump(), 202);
    } catch (const std::exception& e) {
        send_error_response(res, e.what(), 500);
    }
}

void WebServer::process_events() {
    if (!running_.load(std::memory_order_acquire)) {
        return;  // Stop polling if server is shutting down
    }

    for (int i = 0; i < 100; ++i) {
        auto record = event_mux_.read(event_cursor_);
        if (!record) {
            // no event available
            break;
        }

        const Event* event = reinterpret_cast<const Event*>(record.data);
        switch(event->type) {
            case EventType::LEVEL2_UPDATE: {
                const auto* level2_event = reinterpret_cast<const Level2UpdateEvent*>(event);
                broadcast_level2_update(*level2_event);
                break;
            }
            case EventType::TOB:
                // A mark moved. Many arrive per pass on a busy book and a browser needs
                // none of them individually, so they only raise a flag.
                pnl_changed_ = true;
                break;
            case EventType::ORDER_ACK: {
                const auto* ack = reinterpret_cast<const OrderAckEvent*>(event);
                handle_order_ack(*ack);
                break;
            }
            case EventType::ORDER_FILL: {
                const auto* fill = reinterpret_cast<const OrderFillEvent*>(event);
                // A broker's replay of its journal at startup is the run before's: the
                // positions it may still book reach the dashboard with the PnL.
                if (!fill->replayed) {
                    handle_order_fill(*fill);
                }
                break;
            }
            case EventType::ORDER_CANCEL: {
                const auto* cancel = reinterpret_cast<const OrderCancelEvent*>(event);
                handle_order_cancel(*cancel);
                break;
            }
            case EventType::ORDER_REJECT: {
                const auto* reject = reinterpret_cast<const OrderRejectEvent*>(event);
                handle_order_reject(*reject);
                break;
            }

        }
    }

    if (pnl_changed_) {
        const uint64_t now_ns = get_timestamp_ns();
        if (now_ns - last_pnl_broadcast_ns_ >= PNL_BROADCAST_INTERVAL_NS) {
            pnl_changed_ = false;
            last_pnl_broadcast_ns_ = now_ns;
            broadcast_pnl_update();
        }
    }

    check_and_broadcast_broker_status();

    uWS::Loop::get()->defer([this]() { process_events(); });
}

void WebServer::check_and_broadcast_broker_status() {
    auto& reg = BrokerStatusRegistry::instance();
    const auto& all = reg.all();
    bool changed = false;

    for (size_t i = 1; i < all.size(); ++i) {
        const auto& s = all[i];
        if (s.name[0] == '\0') continue;
        auto& snap = last_broker_status_[i];
        bool me = s.md_enabled.load(std::memory_order_acquire);
        bool mc = s.md_connected.load(std::memory_order_acquire);
        bool oe = s.order_enabled.load(std::memory_order_acquire);
        bool oc = s.order_connected.load(std::memory_order_acquire);
        if (me != snap.md_enabled || mc != snap.md_connected ||
            oe != snap.order_enabled || oc != snap.order_connected) {
            snap = {me, mc, oe, oc};
            changed = true;
        }
    }

    if (!changed || all_ws_clients_.empty()) return;

    json brokers = json::array();
    for (size_t i = 1; i < all.size(); ++i) {
        const auto& s = all[i];
        if (s.name[0] == '\0') continue;
        brokers.push_back({
            {"id", i},
            {"name", s.name},
            {"md", {
                {"enabled", last_broker_status_[i].md_enabled},
                {"connected", last_broker_status_[i].md_connected}
            }},
            {"order", {
                {"enabled", last_broker_status_[i].order_enabled},
                {"connected", last_broker_status_[i].order_connected}
            }}
        });
    }

    const std::string msg = json{{"type", "broker_status"}, {"brokers", brokers}}.dump();
    for (auto* ws : all_ws_clients_) {
        ws->send(msg, uWS::OpCode::TEXT);
    }
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
    }/*  else {
        static uint64_t unsubscribed_count = 0;
        if (++unsubscribed_count % 100 == 0) {  // Log every 100th to avoid spam
            LOG_DEBUG("WebServer: No subscribers for {} orderbook (total unsubscribed events: {})",
                      instrument, unsubscribed_count);
        }
    } */
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

void WebServer::handle_get_ladder_config(uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
    add_cors_headers(res);
    try {
        send_json_response(res, ladder_config_.dump());
    } catch (const std::exception& e) {
        send_error_response(res, e.what(), 500);
    }
}

void WebServer::handle_put_ladder_config(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                                          const std::string& body) {
    add_cors_headers(res);

    try {
        auto new_config = json::parse(body);

        // Validate config structure
        if (!new_config.contains("default_qty")) {
            send_error_response(res, "Invalid ladder config format: missing required fields", 400);
            return;
        }

        // Update in-memory config
        ladder_config_ = new_config;

        LOG_INFO("WebServer: Updated ladder config - default_qty: {}",
                 ladder_config_["default_qty"].get<double>());

        json response = {
            {"status", "ok"},
            {"message", "Ladder config updated successfully"}
        };
        send_json_response(res, response.dump());

    } catch (const json::exception& e) {
        send_error_response(res, std::string("Invalid JSON: ") + e.what(), 400);
    } catch (const std::exception& e) {
        send_error_response(res, e.what(), 500);
    }
}

void WebServer::handle_order_ack(const OrderAckEvent& ack) {
    std::string cid(ack.client_order_id);
    std::string oid(ack.order_id);

    // Update WebServer-side tracking.
    // Only use ACKs with a valid client_order_id to avoid the empty-cid
    // second ACK from the Coinbase WebSocket callback overwriting our map.
    if (!cid.empty()) {
        auto it = open_orders_.find(cid);
        if (it != open_orders_.end()) {
            // Update existing entry — covers both initial new-order ACK and modify-ack
            if (!oid.empty()) {
                it->second.order_id = oid;
                oid_to_cid_[oid]    = cid;
            }
            it->second.status           = "ACKNOWLEDGED";
            it->second.last_update_time = ack.timestamp_ns;
            if (ack.price != NULL_PRICE && ack.price != 0) {
                it->second.price = from_price_t(ack.price);
            }
            if (ack.quantity != 0) {
                it->second.qty           = from_qty_t(ack.quantity);
                it->second.remaining_qty = from_qty_t(ack.quantity);
            }
        } else if (!oid.empty()) {
            // Algo-submitted order: not yet in open_orders_ — create entry now.
            // We can derive strategy from client_order_id prefix (name_<timestamp>)
            std::string strategy_id;
            auto sep = cid.rfind('_');
            if (sep != std::string::npos) strategy_id = cid.substr(0, sep);

            TrackedOrder t;
            t.strategy_id      = strategy_id;
            t.client_order_id  = cid;
            t.order_id         = oid;
            t.instrument       = std::string(ack.instrument);
            t.side             = (ack.side == Side::BUY) ? "buy" : "sell";
            t.type             = static_cast<int>(ack.order_type);
            t.price            = (ack.price != NULL_PRICE) ? from_price_t(ack.price) : 0.0;
            t.qty              = from_qty_t(ack.quantity);
            t.remaining_qty    = from_qty_t(ack.quantity);
            t.status           = "ACKNOWLEDGED";
            t.created_time     = ack.timestamp_ns;
            t.last_update_time = ack.timestamp_ns;
            open_orders_[cid]  = std::move(t);
            oid_to_cid_[oid]   = cid;
        }
    }

    // Only broadcast ACKs with a valid client_order_id.
    // The Coinbase WebSocket sends a second ACK (OPEN status) with empty client_order_id
    // and price=0 — that's exchange noise and should not be forwarded to the UI.
    if (!cid.empty()) {
        json msg;
        msg["type"]       = "order_update";
        msg["event"]      = "ack";
        msg["order_id"]   = oid;
        msg["instrument"] = std::string(ack.instrument);
        msg["side"]       = (ack.side == Side::BUY) ? "buy" : "sell";
        msg["price"]      = from_price_t(ack.price);
        msg["qty"]        = from_qty_t(ack.quantity);
        msg["status"]     = std::string(to_string(ack.status));
        broadcast_order_update(msg.dump());
    }
}

void WebServer::handle_order_fill(const OrderFillEvent& fill) {
    std::string oid(fill.order_id);

    if (fill.scope == FillScope::LEG) {
        // One leg of a multi-leg order's fill: a position in the leg, and nothing new about
        // the order, which its own fill already brought up to date.
        json msg;
        msg["type"]       = "order_update";
        msg["event"]      = "leg_fill";
        msg["order_id"]   = oid;
        msg["instrument"] = std::string(fill.instrument);
        msg["side"]       = (fill.side == Side::BUY) ? "buy" : "sell";
        msg["fill_price"] = from_price_t(fill.fill_price);
        msg["fill_qty"]   = from_qty_t(fill.fill_quantity);
        broadcast_order_update(msg.dump());
        return;
    }

    // Capture tracked order fields before potential erasure
    std::string instrument, side, fill_status;
    double price = 0.0, qty = 0.0;

    auto cid_it = oid_to_cid_.find(oid);
    if (cid_it != oid_to_cid_.end()) {
        auto it = open_orders_.find(cid_it->second);
        if (it != open_orders_.end()) {
            instrument = it->second.instrument;
            side       = it->second.side;
            price      = it->second.price;
            qty        = it->second.qty;

            double rem = from_qty_t(fill.remaining_quantity);
            it->second.remaining_qty    = rem;
            it->second.filled_qty       = it->second.qty - rem;
            it->second.last_update_time = fill.timestamp_ns;
            if (rem <= 0.0) {
                fill_status = "FILLED";
                open_orders_.erase(it);
                oid_to_cid_.erase(cid_it);
            } else {
                fill_status = "PARTIALLY_FILLED";
                it->second.status = fill_status;
            }
        }
    }

    json msg;
    msg["type"]          = "order_update";
    msg["event"]         = "fill";
    msg["order_id"]      = oid;
    msg["instrument"]    = instrument;
    msg["side"]          = side;
    msg["price"]         = price;
    msg["qty"]           = qty;
    msg["status"]        = fill_status;
    msg["fill_price"]    = from_price_t(fill.fill_price);
    msg["fill_qty"]      = from_qty_t(fill.fill_quantity);
    msg["remaining_qty"] = from_qty_t(fill.remaining_quantity);
    broadcast_order_update(msg.dump());
}

void WebServer::handle_order_cancel(const OrderCancelEvent& cancel) {
    std::string oid(cancel.order_id);

    // Capture tracked order fields before erasure
    std::string instrument, side;
    double price = 0.0, qty = 0.0;

    auto cid_it = oid_to_cid_.find(oid);
    if (cid_it != oid_to_cid_.end()) {
        auto it = open_orders_.find(cid_it->second);
        if (it != open_orders_.end()) {
            instrument = it->second.instrument;
            side       = it->second.side;
            price      = it->second.price;
            qty        = it->second.qty;
            open_orders_.erase(it);
        }
        oid_to_cid_.erase(cid_it);
    }

    json msg;
    msg["type"]       = "order_update";
    msg["event"]      = "cancel";
    msg["order_id"]   = oid;
    msg["instrument"] = instrument;
    msg["side"]       = side;
    msg["price"]      = price;
    msg["qty"]        = qty;
    broadcast_order_update(msg.dump());
}

void WebServer::handle_order_reject(const OrderRejectEvent& reject) {
    std::string cid(reject.client_order_id);

    // Capture tracked order fields before erasure
    std::string instrument, side, order_id;
    double price = 0.0, qty = 0.0;

    if (!cid.empty()) {
        auto it = open_orders_.find(cid);
        if (it != open_orders_.end()) {
            instrument = it->second.instrument;
            side       = it->second.side;
            price      = it->second.price;
            qty        = it->second.qty;
            order_id   = it->second.order_id;
            if (!order_id.empty()) oid_to_cid_.erase(order_id);
            open_orders_.erase(it);
        }
    }

    json msg;
    msg["type"]       = "order_update";
    msg["event"]      = "reject";
    msg["order_id"]   = order_id;
    msg["instrument"] = instrument;
    msg["side"]       = side;
    msg["price"]      = price;
    msg["qty"]        = qty;
    msg["reason"]     = std::string(reject.reason);
    broadcast_order_update(msg.dump());
}

void WebServer::broadcast_order_update(const std::string& message) {
    for (auto* ws : all_ws_clients_) {
        ws->send(message, uWS::OpCode::TEXT);
    }
}

void WebServer::broadcast_pnl_update() {
    if (all_ws_clients_.empty()) return;

    const uint64_t now_ns = get_timestamp_ns();
    auto all_pnl = pnl_calculator_.calculate_all_pnl(now_ns);

    json pnl_obj = json::object();
    for (const auto& [strategy_id, pnl] : all_pnl) {
        pnl_obj[strategy_id] = {
            {"realized_pnl", pnl.get_realized()},
            {"unrealized_pnl", pnl.get_unrealized()},
            {"total_fees", pnl.get_fees()},
            {"total_pnl", pnl.get_total()}
        };
    }

    const std::string message = json{{"type", "pnl_update"}, {"pnl", pnl_obj}}.dump();
    for (auto* ws : all_ws_clients_) {
        ws->send(message, uWS::OpCode::TEXT);
    }
}

} // namespace slick::trader
