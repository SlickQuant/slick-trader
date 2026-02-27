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
#include <slick-trader/core/strategy_config.hpp>
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
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace slick::trader;
using Logger = slick::logger::Logger;
using json = nlohmann::json;

std::atomic<bool> running{true};

#ifdef _WIN32
// Windows console control handler
BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            LOG_INFO("Received shutdown signal, shutting down gracefully...");
            running = false;
            return TRUE;
        default:
            return FALSE;
    }
}
#endif

void signal_handler(int signal) {
    LOG_INFO("Received signal {}, shutting down gracefully...", signal);
    running = false;
}

int main(int argc, char* argv[]) {
    // Check command line arguments
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <config.json>" << std::endl;
        return 1;
    }

    std::string config_path = argv[1];

    std::ifstream config_file(config_path);
    if (!config_file.is_open()) {
        std::cerr << "Failed to open config file: " << config_path << std::endl;
        return 1;
    }

    json config;
    try {
        config = json::parse(config_file);
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse config file: " << e.what() << std::endl;
        return 1;
    }

    std::string log_file = config.value<std::string>("log_file_path", "logs/slick_trader.log");
    slick::logger::LogLevel log_level = config.value<slick::logger::LogLevel>("log_level", slick::logger::LogLevel::L_INFO);

    // Initialize logger with console and file output
    auto &logger = Logger::instance();
    logger.clear_sinks();
#ifdef DEBUG
    logger.add_console_sink(true, true);
#endif
    logger.add_file_sink(log_file);
    logger.set_level(log_level);
    logger.init();

    // Forward Coinbase library logs to slick-logger
    coinbase::logging::set_log_handler([&logger](coinbase::logging::LogLevel level, const char* format_text, std::format_args args) {
        logger.log(static_cast<slick::logger::LogLevel>(level), format_text, std::move(args));
    });

    LOG_INFO("╔══════════════════════════════════════════════╗");
    LOG_INFO("║       SlickTrader v0.1.0 - Starting...       ║");
    LOG_INFO("╚══════════════════════════════════════════════╝");

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
    // Strategy Loading (Dynamic Discovery)
    // ========================================================================
    LOG_INFO("Loading strategies...");

    StrategyLoader loader;
    int strategy_count = 0;

    // Get strategy plugins folder from config
    const std::string& plugins_folder = engine.get_strategy_plugins_folder();
    LOG_INFO("Strategy plugins folder: {}", plugins_folder);

    // Check if folder exists
    if (!std::filesystem::exists(plugins_folder)) {
        LOG_WARN("⚠ Strategy plugins folder does not exist: {}", plugins_folder);
        LOG_WARN("⚠ Creating folder...");
        std::filesystem::create_directories(plugins_folder);
    }

    // Load strategy configurations from main config
    std::unordered_map<std::string, StrategyConfig> strategy_configs;
    if (config.contains("strategies") && config["strategies"].is_array()) {
        for (const auto& strategy_json : config["strategies"]) {
            auto strategy_config = StrategyConfig::from_json(strategy_json);
            if (!strategy_config.name.empty()) {
                strategy_configs[strategy_config.name] = strategy_config;
                LOG_INFO("Loaded config for strategy: {}", strategy_config.name);
            }
        }
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
        for (const auto& entry : std::filesystem::directory_iterator(plugins_folder)) {
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
        LOG_WARN("⚠ No strategy plugins found in: {}", plugins_folder);
        LOG_WARN("⚠ Looking for files with extension: {}", plugin_extension);
    } else {
        LOG_INFO("Found {} plugin file(s) in {}", plugin_paths.size(), plugins_folder);

        // Sort for deterministic loading order
        std::sort(plugin_paths.begin(), plugin_paths.end());

        // Load each plugin
        for (const auto& plugin_path : plugin_paths) {
            auto strategy = loader.load(plugin_path);
            if (strategy) {
                std::string strategy_name = strategy->name();

                // Check if strategy is enabled in config (default: enabled)
                bool enabled = true;
                if (strategy_configs.count(strategy_name)) {
                    enabled = strategy_configs[strategy_name].enabled;
                }

                if (!enabled) {
                    LOG_INFO("⊘ Strategy {} is disabled in config, skipping", strategy_name);
                    continue;
                }

                engine.add_strategy(strategy);
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

                engine.get_risk_manager().set_limits(strategy_name, limits);

                strategy_count++;
            } else {
                LOG_WARN("⚠ Failed to load plugin: {}", std::filesystem::path(plugin_path).filename().string());
                LOG_WARN("  Error: {}", loader.get_last_error());
            }
        }
    }

    if (strategy_count == 0) {
        LOG_WARN("⚠ No strategies loaded - system will receive market data but won't trade");
        LOG_WARN("⚠ Place strategy plugins ({}) in: {}", plugin_extension, plugins_folder);
    } else {
        LOG_INFO("✓ Successfully loaded {} strategy/strategies", strategy_count);
    }

    // Market data subscriptions will be applied after engine.start()
    // (stored in broker configs)

    // ========================================================================
    // Start Trading Engine
    // ========================================================================
    engine.start();

    // Register signal handlers after libraries have initialized
    // (Some libraries may override signal handlers during initialization)
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif

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
    LOG_INFO("║         SlickTrader is now RUNNING           ║");
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
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // ========================================================================
    // Graceful Shutdown
    // ========================================================================
    LOG_INFO("");
    LOG_INFO("╔══════════════════════════════════════════════╗");
    LOG_INFO("║        Initiating Graceful Shutdown...       ║");
    LOG_INFO("╚══════════════════════════════════════════════╝");

    engine.stop();
    LOG_INFO("✓ TradingEngine stopped");

    engine.shutdown();
    LOG_INFO("✓ TradingEngine shutdown complete");

    LOG_INFO("");
    LOG_INFO("╔══════════════════════════════════════════════╗");
    LOG_INFO("║        SlickTrader Shutdown Complete         ║");
    LOG_INFO("╚══════════════════════════════════════════════╝");

    return 0;
}
