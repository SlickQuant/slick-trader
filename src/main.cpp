#include <slick/logger.hpp>
#include <slick-trader/events/event.hpp>
#include <slick-trader/events/trade_event.hpp>
#include <slick-trader/events/quote_event.hpp>
#include <slick-trader/events/order_book_event.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick-trader/utils/types.hpp>
#include <coinbase/logging.hpp>

#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

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
    logger.init();
    coinbase::logging::set_log_handler([&logger](coinbase::logging::LogLevel level, const char* format_text, std::format_args args) {
        logger.log(static_cast<slick::logger::LogLevel>(level), format_text, std::move(args));
    });
    LOG_INFO("SlickTrader v0.1.0 starting...");

    // Print event sizes for verification
    LOG_INFO("Event sizes (bytes):");
    LOG_INFO("  TradeEvent: {}", sizeof(slick::trader::TradeEvent));
    LOG_INFO("  QuoteEvent: {}", sizeof(slick::trader::QuoteEvent));
    LOG_INFO("  OrderBookEvent: {}", sizeof(slick::trader::OrderBookEvent));
    LOG_INFO("  OrderAckEvent: {}", sizeof(slick::trader::OrderAckEvent));
    LOG_INFO("  OrderFillEvent: {}", sizeof(slick::trader::OrderFillEvent));
    LOG_INFO("  OrderRejectEvent: {}", sizeof(slick::trader::OrderRejectEvent));
    LOG_INFO("  OrderCancelEvent: {}", sizeof(slick::trader::OrderCancelEvent));
    LOG_INFO("  NewOrderEvent: {}", sizeof(slick::trader::NewOrderEvent));
    LOG_INFO("  CancelOrderEvent: {}", sizeof(slick::trader::CancelOrderEvent));

    // Test event creation
    slick::trader::TradeEvent trade;
    trade.broker_id = slick::trader::BrokerId::COINBASE;
    trade.set_instrument("BTC-USD");
    trade.price = slick::trader::to_price_t(50000.12345678);
    trade.quantity = slick::trader::to_qty_t(0.1);
    trade.is_buyer_maker = true;
    trade.trade_id = 12345;

    LOG_INFO("Created TradeEvent:");
    LOG_INFO("  Broker: {}", static_cast<int>(trade.broker_id));
    LOG_INFO("  Instrument: {}", trade.instrument);
    LOG_INFO("  Price: {:.8f}", slick::trader::from_price_t(trade.price));
    LOG_INFO("  Quantity: {:.8f}", slick::trader::from_qty_t(trade.quantity));

    LOG_INFO("SlickTrader is ready. Press Ctrl+C to exit.");

    // Main event loop
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    LOG_INFO("SlickTrader shutting down...");
    LOG_INFO("Goodbye!");

    return 0;
}
