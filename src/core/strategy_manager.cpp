#include <slick/logger.hpp>
#include <slick-trader/core/strategy_manager.hpp>
#include <slick-trader/core/strategy_config.hpp>
#include <slick-trader/strategy/manual_strategy.hpp>
#include <slick-trader/brokers/common/md_event_writer.hpp>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <ranges>

using namespace slick;
using namespace slick::trader;

StrategyManager::StrategyManager(
    const json& config,
    PositionTracker& position_tracker,
    PnLCalculator& pnl_calculator,
    RiskManager& risk_manager,
    slick::stream_buffer_multiplexer& event_mux,
    MdRequestQueues& md_request_queues,
    slick::queue<uint8_t>& order_request_queue
)
    : reader_stall_timeout_(config.value("md_request_stall_timeout_ms", 5000))
    , position_tracker_(position_tracker)
    , pnl_calculator_(pnl_calculator)
    , risk_manager_(risk_manager)
    , event_mux_(event_mux)
    , md_request_queues_(md_request_queues)
    , order_request_queue_(order_request_queue)
    , tob_producer_buffer_(
        event_mux.add_producer(
            event_mux.producer_count(),
            config.value("TOB_queue_size", 1048576),
            config.value("TOB_control_size", 65536),
            config.contains("TOB_shm_name") ? config["TOB_shm_name"].get_ref<const std::string&>().c_str() : nullptr
        ))
{
    // Each queue's write index. The connectors were made before this manager and nothing else
    // publishes requests, so it is where each starts reading.
    for (size_t i = 0; i < BROKER_COUNT; ++i) {
        if (const auto* queue = md_request_queues_.find(static_cast<BrokerId>(i))) {
            request_baseline_[i] = queue->initial_reading_index();
        }
    }
}

// ========================================================================
// Strategy Loading (Dynamic Discovery)
// ========================================================================
void StrategyManager::load_strategies(const json& config) {

    auto strategy_plugins_folder = config.value("strategy_plugins_folder", "strategies");
    auto strategy_configs_folder = config.value("strategy_configs_folder", strategy_plugins_folder);

    // Check if folder exists
    if (!std::filesystem::exists(strategy_plugins_folder)) {
        LOG_WARN("⚠ Strategy plugins folder does not exist: {}", strategy_plugins_folder);
        LOG_WARN("⚠ Creating folder...");
        std::filesystem::create_directories(strategy_plugins_folder);
    }
    
    LOG_INFO("Loading strategies from strategy plugins folder: {} ...", strategy_plugins_folder);

    std::unordered_map<std::string, StrategyConfig> strategy_configs;

    // look for strategy-specific config files in strategy configs folder
    // Format: <strategy_name>.config.json
    if (std::filesystem::exists(strategy_configs_folder)) {
        try {
            for (const auto& entry : std::filesystem::directory_iterator(strategy_configs_folder)) {
                if (entry.is_regular_file()) {
                    const auto& path = entry.path();
                    std::string filename = path.filename().string();

                    // Check if it's a strategy config file (*.config.json)
                    if (filename.ends_with(".config.json")) {
                        std::ifstream config_file(path);
                        if (config_file.is_open()) {
                            try {
                                json strategy_json = json::parse(config_file);
                                auto strategy_config = StrategyConfig::from_json(strategy_json);

                                if (!strategy_config.name.empty()) {
                                    // Strategy-specific file takes precedence over main config
                                    strategy_configs[strategy_config.name] = strategy_config;
                                    LOG_INFO("Loaded config for strategy '{}' from {}", strategy_config.name, filename);
                                }
                            } catch (const json::exception& e) {
                                LOG_WARN("Failed to parse strategy config file {}: {}",
                                         filename, e.what());
                            }
                        }
                    }
                }
            }
        } catch (const std::filesystem::filesystem_error& e) {
            LOG_WARN("Failed to scan for strategy config files: {}", e.what());
        }
    }
    else {
        LOG_WARN("Strategy configs folder does not exist: {}", strategy_configs_folder);
    }

    const std::string plugin_extension(PLUGIN_EXTENSION);

    // Scan folder for all plugin files
    std::vector<std::string> plugin_paths;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(strategy_plugins_folder)) {
            if (entry.is_regular_file()) {
                const auto& path = entry.path();
                if (path.extension() == plugin_extension) {
                    plugin_paths.push_back(path.string());
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        LOG_ERROR("Failed to scan strategy folder: {}", e.what());
    }

    if (plugin_paths.empty()) {
        LOG_WARN("⚠ No strategy plugins found in: {}", strategy_configs_folder);
        LOG_WARN("⚠ Looking for files with extension: {}", plugin_extension);
    } else {
        LOG_INFO("Found {} plugin file(s) in {}", plugin_paths.size(), strategy_configs_folder);

        // Sort for deterministic loading order
        std::sort(plugin_paths.begin(), plugin_paths.end());

        // Load each plugin
        for (const auto& plugin_path : plugin_paths) {
            auto strategy = loader_.load(plugin_path);
            if (strategy) {
                std::string strategy_name = strategy->name();

                // Check if strategy is enabled in config (default: enabled)
                bool enabled = true;
                auto iter = strategy_configs.find(strategy_name);
                bool has_config = iter != strategy_configs.end();
                if (has_config) {
                    enabled = iter->second.enabled;
                } else {
                    LOG_WARN("No config find for strategy {}", strategy_name);
                }

                if (!enabled) {
                    LOG_INFO("⊘ Strategy {} is disabled in config, skipping", strategy_name);
                    continue;
                }

                // Initialized whatever its config holds: initialize() hands it the risk manager and
                // order queue it trades through, and a config with no "parameters" left them null.
                static const json no_parameters = json::object();
                const json& params = has_config ? iter->second.parameters : no_parameters;
                strategy->initialize(params, position_tracker_, pnl_calculator_, risk_manager_, order_request_queue_);
                LOG_INFO("  Passed {} parameter(s) to strategy {}", params.size(), strategy_name);

                add_strategy(strategy);
                LOG_INFO("✓ Loaded strategy: {} from {}", strategy_name,
                         std::filesystem::path(plugin_path).filename().string());

                // Apply risk limits from config or use defaults
                RiskLimits limits;
                if (strategy_configs.count(strategy_name)) {
                    limits = strategy_configs[strategy_name].risk_limits;
                    LOG_INFO("  Using custom risk limits from config");
                } else {
                    // Default risk limits
                    limits.max_position_size = to_qty_t(1.0);       // Max 1 BTC/ETH
                    limits.max_order_value = to_price_t(50000.0);   // Max $50k per order
                    limits.max_orders_per_second = 10;              // Max 10 orders/sec
                    LOG_INFO("  Using default risk limits (no config found)");
                }

                risk_manager_.set_limits(strategy_name, limits);
            } else {
                LOG_WARN("⚠ Failed to load plugin: {}", std::filesystem::path(plugin_path).filename().string());
                LOG_WARN("  Error: {}", loader_.get_last_error());
            }
        }
    }

    // Always register ManualStrategy for orders placed through the Web UI, configured by a
    // <name>.config.json naming it if there is one: its "instruments" narrow what ladders list.
    {
        auto manual = std::make_shared<ManualStrategy>();
        const auto config = strategy_configs.find(ManualStrategy::ID);
        const bool has_config = config != strategy_configs.end();
        manual->initialize(has_config ? config->second.parameters : json::object(), position_tracker_,
                           pnl_calculator_, risk_manager_, order_request_queue_);
        risk_manager_.set_limits(ManualStrategy::ID, has_config ? config->second.risk_limits : RiskLimits{});
        add_strategy(manual);
        LOG_INFO("✓ Registered ManualStrategy for web UI orders");
    }

    if (strategies_.size() == 1) {
        LOG_WARN("⚠ No strategy plugins loaded - system will receive market data but won't auto-trade");
        LOG_WARN("⚠ Place strategy plugins ({}) in: {}", plugin_extension, strategy_plugins_folder);
    } else {
        LOG_INFO("✓ Successfully loaded {} strategy/strategies", strategies_.size() - 1);
    }
}

void StrategyManager::add_strategy(std::shared_ptr<IStrategy> strategy) {
    strategies_.push_back(strategy);
    strategy_map_[strategy->name()] = strategy;
    if (auto manual = std::dynamic_pointer_cast<ManualStrategy>(strategy)) {
        manual_ = std::move(manual);
    }

    // Track which MD brokers this strategy depends on (for auto-pause on MD disconnect)
    for (const auto& sub : strategy->get_subscriptions()) {
        strategy_md_brokers_[strategy->name()].insert(sub.broker_id);
    }

    // If the engine is already running, start the strategy immediately (hot-loading)
    // Otherwise, it will be started when engine.start() is called
    if (is_running()) [[unlikely]] {
        strategy->start();
        LOG_INFO("StrategyManager: Added and started strategy: {}", strategy->name());
    } else {
        LOG_INFO("StrategyManager: Added strategy '{}'", strategy->name());
    }
}

bool StrategyManager::start_strategy(const std::string& strategy_id) {
    auto it = strategy_map_.find(strategy_id);
    if (it == strategy_map_.end()) return false;
    auto_paused_strategies_.erase(strategy_id);
    auto_pause_reasons_.erase(strategy_id);
    it->second->start();
    return true;
}

bool StrategyManager::stop_strategy(const std::string& strategy_id) {
    auto it = strategy_map_.find(strategy_id);
    if (it == strategy_map_.end()) return false;
    auto_paused_strategies_.erase(strategy_id);
    auto_pause_reasons_.erase(strategy_id);
    it->second->stop();
    return true;
}

void StrategyManager::start() {
    if (running_.load(std::memory_order_relaxed)) [[unlikely]] {
        LOG_WARN("StrategyManager: Already running");
        return;
    }

    LOG_INFO("StrategyManager: Starting");

    // Requests wait only on the connectors enabled in this module's BrokerStatusRegistry: say
    // which, since one that enabled itself in another module's is not waited on.
    std::string readers;
    for (size_t i = 1; i < BROKER_COUNT; ++i) {
        if (BrokerStatusRegistry::instance().is_md_enabled(static_cast<BrokerId>(i))) {
            readers += readers.empty() ? "" : ", ";
            readers += to_string(static_cast<BrokerId>(i));
        }
    }
    LOG_INFO("StrategyManager: market data requests wait on the connectors of: {}", readers.empty() ? "none" : readers);

    // Start all strategies
    for (auto& strategy : strategies_) {
        strategy->start();

        // subscribe MD update. MANUAL's are subscribed as ladders hold them.
        if (strategy == manual_) {
            continue;
        }
        for (const auto& subscription : strategy->get_subscriptions()) {
            subscribe(subscription);
        }
    }

    running_.store(true, std::memory_order_release);
    worker_thread_ = std::thread(&StrategyManager::run, this);

    LOG_INFO("StrategyManager: Started");
}

void StrategyManager::stop() {
    if (!running_.load(std::memory_order_relaxed)) {
        return;
    }

    LOG_INFO("StrategyManager: Stopping");

    running_.store(false, std::memory_order_release);

    // Join the worker before unsubscribing: it subscribes at runtime (a ladder holding an
    // instrument of MANUAL's), so the subscription map is its state until it stops.
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    // Stop all strategies
    for (auto& strategy : strategies_) {
        auto subscriptions = strategy->get_subscriptions();
        for (const auto &subscription : subscriptions) {
            unsubscribe(subscription);
        }
        if (strategy == manual_) {
            manual_->release_all();   // a ladder holds them again after a restart
        }

        strategy->stop();
    }
    drain_requests();

    LOG_INFO("StrategyManager: Stopped");
}

// One instrument's slot in an MDSubscription.
static constexpr size_t REQUEST_SLOT_SIZE = sizeof(MDSubscription::instruments[0]);

size_t StrategyManager::max_instruments_per_request(const slick::queue<uint8_t>& queue) {
    // A quarter of the queue: send_requests() asks for twice a request's size, and that has
    // to fit with room left for the requests still unread. A queue too small for even one
    // instrument that way gets requests of one, which go out once its connector has caught up.
    const size_t budget = queue.size() / 4;
    constexpr size_t header = sizeof(MDSubscription);
    return budget > header + REQUEST_SLOT_SIZE ? (budget - header) / REQUEST_SLOT_SIZE : 1;
}

void StrategyManager::publish_subscription(EventType type, const SubscriptionRequest& sub,
                                           const std::vector<std::string>& instruments) {
    auto* queue = md_request_queues_.find(sub.broker_id);
    if (queue == nullptr) {
        // Held all the same: no broker of this engine reads them.
        if (type == EventType::SUBSCRIBE) {
            LOG_WARN("StrategyManager: no {} broker was built, so {} instrument(s) are not subscribed there",
                     to_string(sub.broker_id), instruments.size());
        }
        return;
    }
    const auto broker = static_cast<size_t>(sub.broker_id);
    auto& outbox = request_outbox_[broker];
    for_each_part(instruments.size(), max_instruments_per_request(*queue), [&](size_t offset, size_t n) {
        const auto size = static_cast<uint32_t>(sizeof(MDSubscription) + n * REQUEST_SLOT_SIZE);
        const auto write = [&](uint8_t* record) {
            // Zeroed, so each name's slot is padded with zeros: the queue does not clear what
            // it hands out.
            std::memset(record, 0, size);
            auto& event = *new (record) MDSubscription();
            event.type = type;
            event.broker_id = sub.broker_id;
            event.subscription_type = sub.type;
            event.depth = sub.depth;
            event.num_instruments = static_cast<uint32_t>(n);
            for (size_t i = 0; i < n; ++i) {
                const auto& inst = instruments[offset + i];
                auto& slot = event.instruments[i];
                assert(inst.size() < sizeof(slot));
                std::memcpy(slot, inst.data(), std::min(inst.size(), sizeof(slot) - 1));
            }
        };
        if (outbox.empty() && has_room(broker, *queue, size)) {
            const uint64_t index = queue->reserve(size);
            write((*queue)[index]);
            queue->publish(index, size);
        } else {
            // Out from run(), or from stop()'s drain. start(), the worker and stop() never
            // run at once, so the outbox takes no lock.
            write(outbox.emplace_back(size).data());
            ++requests_queued_;
        }
    });
}

uint64_t StrategyManager::request_cursor(size_t i) const {
    const uint64_t cursor = BrokerStatusRegistry::instance().md_request_cursor(static_cast<BrokerId>(i));
    // None reported: it has read none of what this manager published.
    return cursor == BrokerStatus::NO_MD_REQUEST_CURSOR ? request_baseline_[i] : cursor;
}

bool StrategyManager::has_room(size_t i, const slick::queue<uint8_t>& queue, uint32_t size) const {
    // Enabled is taken for reading. A disabled connector that reads anyway, as Hyperliquid's
    // does, drops its requests, so nothing is lost by not waiting on it.
    if (!BrokerStatusRegistry::instance().is_md_enabled(static_cast<BrokerId>(i))) {
        return true;
    }
    // Where the request ends at worst: a record that would cross the end of the ring is written
    // from its start instead. Capped at the queue, which any request fits once the connector
    // has caught up.
    const uint64_t capacity = queue.size();
    const uint64_t end = queue.initial_reading_index() + std::min<uint64_t>(2ull * size, capacity);
    return end <= request_cursor(i) + capacity;
}

bool StrategyManager::send_requests(std::chrono::steady_clock::time_point now) {
    last_request_send_ = now;
    for (size_t i = 0; i < BROKER_COUNT; ++i) {
        auto& outbox = request_outbox_[i];
        if (outbox.empty()) {
            continue;
        }
        const auto broker = static_cast<BrokerId>(i);
        auto& queue = *md_request_queues_.find(broker);   // nothing is queued for a broker without one
        while (!outbox.empty()) {
            const auto& record = outbox.front();
            const auto size = static_cast<uint32_t>(record.size());
            if (!has_room(i, queue, size)) {
                break;
            }
            const uint64_t index = queue.reserve(size);
            std::memcpy(queue[index], record.data(), size);
            queue.publish(index, size);
            outbox.pop_front();
            --requests_queued_;
        }

        const uint64_t cursor = request_cursor(i);
        auto& wait = request_waits_[i];
        if (outbox.empty() || wait.since == std::chrono::steady_clock::time_point{} || cursor != wait.cursor) {
            if (wait.reported) {
                LOG_INFO("StrategyManager: {} market data connector is reading requests again", to_string(broker));
            }
            wait = outbox.empty() ? RequestWait{} : RequestWait{cursor, now, false};
        } else if (!wait.reported && now - wait.since >= reader_stall_timeout_) {
            wait.reported = true;
            LOG_WARN("StrategyManager: {} market data connector has read no request in {} ms, with {} of its "
                     "own waiting for it",
                     to_string(broker), reader_stall_timeout_.count(), outbox.size());
        }
    }
    return requests_queued_ == 0;
}

void StrategyManager::drain_requests() {
    // A connector still reading takes the rest. One that has read nothing in
    // md_request_stall_timeout_ms is given up on, here alone: its broker is stopping too.
    while (!send_requests(std::chrono::steady_clock::now())) {
        const bool any_reading = std::ranges::any_of(std::views::iota(size_t{0}, BROKER_COUNT), [&](size_t i) {
            return !request_outbox_[i].empty() && !request_waits_[i].reported;
        });
        if (!any_reading) {
            for (size_t i = 0; i < BROKER_COUNT; ++i) {
                auto& outbox = request_outbox_[i];
                if (!outbox.empty()) {
                    LOG_WARN("StrategyManager: {} market data request(s) to {} dropped at stop: its connector reads none",
                             outbox.size(), to_string(static_cast<BrokerId>(i)));
                    requests_queued_ -= outbox.size();
                    outbox.clear();
                    request_waits_[i] = {};   // or, started again, it would never be reported nor waited on
                }
            }
            return;
        }
        std::this_thread::yield();
    }
}

void StrategyManager::subscribe(const SubscriptionRequest& sub, bool book) {
    // Only what nobody holds yet goes to the broker; the rest just gains a holder.
    std::vector<std::string> instruments;
    for (auto& inst : sub.instruments) {
        if (++subscriptions_[SubscriptionKey{sub.broker_id, sub.type, inst}] == 1) {
            instruments.push_back(inst);
        }
    }
    if (!instruments.empty()) {
        publish_subscription(EventType::SUBSCRIBE, sub, instruments);
    }
    if (!book) {
        return;
    }

    // Every instrument of the request, not only those new to the broker: MANUAL may hold one
    // already, with no book kept. Books are made in start() alone, before the worker reads the
    // bus, so each takes in its broker's snapshot, which no SUBSCRIBE before start() has had
    // read yet. One made later, for one MANUAL streams, would be built from increments alone.
    assert(!is_running());
    if (streams_book(sub.type)) {
        subscribe_book_update(sub.instruments);
    } else if (sub.type == SubscriptionType::MBO) {
        // Built now, not on the first event the worker dispatches.
        for (const auto& inst : sub.instruments) {
            get_or_create_mbo_book(inst);
        }
    }
}

void StrategyManager::unsubscribe(const SubscriptionRequest& sub) {
    // Only what its last holder lets go of is unsubscribed at the broker.
    std::vector<std::string> instruments;
    for (auto& inst : sub.instruments) {
        auto iter = subscriptions_.find(SubscriptionKey{sub.broker_id, sub.type, inst});
        if (iter != subscriptions_.end() && --iter->second == 0) {
            subscriptions_.erase(iter);
            instruments.push_back(inst);
        }
    }

    if (!instruments.empty()) {
        publish_subscription(EventType::UNSUBSCRIBE, sub, instruments);
    }
}

void StrategyManager::run() {
    LOG_INFO("StrategyManager: Worker thread started");

    auto last_periodic_check = std::chrono::steady_clock::now();
    constexpr auto check_interval = std::chrono::seconds(1);

    while (running_.load(std::memory_order_relaxed)) {
        bool busy = false;
        if (auto record = event_mux_.read(event_cursor_)) {
            process_event(reinterpret_cast<const Event*>(record.data));
            busy = true;
        }

        auto now = std::chrono::steady_clock::now();
        // Retried at an interval, not on every event: a connector frees room a request at a time.
        if (requests_queued_ != 0 && now - last_request_send_ >= REQUEST_RETRY_INTERVAL) {
            send_requests(now);
        }
        if (now - last_periodic_check >= check_interval) {
            check_loss_limits();
            check_broker_status();
            last_periodic_check = now;
            busy = true;
        }

        if (!busy) {
            std::this_thread::yield();
        }
    }

    LOG_INFO("StrategyManager: Worker thread stopped");
}

// A parameter update published by the WebServer. Running it here is the point of routing it
// through the bus: on_config() lands on the worker thread, so a strategy can apply the change
// straight into state only this thread owns, with no latch and no lock.
//
// Deliberately unconditional on run state — a paused or halted strategy still has to see a
// config change, and applying one is not the same as trading on it.
void StrategyManager::dispatch_strategy_config(const StrategyConfigEvent& event) {
    const std::string strategy_id(event.strategy);

    auto strategy = get_strategy(strategy_id);
    if (!strategy) {
        LOG_WARN("StrategyManager: config update for unknown strategy '{}'", strategy_id);
        return;
    }
    auto* base = dynamic_cast<StrategyBase*>(strategy.get());
    if (base == nullptr) {
        LOG_WARN("StrategyManager: strategy '{}' does not support config updates", strategy_id);
        return;
    }

    // The WebServer validated the JSON before publishing, so a parse failure here means the
    // record was truncated or corrupted in transit — never apply half a config.
    json params;
    try {
        params = json::parse(event.config_text());
    } catch (const json::exception& e) {
        LOG_ERROR("StrategyManager: unparsable config for '{}': {}", strategy_id, e.what());
        return;
    }

    base->set_config(params);
    LOG_INFO("StrategyManager: Applied config update to strategy {}", strategy_id);

    // A list MANUAL is given now names instruments brokers defined already, and they may not
    // define them again.
    if (strategy == manual_) {
        for (size_t i = 0; i < BROKER_COUNT; ++i) {
            for (const auto& [instrument, tick_size] : defined_[i]) {
                list(static_cast<BrokerId>(i), instrument, tick_size);
            }
        }
    }
}

void StrategyManager::check_broker_status() {
    auto& reg = BrokerStatusRegistry::instance();

    for (auto& strategy : strategies_) {
        const std::string name(strategy->name());

        if (risk_manager_.is_halted(name)) continue;

        auto it = strategy_md_brokers_.find(name);
        if (it == strategy_md_brokers_.end()) continue;

        bool md_lost = false;
        std::string reason;
        for (BrokerId bid : it->second) {
            if (reg.is_md_enabled(bid) && !reg.is_md_connected(bid)) {
                md_lost = true;
                reason = std::string("MD disconnected for broker ") + reg.get(bid).name;
                break;
            }
        }

        const bool is_auto_paused = auto_paused_strategies_.count(name) > 0;
        auto* base = dynamic_cast<StrategyBase*>(strategy.get());

        if (md_lost && !is_auto_paused && base && base->is_running()) {
            LOG_WARN("StrategyManager: Auto-pausing '{}' - {}", name, reason);
            strategy->stop();
            auto_paused_strategies_.insert(name);
            auto_pause_reasons_[name] = reason;
        } else if (!md_lost && is_auto_paused) {
            LOG_INFO("StrategyManager: Auto-resuming '{}' after MD reconnect", name);
            strategy->start();
            auto_paused_strategies_.erase(name);
            auto_pause_reasons_.erase(name);
        }
    }
}

void StrategyManager::process_event(const Event* event) {
    switch (event->type) {
        case EventType::TRADE:
            dispatch_trade(*reinterpret_cast<const TradeEvent*>(event));
            break;
        case EventType::QUOTE:
            dispatch_quote(*reinterpret_cast<const QuoteEvent*>(event));
            break;
        case EventType::TOB:
            // This manager's own top of book, back off the bus: the strategies were given
            // it when it was published.
            break;
        case EventType::LEVEL2_UPDATE:
            dispatch_level2_updates(*reinterpret_cast<const Level2UpdateEvent*>(event));
            break;
        case EventType::MBO_UPDATE:
            dispatch_mbo_update(*reinterpret_cast<const MboUpdateEvent*>(event));
            break;
        case EventType::ORDER_BOOK:
            dispatch_order_book(*reinterpret_cast<const OrderBookEvent*>(event));
            break;
        case EventType::ORDER_ACK:
            dispatch_order_ack(*reinterpret_cast<const OrderAckEvent*>(event));
            break;
        case EventType::ORDER_FILL:
            dispatch_order_fill(*reinterpret_cast<const OrderFillEvent*>(event));
            break;
        case EventType::ORDER_REJECT:
            dispatch_order_reject(*reinterpret_cast<const OrderRejectEvent*>(event));
            break;
        case EventType::ORDER_CANCEL:
            dispatch_order_cancel(*reinterpret_cast<const OrderCancelEvent*>(event));
            break;
        case EventType::STRATEGY_CONFIG:
            dispatch_strategy_config(*reinterpret_cast<const StrategyConfigEvent*>(event));
            break;
        case EventType::INSTRUMENT_DEFINITION:
            dispatch_instrument_definition(*reinterpret_cast<const InstrumentDefinitionEvent*>(event));
            break;
        case EventType::INSTRUMENT_STATUS:
            dispatch_instrument_status(*reinterpret_cast<const InstrumentStatusEvent*>(event));
            break;
        case EventType::LADDER_SUBSCRIPTION:
            dispatch_ladder_subscription(*reinterpret_cast<const LadderSubscriptionEvent*>(event));
            break;
        default:
            LOG_WARN("StrategyManager: Unknown event type: {}", static_cast<int>(event->type));
            break;
    }
}

void StrategyManager::check_loss_limits() {
    const auto now_ns = get_timestamp_ns();

    // Check each strategy
    for (auto& strategy : strategies_) {
        const std::string strategy_name(strategy->name());

        // Skip if already halted
        if (risk_manager_.is_halted(strategy_name)) {
            continue;
        }

        // Get risk limits
        const auto limits = risk_manager_.get_limits(strategy_name);

        // Calculate current PnL
        const auto pnl = pnl_calculator_.calculate_pnl(strategy_name, now_ns);

        // Check if total PnL has breached the loss limit
        if (pnl.total_pnl < limits.max_loss_per_strategy) {
            const double loss = from_price_t(pnl.total_pnl);
            const double max_loss = from_price_t(limits.max_loss_per_strategy);

            const std::string reason = "Loss limit breached: PnL=" +
                std::to_string(loss) + " < max_loss=" + std::to_string(max_loss);

            LOG_ERROR("StrategyManager: {} - {}", strategy_name, reason);

            // Close all open positions before halting — leaves no unmanaged exposure
            if (auto* base = dynamic_cast<StrategyBase*>(strategy.get())) {
                base->close_all_positions();
            }

            risk_manager_.halt_strategy(strategy_name, reason);

            // Stop the strategy
            strategy->stop();
        }
    }
}

// Every strategy is added by load_strategies() before brokers publish anything, so a
// definition reaches all of them as it arrives. defined_ keeps it for a list MANUAL is given later.
void StrategyManager::dispatch_instrument_definition(const InstrumentDefinitionEvent& event) {
    const std::string_view instrument(event.instrument);
    position_tracker_.set_contract_multiplier(instrument, event.contract_multiplier);
    LOG_INFO("StrategyManager: instrument {} on {}: tick={} multiplier={} currency={}",
             instrument, to_string(event.broker_id), from_price_t(event.tick_size),
             event.contract_multiplier, event.currency);
    for (auto& strategy : strategies_) {
        strategy->on_instrument_definition(event);
    }

    // A replay, as a venue loops its definitions, was listed or not the first time it came: MANUAL
    // takes a new list over defined_ itself. Only a first one goes to it.
    const auto broker = static_cast<size_t>(event.broker_id);
    if (broker < BROKER_COUNT && !defined_[broker].contains(instrument)) {
        // A copy: the event is packed, and emplace() would bind a reference to its field.
        defined_[broker].emplace(std::string(instrument), price_t{event.tick_size});
        list(event.broker_id, instrument, event.tick_size);
    }
}

void StrategyManager::list(BrokerId broker, std::string_view instrument, price_t tick_size) {
    // For the Web UI's ladders to pick from. Nothing is subscribed until one holds it.
    if (manual_) {
        manual_->add_listing(broker, instrument, tick_size);
    }
}

void StrategyManager::dispatch_ladder_subscription(const LadderSubscriptionEvent& event) {
    // Held or released once, whatever the WebServer sends: the subscription map counts MANUAL as
    // one holder, so a strategy holding the same book keeps it.
    // One a strategy only quotes or trades is the ladder's to hold too: it streams no book.
    const std::string_view instrument(event.instrument);
    if (!manual_ || !(event.subscribe
                          ? manual_->hold(event.broker_id, instrument, subscribed(event.broker_id, instrument))
                          : manual_->release(event.broker_id, instrument))) {
        return;
    }
    const SubscriptionRequest request(event.broker_id, ManualStrategy::SUBSCRIPTION, {std::string(instrument)});
    if (event.subscribe) {
        LOG_INFO("StrategyManager: a ladder holds {} on {}", instrument, to_string(event.broker_id));
        subscribe(request, false);
    } else {
        LOG_INFO("StrategyManager: no ladder holds {} on {} now", instrument, to_string(event.broker_id));
        unsubscribe(request);
    }
}

bool StrategyManager::subscribed(BrokerId broker, std::string_view instrument) const {
    std::string name(instrument);
    for (const auto type : {SubscriptionType::TRADE, SubscriptionType::QUOTE, SubscriptionType::LEVEL2,
                            SubscriptionType::ORDER_BOOK, SubscriptionType::MBO}) {
        SubscriptionKey key{broker, type, std::move(name)};
        if (subscriptions_.contains(key)) {
            return true;
        }
        name = std::move(std::get<2>(key));
    }
    return false;
}

void StrategyManager::dispatch_instrument_status(const InstrumentStatusEvent& event) {
    LOG_INFO("StrategyManager: instrument {} on {} is {}{}{}{}", event.instrument, to_string(event.broker_id),
             to_string(event.status), event.cancel_allowed ? "" : " (no cancels)",
             event.reason[0] != '\0' ? ": " : "", event.reason);
    for (auto& strategy : strategies_) {
        strategy->on_instrument_status(event);
    }
}

void StrategyManager::dispatch_trade(const TradeEvent& event) {
    pnl_calculator_.update_mark_price(std::string(event.instrument), event.price, event.timestamp_ns);
    for (auto& strategy : strategies_) {
        strategy->on_trade(event);
    }
}

void StrategyManager::dispatch_quote(const QuoteEvent& event) {
    const bool bid_valid = event.bid_price != NULL_PRICE;
    const bool ask_valid = event.ask_price != NULL_PRICE;
    if (bid_valid && ask_valid) {
        pnl_calculator_.update_top_of_book(std::string(event.instrument),
                                            event.bid_price,
                                            event.ask_price,
                                            event.timestamp_ns);
    } else {
        LOG_DEBUG("dispatch_quote: pnl calculator skipping mark price update for {} — bid={} ask={} (NULL_PRICE={})",
                    event.instrument,
                    event.bid_price, event.ask_price, NULL_PRICE);
    }
    for (auto& strategy : strategies_) {
        strategy->on_quote(event);
    }
}

void StrategyManager::dispatch_level2_updates(const Level2UpdateEvent& event) {
    // Only the books strategies subscribed. MANUAL's instruments stream to the Web UI alone.
    slick::orderbook::OrderBookL2* book = find_l2_book(event.instrument);
    if (book == nullptr) {
        return;
    }

    if (event.is_snapshot) {
        book->clear();
        LOG_DEBUG("StrategyManager: dispatch snapshot {}", event.instrument);
    }

    for (uint16_t i = 0, last = event.num_levels - 1; i < event.num_levels; ++i) {
        if (event.levels[i].side == Side::BUY) {
            book->updateLevel(slick::orderbook::Side::Buy,
                                static_cast<slick::orderbook::Price>(event.levels[i].price),
                                static_cast<slick::orderbook::Quantity>(event.levels[i].quantity),
                                event.event_time_ns,
                                event.sequence_num,
                                i == last);
        }
        else {
            book->updateLevel(slick::orderbook::Side::Sell,
                                static_cast<slick::orderbook::Price>(event.levels[i].price),
                                static_cast<slick::orderbook::Quantity>(event.levels[i].quantity),
                                event.event_time_ns,
                                event.sequence_num,
                                i == last);
        }
    }
}

void StrategyManager::on_top_of_book_update(const std::string &instrument, const slick::orderbook::TopOfBook& tob) {
    // A side with nothing on it has no price. The book reports such a side as price 0 - a
    // real price for a futures spread - so it is told apart by its quantity, and goes out as
    // NULL_PRICE: marking a position against it would value it at zero, or at half the
    // price of the side that is there.
    const price_t bid = tob.bid_quantity > 0 ? static_cast<price_t>(tob.best_bid) : NULL_PRICE;
    const price_t ask = tob.ask_quantity > 0 ? static_cast<price_t>(tob.best_ask) : NULL_PRICE;
    if (bid != NULL_PRICE && ask != NULL_PRICE) {
        pnl_calculator_.update_top_of_book(instrument, bid, ask, tob.timestamp);
    }

    QuoteEvent event;
    event.set_instrument(instrument);
    event.bid_price = bid;
    event.bid_quantity = static_cast<qty_t>(tob.bid_quantity);
    event.ask_price = ask;
    event.ask_quantity = static_cast<qty_t>(tob.ask_quantity);
    event.event_time_ns = tob.timestamp;

    // Published for readers outside this thread - the dashboard refreshes PnL on it - as a
    // TOB record, not a QUOTE: this thread reads the same bus, and a QUOTE would come back
    // and be handed to the strategies a second time.
    EventSerializer::publish<QuoteEvent>(*tob_producer_buffer_, [&](QuoteEvent& q) {
        q = event;
        q.type = EventType::TOB;
    });

    for (auto& strategy : strategies_) {
        strategy->on_quote(event);
    }
}

StrategyManager::MboBook& StrategyManager::get_or_create_mbo_book(std::string_view instrument) {
    auto it = mbo_books_.find(instrument);
    if (it == mbo_books_.end()) [[unlikely]] {
        std::string name(instrument);
        // Quotes come from this book unless an L2 book of the instrument already gives them.
        // Found by name, so its id only labels it: with every id taken, 0 shares no book.
        auto entry = std::make_unique<MboBook>(get_or_create_symbol_id(name).value_or(0), name,
                                               !l2_books_.contains(name));
        it = mbo_books_.emplace(std::move(name), std::move(entry)).first;
    }
    return *it->second;
}

void StrategyManager::dispatch_mbo_update(const MboUpdateEvent& event) {
    MboBook& entry = get_or_create_mbo_book(event.instrument);
    auto& book = entry.book;

    if (event.is_snapshot) {
        book.clear(event.event_time_ns);
        LOG_DEBUG("StrategyManager: dispatch MBO snapshot {}", entry.instrument);
    }

    for (uint16_t i = 0; i < event.num_orders; ++i) {
        const MboOrder& order = event.orders[i];
        // The broker already put the feed in order, so the book checks no sequence numbers.
        const bool is_last = event.is_last && i + 1 == event.num_orders;
        if (order.action == MboAction::REMOVE) {
            book.deleteOrder(order.order_id, event.event_time_ns, 0, is_last);
        } else {
            book.addOrModifyOrder(order.order_id,
                                  order.side == Side::BUY ? slick::orderbook::Side::Buy : slick::orderbook::Side::Sell,
                                  static_cast<slick::orderbook::Price>(order.price),
                                  static_cast<slick::orderbook::Quantity>(order.quantity),
                                  event.event_time_ns,
                                  order.priority,
                                  0,
                                  is_last);
        }
    }

    // Strategies see the book only when it is whole: at the end of the exchange event.
    if (!event.is_last) {
        return;
    }

    if (entry.publishes_top) {
        const auto top = book.getTopOfBook();
        if (top.best_bid != entry.top.best_bid || top.bid_quantity != entry.top.bid_quantity ||
            top.best_ask != entry.top.best_ask || top.ask_quantity != entry.top.ask_quantity) {
            entry.top = top;
            on_top_of_book_update(entry.instrument, top);
        }
    }

    for (auto& strategy : strategies_) {
        strategy->on_order_book_l3(entry.instrument, book);
    }
}

void StrategyManager::on_order_book_update(const std::string& instrument, const slick::orderbook::OrderBookL2& book) {
    for (auto& strategy : strategies_) {
        strategy->on_order_book(instrument, book);
    }
}

void StrategyManager::dispatch_order_book(const OrderBookEvent& event) {
    slick::orderbook::OrderBookL2* book = find_l2_book(event.instrument);
    if (book == nullptr) {
        return;
    }

    if (event.is_snapshot) {
        book->clear();
    }

    const bool is_last_bid = (event.num_asks == 0);
    for (uint16_t i = 0; i < event.num_bids; ++i) {
        book->updateLevel(slick::orderbook::Side::Buy,
                            static_cast<slick::orderbook::Price>(event.bids[i].price),
                            static_cast<slick::orderbook::Quantity>(event.bids[i].quantity),
                            event.event_time_ns,
                            event.sequence_num,
                            is_last_bid && (i == event.num_bids - 1));
    }
    for (uint16_t i = 0; i < event.num_asks; ++i) {
        book->updateLevel(slick::orderbook::Side::Sell,
                            static_cast<slick::orderbook::Price>(event.asks[i].price),
                            static_cast<slick::orderbook::Quantity>(event.asks[i].quantity),
                            event.event_time_ns,
                            event.sequence_num,
                            i == event.num_asks - 1);
    }
}

std::optional<slick::orderbook::SymbolId> StrategyManager::get_or_create_symbol_id(std::string_view instrument) {
    if (const auto it = symbol_ids_.find(instrument); it != symbol_ids_.end()) {
        return it->second;
    }
    if (symbol_ids_.size() > std::numeric_limits<slick::orderbook::SymbolId>::max()) {
        LOG_ERROR("StrategyManager: no book for {}: all {} book ids are taken", instrument, symbol_ids_.size());
        return std::nullopt;
    }
    const slick::orderbook::SymbolId sid = next_symbol_id_++;
    symbol_ids_.emplace(std::string(instrument), sid);
    LOG_DEBUG("StrategyManager: Assigned symbol_id={} for {}", sid, instrument);
    return sid;
}

void StrategyManager::dispatch_order_ack(const OrderAckEvent& event) {
    LOG_INFO("StrategyManager: dispatch_order_ack order_id={} client_order_id={} to {} strategies",
                std::string(event.order_id), std::string(event.client_order_id), strategies_.size());
    for (auto& strategy : strategies_) {
        strategy->on_order_ack(event);
    }
}

void StrategyManager::dispatch_order_fill(const OrderFillEvent& event) {
    // A broker replays its fill journal after a restart: what the strategies had before
    // the process stopped - booked, or seen to be no one's - is not had twice.
    if (position_tracker_.fill_handled(event)) {
        LOG_DEBUG("StrategyManager: fill {} of {} (journal {}) was handled before a restart - dropped",
                  std::string(event.fill_id), std::string(event.client_order_id), event.journal_seq);
        return;
    }

    // Update mark price for PnL calculation
    pnl_calculator_.update_mark_price(std::string(event.instrument),
                                        event.fill_price,
                                        event.timestamp_ns);

    // Dispatch to strategies (they track their own fills)
    for (auto& strategy : strategies_) {
        strategy->on_order_fill(event);
    }
    position_tracker_.fill_dispatched(event);
}

void StrategyManager::dispatch_order_reject(const OrderRejectEvent& event) {
    for (auto& strategy : strategies_) {
        strategy->on_order_reject(event);
    }
}

void StrategyManager::dispatch_order_cancel(const OrderCancelEvent& event) {
    for (auto& strategy : strategies_) {
        strategy->on_order_cancel(event);
    }
}
