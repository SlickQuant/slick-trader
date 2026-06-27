// Example: Complete trading system with Coinbase integration and strategies
//
// This example demonstrates:
// - TradingEngine initialization
// - Coinbase broker setup with MD and order connectors
// - Strategy registration (SimpleLogger and SimpleMarketMaker)
// - Full event flow: MD → Strategy → Orders → Fills
//
// To run:
// 1. Set COINBASE_API_KEY and COINBASE_API_SECRET environment variables
// 2. Create config.json with queue sizes
// 3. Run: ./example_trading_system config.json

#include <slick/logger.hpp>
#include <slick-trader/core/trading_engine.hpp>
#include <slick-trader/web/web_server.hpp>
#include <slick-trader/core/strategy_loader.hpp>
#include <slick-trader/brokers/coinbase/coinbase_broker.hpp>
#include <slick-trader/brokers/coinbase/coinbase_md_connector.hpp>
#include <slick-trader/brokers/coinbase/coinbase_order_connector.hpp>

// Include example strategies (compiled directly into executable)
#include "../strategies/simple_logger_strategy.hpp"
#include "../strategies/simple_market_maker.hpp"
#include <slick/net/logging.hpp>

#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>

using namespace slick::trader;
using Logger = slick::logger::Logger;

std::atomic<bool> running{true};

void signal_handler(int signal) {
    std::cout << "Received signal " << signal << ", shutting down..." << std::endl;
    running = false;
}

int main(int argc, char* argv[]) {
    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Initialize logger
    auto &logger = Logger::instance();
    logger.clear_sinks();
    logger.add_console_sink(true, true);
    logger.add_file_sink("slick-trader.log");
    logger.set_level(slick::logger::LogLevel::L_DEBUG);
    logger.init();

    slick::net::set_log_handler(
        [&logger](slick::net::LogLevel level, const char* format_text, std::format_args args) {
            logger.log(static_cast<slick::logger::LogLevel>(level), format_text, std::move(args));
        },
        [&logger]() {
            return static_cast<slick::net::LogLevel>(logger.get_level());
        }
    );

    LOG_INFO("=== SlickTrader Example System v0.1.0 ===");

    // Check command line arguments
    if (argc < 2) {
        LOG_ERROR("Usage: {} <config.json>", argv[0]);
        return 1;
    }

    std::string config_path = argv[1];

    // Initialize Trading Engine
    auto& engine = TradingEngine::instance();

    if (!engine.initialize(config_path)) {
        LOG_ERROR("Failed to initialize TradingEngine");
        return 1;
    }

    LOG_INFO("Registering strategies...");

    // Strategy 1: Simple logger (logs all market data and order events)
    auto logger_strategy = std::make_shared<SimpleLoggerStrategy>();
    engine.add_strategy(logger_strategy);

    // Strategy 2: Dynamically load strategy plugin from shared library
    // NOTE: This is a demo strategy - it will actually submit orders!
    // Uncomment only if you want to trade (requires API credentials)
    /*
    LOG_INFO("Loading strategy plugin...");
    StrategyLoader loader;

    #ifdef _WIN32
        const std::string plugin_path = "strategies/simple_strategy_plugin.dll";
    #else
        const std::string plugin_path = "strategies/libsimple_strategy_plugin.so";
    #endif

    auto simple_strategy = loader.load(plugin_path);
    if (simple_strategy) {
        engine.add_strategy(simple_strategy);

        // Set risk limits for the plugin strategy
        RiskLimits limits;
        limits.max_position_size = to_qty_t(0.1);      // Max 0.1 BTC position
        limits.max_order_value = to_price_t(10000.0);  // Max $10k per order
        limits.max_orders_per_second = 10;             // Max 10 orders/sec
        engine.get_risk_manager().set_limits(simple_strategy->name(), limits);

        LOG_INFO("Successfully loaded and configured strategy plugin");
    } else {
        LOG_ERROR("Failed to load strategy plugin: {}", loader.get_last_error());
    }
    */

    // Strategy 3: Simple market maker for BTC-USD
    // Uncomment to enable market making (requires API credentials)
    /*
    auto market_maker = std::make_shared<SimpleMarketMaker>(
        "BTC-USD",  // instrument
        20.0,       // spread in basis points (0.2%)
        0.001       // order size in BTC
    );
    engine.add_strategy(market_maker);
    */

    // Subscribe to market data
    LOG_INFO("Subscribing to market data...");
    std::vector<std::string> instruments = {"BTC-USD", "ETH-USD"};

    md_connector->subscribe(SubscriptionType::TRADE, instruments);
    md_connector->subscribe(SubscriptionType::QUOTE, instruments);
    // md_connector->subscribe(SubscriptionType::ORDER_BOOK, instruments, 10);

    // Start the trading engine
    engine.start();

    LOG_INFO("=== SlickTrader is running ===");
    LOG_INFO("Receiving market data and processing events");
    LOG_INFO("");
    LOG_INFO("Web Dashboard: http://localhost:{}", engine.get_web_server().get_port());
    LOG_INFO("API Endpoints:");
    LOG_INFO("  - GET /api/health");
    LOG_INFO("  - GET /api/positions");
    LOG_INFO("  - GET /api/pnl");
    LOG_INFO("");
    LOG_INFO("Press Ctrl+C to shutdown gracefully");
    LOG_INFO("");

    // Main event loop
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Shutdown sequence
    LOG_INFO("");
    LOG_INFO("=== Shutting down SlickTrader ===");

    engine.stop();
    engine.shutdown();

    LOG_INFO("=== Shutdown complete ===");
    LOG_INFO("Goodbye!");

    return 0;
}
