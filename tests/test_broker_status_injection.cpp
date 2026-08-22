// Regression test: a strategy plugin must observe the HOST's BrokerStatusRegistry.
//
// Bug: slick-trader-core is a static library and the host exports no symbols, so a
// plugin DLL linked against it got its own function-local `instance()` static. Every
// set_order_enabled/set_order_connected call lives in the host's broker connectors, so
// the plugin's copy stayed all-false forever and StrategyBase::submit_order() rejected
// every order with "Broker order connector disconnected". Orders placed from inside the
// host (the MANUAL web-UI strategy) worked, which is what made it hard to spot.

#include <gtest/gtest.h>
#include <slick-trader/core/broker_status.hpp>
#include <slick-trader/core/strategy_loader.hpp>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

using namespace slick::trader;

namespace {

using ProbeFunc = bool (*)(int);

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
        }
#else
        handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
        if (handle) {
            enabled   = reinterpret_cast<ProbeFunc>(dlsym(handle, "probe_is_order_enabled"));
            connected = reinterpret_cast<ProbeFunc>(dlsym(handle, "probe_is_order_connected"));
        }
#endif
    }

    void*     handle{nullptr};
    ProbeFunc enabled{nullptr};
    ProbeFunc connected{nullptr};
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
    // sees a dead broker even though the host has it enabled and connected.
    EXPECT_FALSE(probe.enabled(kCoinbase));
    EXPECT_FALSE(probe.connected(kCoinbase));

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
