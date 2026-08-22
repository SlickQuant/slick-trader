// GTest entry point. StrategyLoader and StrategyBase log through slick-logger, which
// has to be initialized before anything calls a LOG_* macro. Errors only — the tests
// deliberately drive the "foreign order ignored" paths, which are chatty at debug level.

#include <gtest/gtest.h>
#include <slick/logger.hpp>

int main(int argc, char** argv) {
    auto& logger = slick::logger::Logger::instance();
    logger.clear_sinks();
    logger.add_console_sink(true, true);
    logger.set_level(slick::logger::LogLevel::L_ERROR);
    logger.init();

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
