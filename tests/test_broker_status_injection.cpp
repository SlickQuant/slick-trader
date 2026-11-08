// Regression test: a strategy plugin must observe the HOST's BrokerStatusRegistry.
//
// Bug: slick-trader-core is a static library and the host exports no symbols, so a
// plugin DLL linked against it got its own function-local `instance()` static. Every
// set_order_enabled/set_order_connected call lives in the host's broker connectors, so
// the plugin's copy stayed all-false forever and StrategyBase::submit_order() rejected
// every order with "Broker order connector disconnected". Orders placed from inside the
// host (the MANUAL web-UI strategy) worked, which is what made it hard to spot.

#include "strategy_manager_fixture.hpp"

#include <gtest/gtest.h>
#include <slick-trader/core/broker_status.hpp>
#include <slick-trader/core/strategy_loader.hpp>
#include <slick-trader/core/strategy_manager.hpp>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

using namespace slick::trader;

namespace {

using ProbeFunc = bool (*)(int);
using InitializedFunc = bool (*)();

// Opens the already-loaded plugin a second time purely to reach its probe exports.
// On Windows this bumps the refcount and returns the same HMODULE, so the statics we
// are inspecting are the ones StrategyLoader talked to.
struct PluginProbe {
    explicit PluginProbe(const char* path) {
#ifdef _WIN32
        handle = LoadLibraryA(path);
        if (handle) {
            enabled   = reinterpret_cast<ProbeFunc>(GetProcAddress(static_cast<HMODULE>(handle), "probe_is_order_enabled"));
            connected = reinterpret_cast<ProbeFunc>(GetProcAddress(static_cast<HMODULE>(handle), "probe_is_order_connected"));
            initialized = reinterpret_cast<InitializedFunc>(GetProcAddress(static_cast<HMODULE>(handle), "probe_initialized"));
        }
#else
        handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
        if (handle) {
            enabled   = reinterpret_cast<ProbeFunc>(dlsym(handle, "probe_is_order_enabled"));
            connected = reinterpret_cast<ProbeFunc>(dlsym(handle, "probe_is_order_connected"));
            initialized = reinterpret_cast<InitializedFunc>(dlsym(handle, "probe_initialized"));
        }
#endif
    }

    void*     handle{nullptr};
    ProbeFunc enabled{nullptr};
    ProbeFunc connected{nullptr};
    InitializedFunc initialized{nullptr};
};

constexpr int kCoinbase = static_cast<int>(BrokerId::COINBASE);

} // namespace

TEST(BrokerStatusInjection, StrategyLoaderRedirectsPluginRegistryAtHost) {
    auto& host = BrokerStatusRegistry::instance();
    host.set_name(BrokerId::COINBASE, "Coinbase");
    host.set_order_enabled(BrokerId::COINBASE, true);
    host.set_order_connected(BrokerId::COINBASE, true);
    ASSERT_TRUE(host.is_order_connected(BrokerId::COINBASE));

    PluginProbe probe(SLICK_TRADER_TEST_PLUGIN_PATH);
    ASSERT_NE(probe.handle, nullptr) << "failed to open " << SLICK_TRADER_TEST_PLUGIN_PATH;
    ASSERT_NE(probe.enabled, nullptr);
    ASSERT_NE(probe.connected, nullptr);

    // The bug, reproduced: opened on its own the plugin reads its private registry and
    // sees a dead broker even though the host has it enabled and connected. Not on macOS:
    // dyld coalesces the weak definitions of instance()'s statics across the executable
    // and the plugins it loads, so there the plugin shares the host's registry from the
    // start - and what follows still has to hold.
#ifdef __APPLE__
    EXPECT_TRUE(probe.enabled(kCoinbase));
    EXPECT_TRUE(probe.connected(kCoinbase));
#else
    EXPECT_FALSE(probe.enabled(kCoinbase));
    EXPECT_FALSE(probe.connected(kCoinbase));
#endif

    // Loading it the way the engine does must hand it the host's registry.
    StrategyLoader loader;
    auto strategy = loader.load(SLICK_TRADER_TEST_PLUGIN_PATH);
    ASSERT_NE(strategy, nullptr);

    EXPECT_TRUE(probe.enabled(kCoinbase));
    EXPECT_TRUE(probe.connected(kCoinbase));

    // Aliased, not copied: later broker state changes must reach the plugin too.
    host.set_order_connected(BrokerId::COINBASE, false);
    EXPECT_FALSE(probe.connected(kCoinbase));
    EXPECT_TRUE(probe.enabled(kCoinbase));

    host.set_order_connected(BrokerId::COINBASE, true);
    EXPECT_TRUE(probe.connected(kCoinbase));
}

TEST(BrokerStatusInjection, HostKeepsItsOwnRegistryWhenNothingIsInjected) {
    // set_instance() is never called in the host, so instance() has to keep returning
    // the module-local object the broker connectors write to.
    auto& a = BrokerStatusRegistry::instance();
    auto& b = BrokerStatusRegistry::instance();
    EXPECT_EQ(&a, &b);

    a.set_order_enabled(BrokerId::SIMULATED, true);
    EXPECT_TRUE(b.is_order_enabled(BrokerId::SIMULATED));
    a.set_order_enabled(BrokerId::SIMULATED, false);
    EXPECT_FALSE(b.is_order_enabled(BrokerId::SIMULATED));
}

// The order dialog listed Coinbase, Alpaca and Simulated whatever the engine had built. It now
// lists the brokers the registry knows, and the web server turns an order down for the same
// reasons submit_order() does, so the caller hears why.
TEST(BrokerStatusInjection, TheRegistryKnowsItsBrokersAndWhyOneTakesNoOrder) {
    auto& reg = BrokerStatusRegistry::instance();
    constexpr BrokerId id = BrokerId::ALPACA;   // no connector in this build makes one
    ASSERT_FALSE(reg.is_registered(id));

    reg.set_name(id, "Alpaca");
    EXPECT_TRUE(reg.is_registered(id));
    EXPECT_STREQ(reg.order_unavailable_reason(id), "Broker order connector disabled");
    reg.set_order_enabled(id, true);
    EXPECT_STREQ(reg.order_unavailable_reason(id), "Broker order connector disconnected");
    reg.set_order_connected(id, true);
    EXPECT_EQ(reg.order_unavailable_reason(id), nullptr);

    reg.set_order_connected(id, false);
    reg.set_order_enabled(id, false);
    reg.set_name(id, "");
    EXPECT_FALSE(reg.is_registered(id));
}

// The strategy manager finds plugins by their file extension, and that has to be the one
// CMake gives a shared library here: macOS names them .dylib, and a manager that looked
// for .so there loaded no strategy at all.
TEST(BrokerStatusInjection, ThePluginExtensionIsTheOneThePlatformBuilds) {
    EXPECT_EQ(std::filesystem::path(SLICK_TRADER_TEST_PLUGIN_PATH).extension().string(),
              StrategyManager::PLUGIN_EXTENSION);
}

// A plugin built before IStrategy::on_order_book named its instrument was loaded all the same:
// the host called it through its new vtable, and indexed BrokerStatus by another layout. The
// loader now takes only a plugin built against its own STRATEGY_API_VERSION.
TEST(BrokerStatusInjection, ThePluginOfAnotherStrategyApiIsRefused) {
    StrategyLoader loader;
    EXPECT_EQ(loader.load(SLICK_TRADER_TEST_STALE_PLUGIN_PATH), nullptr);
    EXPECT_NE(loader.get_last_error().find("Rebuild it"), std::string::npos) << loader.get_last_error();

    StrategyLoader current;
    EXPECT_NE(current.load(SLICK_TRADER_TEST_PLUGIN_PATH), nullptr) << current.get_last_error();
}

// A plugin whose config file gave it no "parameters" was never initialize()d: load_strategies()
// passed a config's parameters only when there were some. It kept a null risk manager and order
// queue, and its first order dereferenced them.
TEST(BrokerStatusInjection, APluginWhoseConfigHasNoParametersIsInitialized) {
    namespace fs = std::filesystem;
    // A copy of its own, a module apart from the one the tests above inspect. Copied only when
    // newer: a run repeated in this process has the last copy loaded still.
    const fs::path dir = fs::temp_directory_path() / "slick-trader-plugin-init-test";
    fs::create_directories(dir);
    const fs::path plugin = dir / fs::path(SLICK_TRADER_TEST_PLUGIN_PATH).filename();
    fs::copy_file(SLICK_TRADER_TEST_PLUGIN_PATH, plugin, fs::copy_options::update_existing);
    std::ofstream(dir / "probe.config.json") << R"({"name": "Probe"})";

    test::StrategyManagerFixture env;
    env.manager_.load_strategies(nlohmann::json{{"strategy_plugins_folder", dir.string()}});
    ASSERT_NE(env.manager_.get_strategy("Probe"), nullptr);

    PluginProbe probe(plugin.string().c_str());
    ASSERT_NE(probe.initialized, nullptr) << "failed to open " << plugin;
    EXPECT_TRUE(probe.initialized());
}
