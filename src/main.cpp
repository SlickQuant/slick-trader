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

#include <iostream>
#include <csignal>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace slick::trader;

std::atomic_bool running{true};

void signal_handler(int signal) {
    LOG_INFO("Received signal {}, shutting down gracefully...", signal);
    running.store(false, std::memory_order_release);
}

int main(int argc, char* argv[]) {
    // Check command line arguments
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <config.json>" << std::endl;
        return 1;
    }

    std::string config_path = argv[1];

    auto &engine = TradingEngine::instance();
    if (!engine.initialize(config_path)) {
        LOG_ERROR("TradingEngine failed to initialize");
        return 1;
    }


    // Register signal handlers after libraries have initialized
    // (Some libraries may override signal handlers during initialization)
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    engine.start();

    // ========================================================================
    // System Ready
    // ========================================================================
    LOG_INFO("");
    LOG_INFO("========================================================================");
    LOG_INFO("          SlickTrader is now RUNNING...");
    LOG_INFO("========================================================================");
    LOG_INFO("");
    LOG_INFO("Press Ctrl+C to shutdown gracefully");
    LOG_INFO("");

    // ========================================================================
    // Main Event Loop
    // ========================================================================
    while (running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // ========================================================================
    // Graceful Shutdown
    // ========================================================================
    LOG_INFO("");
    LOG_INFO("========================================================================");
    LOG_INFO("          Initiating Graceful Shutdown...");
    LOG_INFO("========================================================================");

    engine.shutdown();

    LOG_INFO("");
    LOG_INFO("========================================================================");
    LOG_INFO("          SlickTrader Shutdown Complete");
    LOG_INFO("========================================================================");

    return 0;
}
