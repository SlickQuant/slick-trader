#pragma once

#include <slick-trader/strategy/istrategy.hpp>
#include <slick/logger.hpp>
#include <memory>
#include <string>

// #ifdef _WIN32
//     #define SLICK_TRADER_EXPORT __declspec(dllexport)
//     #define SLICK_TRADER_IMPORT __declspec(dllimport)
// #else
//     #define SLICK_TRADER_EXPORT __attribute__((visibility("default")))
//     #define SLICK_TRADER_IMPORT
// #endif

// // Macro for strategy plugin authors to export their strategy
// // Usage in strategy .cpp:
// //   SLICK_TRADER_EXPORT_STRATEGY(MyStrategy)
// #define SLICK_TRADER_EXPORT_STRATEGY(StrategyClass) \
//     extern "C" { \
//         SLICK_TRADER_EXPORT slick::trader::IStrategy* create_strategy() { \
//             return new StrategyClass(); \
//         } \
//         SLICK_TRADER_EXPORT void destroy_strategy(slick::trader::IStrategy* strategy) { \
//             delete strategy; \
//         } \
//         SLICK_TRADER_EXPORT const char* get_strategy_name() { \
//             static StrategyClass instance; \
//             return instance.name(); \
//         } \
//         SLICK_TRADER_EXPORT const char* get_strategy_version() { \
//             static StrategyClass instance; \
//             return instance.version(); \
//         } \
//     }

namespace slick::trader {

// Strategy loader for dynamic library loading
// Loads .so/.dll strategy plugins at runtime
class StrategyLoader {
public:
    StrategyLoader() = default;
    ~StrategyLoader();

    // Non-copyable, non-movable
    StrategyLoader(const StrategyLoader&) = delete;
    StrategyLoader& operator=(const StrategyLoader&) = delete;
    StrategyLoader(StrategyLoader&&) = delete;
    StrategyLoader& operator=(StrategyLoader&&) = delete;

    // Load strategy from shared library
    // Returns nullptr on failure
    std::shared_ptr<IStrategy> load(const std::string& library_path);

    // Unload a previously loaded strategy
    void unload();

    // Check if a strategy is currently loaded
    bool is_loaded() const { return handle_ != nullptr; }

    // Get library path
    const std::string& get_library_path() const { return library_path_; }

    // Get last error message
    const std::string& get_last_error() const { return last_error_; }

private:
    using CreateStrategyFunc = IStrategy* (*)();
    using DestroyStrategyFunc = void (*)(IStrategy*);
    using GetNameFunc = const char* (*)();
    using GetVersionFunc = const char* (*)();
    using SetLoggerFunc = void (*)(slick::logger::Logger*);

    void* handle_{nullptr};
    std::string library_path_;
    std::string last_error_;

    CreateStrategyFunc create_func_{nullptr};
    DestroyStrategyFunc destroy_func_{nullptr};

    // Custom deleter for shared_ptr that calls destroy_strategy()
    // Also keeps the DLL handle alive as long as the strategy exists
    struct StrategyDeleter {
        DestroyStrategyFunc destroy_func;
        void* dll_handle;  // Keep DLL loaded

        void operator()(IStrategy* strategy) const {
            if (destroy_func && strategy) {
                destroy_func(strategy);
            }
            // Note: We intentionally don't unload dll_handle here
            // The DLL will be unloaded when the process exits
            // This prevents crashes when destructors are called during program shutdown
        }
    };
};

} // namespace slick::trader
