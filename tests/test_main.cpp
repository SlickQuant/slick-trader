// GTest entry point. StrategyLoader and StrategyBase log through slick-logger, which
// has to be initialized before anything calls a LOG_* macro. The console shows errors only:
// the tests deliberately drive the "foreign order ignored" paths, which are chatty at debug
// level. Warnings go to test::LogCount alone.

#include "log_count.hpp"

#include <gtest/gtest.h>
#include <slick/logger.hpp>

int main(int argc, char** argv) {
    auto& logger = slick::logger::Logger::instance();
    logger.clear_sinks();
    auto console = std::make_shared<slick::logger::ConsoleSink>(true, true);
    console->set_min_level(slick::logger::LogLevel::L_ERROR);
    logger.add_sink(std::move(console));
    logger.add_sink(slick::trader::test::LogCountSink::instance());
    logger.set_level(slick::logger::LogLevel::L_WARN);
    logger.init();

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
