// SlickTrader - Production Trading System
//
// This is the main executable for the SlickTrader algorithmic trading framework.
// It demonstrates the complete system including:
// - TradingEngine initialization and lifecycle management
// - Multi-broker support (Coinbase)
// - Strategy registration and execution
// - Position tracking and PnL calculation
// - Risk management
// - Web API for monitoring
//
// Usage:
//   ./slick-trader config.json
//
// Required environment variables (for order submission):
//   COINBASE_API_KEY - Coinbase API key
//   COINBASE_API_SECRET - Coinbase API secret

#include <slick/logger.hpp>
#include <slick-trader/core/trading_engine.hpp>
#include <slick-trader/core/broker_factory.hpp>
#include <slick-trader/core/risk_manager.hpp>
#include <slick-trader/web/web_server.hpp>
#include <slick-trader/strategy/strategy_loader.hpp>
#include <slick-trader/brokers/imarket_data_connector.hpp>
#include <coinbase/logging.hpp>
#include <nlohmann/json.hpp>

#include <iostream>
#include <fstream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>

using namespace slick::trader;
using Logger = slick::logger::Logger;

std::atomic<bool> running{true};

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down gracefully..." << std::endl;
    running = false;
}

int main(int argc, char* argv[]) {
    // Setup signal handlers for graceful shutdown
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Initialize logger with console and file output
    auto &logger = Logger::instance();
    logger.clear_sinks();
    logger.add_console_sink(true, true);
    logger.add_file_sink("slick-trader.log");
    logger.set_level(slick::logger::LogLevel::L_INFO);
    logger.init();

    // Forward Coinbase library logs to slick-logger
    coinbase::logging::set_log_handler([&logger](coinbase::logging::LogLevel level, const char* format_text, std::format_args args) {
        logger.log(static_cast<slick::logger::LogLevel>(level), format_text, std::move(args));
    });

    LOG_INFO("╔══════════════════════════════════════════════╗");
    LOG_INFO("║       SlickTrader v0.1.0 - Starting...      ║");
    LOG_INFO("╚══════════════════════════════════════════════╝");

    // Check command line arguments
    if (argc < 2) {
        LOG_ERROR("Usage: {} <config.json>", argv[0]);
        LOG_ERROR("Example: {} config.json", argv[0]);
        return 1;
    }

    std::string config_path = argv[1];
    LOG_INFO("Loading configuration from: {}", config_path);

    // Initialize Trading Engine (singleton)
    auto& engine = TradingEngine::instance();

    if (!engine.initialize(config_path)) {
        LOG_ERROR("Failed to initialize TradingEngine from config: {}", config_path);
        return 1;
    }

    LOG_INFO("TradingEngine initialized successfully");

    // ========================================================================
    // Broker Setup from Configuration
    // ========================================================================
    LOG_INFO("Setting up brokers from configuration...");

    // Re-read config for broker setup
    std::ifstream config_file_brokers(config_path);
    if (!config_file_brokers.is_open()) {
        LOG_ERROR("Failed to re-open config file for brokers: {}", config_path);
        return 1;
    }

    nlohmann::json config;
    try {
        config_file_brokers >> config;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse config file for brokers: {}", e.what());
        return 1;
    }

    int broker_count = 0;
    if (config.contains("brokers") && config["brokers"].is_array()) {
        for (const auto& broker_json : config["brokers"]) {
            // Parse broker configuration
            auto broker_config = BrokerFactory::parse_broker_config(broker_json);

            if (!broker_config.enabled) {
                LOG_INFO("Skipping disabled broker: {}", broker_config.broker_name);
                continue;
            }

            // Create broker
            auto broker = BrokerFactory::create_broker(broker_config);
            if (!broker) {
                LOG_ERROR("Failed to create broker: {}", broker_config.broker_name);
                continue;
            }

            // Register broker with engine
            if (!engine.register_broker(broker)) {
                LOG_ERROR("Failed to register broker: {}", broker_config.broker_name);
                continue;
            }

            LOG_INFO("✓ Registered broker: {}", broker_config.broker_name);
            broker_count++;

            // Store subscriptions for later (after engine.start())
            // We'll apply subscriptions after brokers are started
        }
    } else {
        LOG_WARN("⚠ No brokers configured in config.json");
        LOG_WARN("⚠ Add a 'brokers' array to enable market data and trading");
    }

    if (broker_count == 0) {
        LOG_ERROR("No brokers registered - cannot continue");
        return 1;
    }

    LOG_INFO("✓ Registered {} broker(s)", broker_count);

    // ========================================================================
    // Strategy Loading
    // ========================================================================
    LOG_INFO("Loading strategies...");

    StrategyLoader loader;
    int strategy_count = 0;

    // Get strategy plugins folder from config
    const std::string& plugins_folder = engine.get_strategy_plugins_folder();
    LOG_INFO("Strategy plugins folder: {}", plugins_folder);

    // Define plugin filenames to load
    #ifdef _WIN32
        const std::vector<std::string> plugin_names = {
            "simple_strategy_plugin.dll",
            "simple_logger_strategy_plugin.dll"
        };
    #else
        const std::vector<std::string> plugin_names = {
            "libsimple_strategy_plugin.so",
            "libsimple_logger_strategy_plugin.so"
        };
    #endif

    // Construct full paths and load each plugin
    for (const auto& plugin_name : plugin_names) {
        std::string plugin_path = plugins_folder + "/" + plugin_name;

        auto strategy = loader.load(plugin_path);
        if (strategy) {
            engine.add_strategy(strategy);
            LOG_INFO("✓ Loaded strategy: {} from {}", strategy->name(), plugin_path);

            // Set default risk limits for each strategy
            RiskLimits limits;
            limits.max_position_size = to_qty_t(1.0);       // Max 1 BTC/ETH
            limits.max_order_value = to_price_t(50000.0);   // Max $50k per order
            limits.max_orders_per_second = 10;              // Max 10 orders/sec
            engine.get_risk_manager().set_limits(strategy->name(), limits);

            strategy_count++;
        } else {
            LOG_WARN("⚠ Failed to load strategy plugin: {}", plugin_path);
            LOG_WARN("  Error: {}", loader.get_last_error());
        }
    }

    if (strategy_count == 0) {
        LOG_WARN("⚠ No strategies loaded - system will receive market data but won't trade");
    } else {
        LOG_INFO("✓ Loaded {} strategy/strategies", strategy_count);
    }

    // Market data subscriptions will be applied after engine.start()
    // (stored in broker configs)

    // ========================================================================
    // Start Trading Engine
    // ========================================================================
    LOG_INFO("Starting TradingEngine...");

    engine.start();

    LOG_INFO("✓ TradingEngine started");

    // ========================================================================
    // Market Data Subscriptions (Strategy-Driven)
    // ========================================================================
    // Collect subscription requirements from all strategies and apply them
    engine.apply_strategy_subscriptions();

    // ========================================================================
    // System Ready
    // ========================================================================
    LOG_INFO("");
    LOG_INFO("╔══════════════════════════════════════════════╗");
    LOG_INFO("║       SlickTrader is now RUNNING            ║");
    LOG_INFO("╚══════════════════════════════════════════════╝");
    LOG_INFO("");
    LOG_INFO("Market Data: Receiving live feeds from Coinbase");
    LOG_INFO("Strategies: {} active", strategy_count);
    LOG_INFO("Web Server: http://localhost:{}", engine.get_web_server().get_port());
    LOG_INFO("");
    LOG_INFO("Available endpoints:");
    LOG_INFO("  GET /api/health          - Health check");
    LOG_INFO("  GET /api/positions       - All positions");
    LOG_INFO("  GET /api/positions/:id   - Strategy positions");
    LOG_INFO("  GET /api/pnl             - All PnL snapshots");
    LOG_INFO("  GET /api/pnl/:id         - Strategy PnL");
    LOG_INFO("  GET /api/strategies      - Strategy list");
    LOG_INFO("");
    LOG_INFO("Press Ctrl+C to shutdown gracefully");
    LOG_INFO("");

    // ========================================================================
    // Main Event Loop
    // ========================================================================
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // ========================================================================
    // Graceful Shutdown
    // ========================================================================
    LOG_INFO("");
    LOG_INFO("╔══════════════════════════════════════════════╗");
    LOG_INFO("║       Initiating Graceful Shutdown...       ║");
    LOG_INFO("╚══════════════════════════════════════════════╝");

    engine.stop();
    LOG_INFO("✓ TradingEngine stopped");

    engine.shutdown();
    LOG_INFO("✓ TradingEngine shutdown complete");

    LOG_INFO("");
    LOG_INFO("╔══════════════════════════════════════════════╗");
    LOG_INFO("║       SlickTrader Shutdown Complete         ║");
    LOG_INFO("╚══════════════════════════════════════════════╝");
    LOG_INFO("Goodbye!");

    return 0;
}
