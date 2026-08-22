#pragma once

#include <slick/logger.hpp>
#include <slick-trader/core/broker_status.hpp>
#include <slick-trader/strategy/istrategy.hpp>

#ifdef _WIN32
    #define SLICK_TRADER_EXPORT __declspec(dllexport)
    #define SLICK_TRADER_IMPORT __declspec(dllimport)
#else
    #define SLICK_TRADER_EXPORT __attribute__((visibility("default")))
    #define SLICK_TRADER_IMPORT
#endif

// Macro for strategy plugin authors to export their strategy
// Usage in strategy .cpp:
//   SLICK_TRADER_EXPORT_STRATEGY(MyStrategy)
//
// The set_* entry points exist because a plugin links slick-trader-core statically
// and therefore owns private copies of the host's singletons. StrategyLoader calls
// them before create_strategy() to redirect the plugin at the host's instances.
#define SLICK_TRADER_EXPORT_STRATEGY(StrategyClass) \
    extern "C" { \
        SLICK_TRADER_EXPORT slick::trader::IStrategy* create_strategy() { \
            return new StrategyClass(); \
        } \
        SLICK_TRADER_EXPORT void destroy_strategy(slick::trader::IStrategy* strategy) { \
            delete strategy; \
        } \
        SLICK_TRADER_EXPORT const char* get_strategy_name() { \
            static StrategyClass instance; \
            return instance.name(); \
        } \
        SLICK_TRADER_EXPORT const char* get_strategy_version() { \
            static StrategyClass instance; \
            return instance.version(); \
        } \
        SLICK_TRADER_EXPORT void set_logger_instance(slick::logger::Logger* logger) { \
            slick::logger::Logger::set_instance(logger); \
        } \
        SLICK_TRADER_EXPORT void set_broker_status_registry(slick::trader::BrokerStatusRegistry* registry) { \
            slick::trader::BrokerStatusRegistry::set_instance(registry); \
        } \
    }
