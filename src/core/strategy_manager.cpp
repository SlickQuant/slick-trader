#include <slick/logger.hpp>
#include <slick-trader/core/strategy_manager.hpp>
#include <slick-trader/core/strategy_config.hpp>
#include <slick-trader/strategy/manual_strategy.hpp>

using namespace slick;
using namespace slick::trader;

StrategyManager::StrategyManager(
    const json& config,
    PositionTracker& position_tracker,
    PnLCalculator& pnl_calculator,
    RiskManager& risk_manager,
    slick::stream_buffer_multiplexer& event_mux,
    slick::queue<uint8_t>& md_request_queue,
    slick::queue<uint8_t>& order_request_queue
)
    : position_tracker_(position_tracker)
    , pnl_calculator_(pnl_calculator)
    , risk_manager_(risk_manager)
    , event_mux_(event_mux)
    , md_request_queue_(md_request_queue)
    , order_request_queue_(order_request_queue)
    , tob_producer_buffer_(
        event_mux.add_producer(
            event_mux.producer_count(),
            config.value("TOB_queue_size", 1048576),
            config.value("TOB_control_size", 65536),
            config.contains("TOB_shm_name") ? config["TOB_shm_name"].get_ref<const std::string&>().c_str() : nullptr
        ))
{}

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

    // Determine plugin extension based on platform
    #ifdef _WIN32
        const std::string plugin_extension = ".dll";
    #else
        const std::string plugin_extension = ".so";
    #endif

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

                // Pass custom parameters to strategy if available
                if (has_config) {
                    const auto& params = iter->second.parameters;
                    if (!params.empty()) {
                        // Try to cast to StrategyBase to set config
                        if (auto* base_strategy = dynamic_cast<StrategyBase*>(strategy.get())) {
                            base_strategy->initialize(
                                params,
                                position_tracker_,
                                pnl_calculator_,
                                risk_manager_,
                                md_request_queue_,
                                order_request_queue_
                            );
                            LOG_INFO("  Passed {} custom parameter(s) to strategy {}", params.size(), strategy_name);
                        }
                    }
                } else {
                    if (auto* base_strategy = dynamic_cast<StrategyBase*>(strategy.get())) {
                        base_strategy->initialize(
                            {},
                            position_tracker_,
                            pnl_calculator_,
                            risk_manager_,
                            md_request_queue_,
                            order_request_queue_
                        );
                        LOG_INFO("  Passed default parameters to strategy {}", strategy_name);
                    }
                }

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

    // Always register ManualStrategy for orders placed through the Web UI
    {
        auto manual = std::make_shared<ManualStrategy>();
        manual->initialize({}, position_tracker_, pnl_calculator_, risk_manager_,
                           md_request_queue_, order_request_queue_);
        risk_manager_.set_limits(ManualStrategy::ID, RiskLimits{});
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

    // Start all strategies
    for (auto& strategy : strategies_) {
        strategy->start();

        // subscribe MD update
        auto subscriptions = strategy->get_subscriptions();
        for (const auto &subscription : subscriptions) {
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

    // Stop all strategies
    for (auto& strategy : strategies_) {
        auto subscriptions = strategy->get_subscriptions();
        for (const auto &subscription : subscriptions) {
            unsubscribe(subscription);
        }

        strategy->stop();
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    LOG_INFO("StrategyManager: Stopped");
}

void StrategyManager::subscribe(const SubscriptionRequest& sub) {
    std::vector<std::string> instruments;
    for (auto& inst : sub.instruments) {
        auto iter = subscriptions_.find(inst);
        if (iter == subscriptions_.end()) {
            subscriptions_.emplace(std::make_pair(inst, 1));
            instruments.push_back(inst);
        } else {
            ++iter->second;
        }
    }
    
    // Remove duplicates
    std::sort(instruments.begin(), instruments.end());
    instruments.erase(
        std::unique(instruments.begin(), instruments.end()),
        instruments.end()
    );

    if (!instruments.empty()) {
        uint32_t size = sizeof(MDSubscription) + instruments.size() * sizeof(char [32]);
        EventSerializer::publish<MDSubscription>(md_request_queue_, size, [&](MDSubscription &event) {
            event.type = EventType::SUBSCRIBE;
            event.broker_id = sub.broker_id;
            event.subscription_type = sub.type;
            event.depth = sub.depth;
            event.num_instruments = instruments.size();
            size_t i = 0;
            for (const auto& inst : instruments) {
                assert(inst.size() < sizeof(event.instruments[i]));
                std::memcpy(event.instruments[i], inst.c_str(), sizeof(event.instruments[i]));
            }
        });

        if (sub.type == SubscriptionType::LEVEL2 || sub.type == SubscriptionType::ORDER_BOOK) {
            subscribe_book_update(instruments);
        }
    }
}

void StrategyManager::unsubscribe(const SubscriptionRequest& sub) {
    std::vector<std::string> instruments;
    for (auto& inst : sub.instruments) {
        auto iter = subscriptions_.find(inst);
        if (iter != subscriptions_.end()) {
            if (--iter->second == 0) {
                subscriptions_.erase(iter);
                instruments.push_back(inst);
            }
        }
    }

    if (instruments.empty()) {
        uint32_t size = sizeof(MDSubscription) + instruments.size() * sizeof(char [32]);
        EventSerializer::publish<MDSubscription>(md_request_queue_, size, [&](MDSubscription &event) {
            event.type = EventType::UNSUBSCRIBE;
            event.broker_id = sub.broker_id;
            event.subscription_type = sub.type;
            event.depth = sub.depth;
            event.num_instruments = instruments.size();
            size_t i = 0;
            for (const auto& inst : instruments) {
                assert(inst.size() < sizeof(event.instruments[i]));
                std::memcpy(event.instruments[i], inst.c_str(), sizeof(event.instruments[i]));
            }
        });
    }
}

void StrategyManager::run() {
    LOG_INFO("StrategyManager: Worker thread started");

    auto last_periodic_check = std::chrono::steady_clock::now();
    constexpr auto check_interval = std::chrono::seconds(1);
    bool has_event = false;

    while (running_.load(std::memory_order_relaxed)) {
        if (auto record = event_mux_.read(event_cursor_)) {
            process_event(reinterpret_cast<const Event*>(record.data));
            has_event = true;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - last_periodic_check >= check_interval) {
            check_loss_limits();
            check_broker_status();
            last_periodic_check = now;
            has_event = true;
        }

        if (!has_event) {
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
        case EventType::LEVEL2_UPDATE:
            dispatch_level2_updates(*reinterpret_cast<const Level2UpdateEvent*>(event));
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
// definition reaches all of them as it arrives - there is nothing to replay.
void StrategyManager::dispatch_instrument_definition(const InstrumentDefinitionEvent& event) {
    const std::string_view instrument(event.instrument);
    position_tracker_.set_contract_multiplier(instrument, event.contract_multiplier);
    LOG_INFO("StrategyManager: instrument {} on {}: tick={} multiplier={} currency={}",
             instrument, to_string(event.broker_id), from_price_t(event.tick_size),
             event.contract_multiplier, event.currency);
    for (auto& strategy : strategies_) {
        strategy->on_instrument_definition(event);
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
    const std::string instrument(event.instrument);
    slick::orderbook::SymbolId sid = get_or_create_symbol_id(instrument);
    slick::orderbook::OrderBookL2* book = book_manager_.getOrCreateOrderBook(sid);

    if (event.is_snapshot) {
        book->clear();
        LOG_DEBUG("StrategyManager: dispatch snapshot {}", instrument);
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
    if (tob.best_bid != NULL_PRICE && tob.best_ask != NULL_PRICE) {
        pnl_calculator_.update_top_of_book(instrument, tob.best_bid, tob.best_ask, tob.timestamp);
    }

    EventSerializer::publish<QuoteEvent>(*tob_producer_buffer_, [&](QuoteEvent& q) {
        q.set_instrument(instrument);
        q.bid_price = tob.best_bid;
        q.bid_quantity = static_cast<qty_t>(tob.bid_quantity);
        q.ask_price = tob.best_ask;
        q.ask_quantity = static_cast<qty_t>(tob.ask_quantity);
        q.event_time_ns = tob.timestamp;
    });

    QuoteEvent event;
    event.set_instrument(instrument);
    event.bid_price = tob.best_bid;
    event.bid_quantity = static_cast<qty_t>(tob.bid_quantity);
    event.ask_price = tob.best_ask;
    event.ask_quantity = static_cast<qty_t>(tob.ask_quantity);
    event.event_time_ns = tob.timestamp;
    for (auto& strategy : strategies_) {
        strategy->on_quote(event);
    }
}

void StrategyManager::on_order_book_update(const slick::orderbook::OrderBookL2& book) {
    for (auto& strategy : strategies_) {
        strategy->on_order_book(book);
    }
}

void StrategyManager::dispatch_order_book(const OrderBookEvent& event) {
    const std::string instrument(event.instrument);
    slick::orderbook::SymbolId sid = get_or_create_symbol_id(instrument);
    slick::orderbook::OrderBookL2* book = book_manager_.getOrCreateOrderBook(sid);

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

slick::orderbook::SymbolId StrategyManager::get_or_create_symbol_id(const std::string& instrument) {
    auto it = symbol_ids_.find(instrument);
    if (it == symbol_ids_.end()) {
        slick::orderbook::SymbolId sid = next_symbol_id_++;
        symbol_ids_.emplace(instrument, sid);
        symbol_names_.emplace(sid, instrument);
        LOG_INFO("StrategyManager: Assigned symbol_id={} for {}", sid, instrument);
        return sid;
    }
    return it->second;
}

void StrategyManager::dispatch_order_ack(const OrderAckEvent& event) {
    LOG_INFO("StrategyManager: dispatch_order_ack order_id={} client_order_id={} to {} strategies",
                std::string(event.order_id), std::string(event.client_order_id), strategies_.size());
    for (auto& strategy : strategies_) {
        strategy->on_order_ack(event);
    }
}

void StrategyManager::dispatch_order_fill(const OrderFillEvent& event) {
    // Update mark price for PnL calculation
    pnl_calculator_.update_mark_price(std::string(event.instrument),
                                        event.fill_price,
                                        event.timestamp_ns);

    // Dispatch to strategies (they track their own fills)
    for (auto& strategy : strategies_) {
        strategy->on_order_fill(event);
    }
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
