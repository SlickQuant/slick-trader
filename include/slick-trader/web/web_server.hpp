#pragma once

#include <slick/queue.hpp>
#include <uwebsockets/App.h>
#include <slick-trader/events/event.hpp>
#include <slick-trader/events/level2_update_event.hpp>
#include <slick-trader/web/fill_history.hpp>
#include <slick-trader/web/level2_book.hpp>
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
        // How long an instrument of MANUAL's stays held once no ladder shows it, so a page
        // reloaded or switched back keeps its stream. web_server_ladder_release_ms.
        uint64_t ladder_release_ns{5'000'000'000};
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

    // Start web server thread. start() and stop() are for one controlling thread.
    void start();

    // Stop web server and join its thread
    void stop();

    // Check if running
    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

    // Get current port
    uint16_t get_port() const { return config_.port; }

private:
    void run();
    // Closes the listen socket and every connection so the event loop can exit.
    // Event loop thread only. stop() defers it there.
    void close_connections();

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
    void handle_get_fills(uWS::HttpResponse<false>* res, uWS::HttpRequest* req);
    void handle_delete_order(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                             const std::string& strategy_id, const std::string& order_id,
                             std::string_view body);
    void handle_put_order(uWS::HttpResponse<false>* res, uWS::HttpRequest* req,
                          const std::string& strategy_id, const std::string& order_id,
                          std::string_view body);
    void handle_delete_all_orders(uWS::HttpResponse<false>* res, uWS::HttpRequest* req);

    // Hand a parameter update to the StrategyManager worker over the event bus.
    // Returns false when the update could not be published at all.
    bool publish_strategy_config(const std::string& strategy_id, const nlohmann::json& params);

    // Persist updated strategy parameters to whichever file the strategy loader reads
    void save_strategy_parameters(const std::string& strategy_id, const nlohmann::json& params);

    // Writes `parameters` into <configs_folder>/*.config.json for the file whose "name"
    // is strategy_id. Returns false when no such file exists.
    bool save_to_strategy_config_file(const std::string& configs_folder,
                                      const std::string& strategy_id,
                                      const nlohmann::json& params);

    // WebSocket handlers
    void handle_ws_open(uWS::WebSocket<false, true, void*>* ws);
    void handle_ws_message(uWS::WebSocket<false, true, void*>* ws, std::string_view message);
    void handle_ws_close(uWS::WebSocket<false, true, void*>* ws);

    using Socket = uWS::WebSocket<false, true, void*>;
    // Sends `message` as text. One uWS drops, its client too far behind, leaves that client's
    // ladder or lists short of it for good: close_lagging() closes it, and it reconnects whole.
    // Not uWS's closeOnBackpressureLimit: that only shuts the read side, so the close waits on
    // the client sending again, and what is sent meanwhile reaches it past the gap.
    void send_text(Socket* ws, std::string_view message);
    void close_lagging();

    // What uWS holds for a client before it drops a send. Over the largest message: the list a
    // dashboard gets on connect runs to megabytes with MANUAL on "*", and the 64 KB default
    // dropped the next message to any client still reading it, at every reconnect too.
    static constexpr unsigned int WS_MAX_BACKPRESSURE = 16u << 20;

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
    void broadcast_order_update(const std::string& message);
    // Keeps a fill in fills_ and sends it to every client.
    void broadcast_fill(json& message);

    // The ladder's instruments. index_listings() takes what MANUAL listed since it last looked
    // into listings_, and what is new into listings_added_; send_instruments() writes them all
    // to a client that asked, send_instruments_added() what is new to every one that did.
    void index_listings();
    void send_instruments(Socket* ws);
    void send_instruments_added(uint64_t now_ns);

    // A ladder shows `instrument` and nobody else did (watched), or the last one let go
    // (unwatched). One of MANUAL's is held at its broker while watched, and released
    // ladder_release_ns after it is not, by release_holds(). False when a hold was needed and
    // could not be asked for.
    bool watched(std::string_view instrument);
    void unwatched(std::string_view instrument);
    void release_holds(uint64_t now_ns);
    bool publish_ladder_subscription(BrokerId broker, std::string_view instrument, bool subscribe);

    // PnL broadcasting
    void broadcast_pnl_update();

    // Broker status broadcasting
    void check_and_broadcast_broker_status();

    StrategyManager& strategy_manager_;
    PositionTracker& position_tracker_;
    PnLCalculator& pnl_calculator_;
    RiskManager& risk_manager_;

    Config config_;
    std::atomic_bool running_{false};
    std::thread worker_thread_;
    // The running app, for close_connections(). Touched only on the worker's event loop thread.
    uWS::App* app_{nullptr};
    // The worker's event loop, for stop() to hand the shutdown to. uWS may only be used from
    // the thread running the loop. Loop::defer is the one call that is safe from another.
    // The loop is thread local and is freed when the worker thread exits, so the pointer can
    // only be used while loop_mutex_ is held. stop() holds the lock across defer(), and the
    // worker takes it to clear loop_ before it exits, so the loop can't be freed in the middle
    // of a defer().
    std::mutex loop_mutex_;
    uWS::Loop* loop_{nullptr};

    slick::stream_buffer_multiplexer& event_mux_;
    uint64_t event_cursor_{0};

    // PnL goes to the dashboard when a top of book moved, at most this often. Touched only
    // by the uWS event loop thread.
    static constexpr uint64_t PNL_BROADCAST_INTERVAL_NS = 250'000'000;
    bool pnl_changed_{false};
    uint64_t last_pnl_broadcast_ns_{0};

    // Every instrument a ladder can show: each strategy's, then each MANUAL lists, the first
    // broker named winning, with its tick, 0 when unknown. listings_indexed_ is how many of
    // MANUAL's are in. A dashboard asking gets them all, then what is new at most this often.
    struct Listing {
        BrokerId broker;
        price_t tick_size;
        // A strategy streams its book at `broker` (LEVEL2 or ORDER_BOOK), so it is never held for
        // a ladder. One a strategy only quotes or trades streams no book: it is held.
        bool book;
    };
    static constexpr uint64_t INSTRUMENTS_ADDED_INTERVAL_NS = 250'000'000;
    StringMap<Listing> listings_;
    uint32_t listings_indexed_{0};
    // Keys of listings_, which a map keeps in place, new or given a tick since last sent.
    std::vector<const std::string*> listings_added_;
    uint64_t last_instruments_sent_ns_{0};
    std::string instruments_message_;   // one buffer, its capacity kept from one to the next
    // The clients that asked for the list.
    std::unordered_set<uWS::WebSocket<false, true, void*>*> instrument_clients_;

    // MANUAL's instruments ladders hold: release_at_ns is 0 while one shows it, else when it
    // goes. `releasing_` counts those with a deadline.
    struct LadderHold {
        BrokerId broker;
        uint64_t release_at_ns;
    };
    StringMap<LadderHold> ladder_holds_;
    size_t releasing_{0};

    // How long one process_events() pass may read the bus before the loop gets its sockets back.
    static constexpr uint64_t PROCESS_EVENTS_BUDGET_NS = 1'000'000;

    // Each level2 message is written here: one buffer, its capacity kept from one to the next.
    std::string level2_message_;

    // Producer side of the same bus: strategy parameter updates are published here from the
    // uWS event loop thread and applied by the StrategyManager worker, so a strategy's
    // on_config() runs on the thread that owns its state. Registered in initialize(), which
    // is before any producer or consumer thread starts — add_producer() is setup-only.
    std::shared_ptr<slick::stream_buffer_multiplexer::producer_buffer> config_producer_buffer_;

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

    // Clients a send was dropped for, closed at the end of the process_events() pass.
    std::unordered_set<Socket*> lagging_;

    // The level 2 book of every instrument streamed, any strategy's or MANUAL's, and the
    // browsers watching each.
    Level2Streams<Socket> level2_streams_;

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
    // The last fills sent, for /api/fills.
    static constexpr size_t FILL_HISTORY_CAPACITY = 1000;
    FillHistory fills_{FILL_HISTORY_CAPACITY};
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
