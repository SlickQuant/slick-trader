// Regression test: every broker built into the binary must be found by its config name.
//
// Brokers register themselves from a namespace-scope initialiser that nothing in the
// core references by name. Built into a static library, as they used to be, the linker
// drops that object and the broker silently disappears from BrokerRegistry - startup
// then fails with "No broker adapter" for a broker that was compiled. Each broker is an
// OBJECT library (cmake/slick_trader_broker.cmake) to prevent exactly that.

#include <gtest/gtest.h>
#include <slick-trader/core/broker_registry.hpp>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <memory>

using namespace slick::trader;

TEST(BrokerRegistryTest, BuiltInBrokersAreRegistered) {
#ifdef SLICK_TRADER_HAS_COINBASE
    EXPECT_NE(BrokerRegistry::instance().find("coinbase"), nullptr)
        << "Coinbase was built but did not register - is its target still an OBJECT library?";
#endif
#ifdef SLICK_TRADER_HAS_HYPERLIQUID
    EXPECT_NE(BrokerRegistry::instance().find("hyperliquid"), nullptr)
        << "Hyperliquid was built but did not register - is its target still an OBJECT library?";
#endif
}

TEST(BrokerRegistryTest, LookupIsCaseInsensitive) {
    for (const auto& key : BrokerRegistry::instance().keys()) {
        std::string upper = key;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        EXPECT_EQ(BrokerRegistry::instance().find(upper), BrokerRegistry::instance().find(key)) << key;
    }
}

TEST(BrokerRegistryTest, UnknownBrokerIsNotFound) {
    EXPECT_EQ(BrokerRegistry::instance().find("no-such-broker"), nullptr);
    EXPECT_EQ(BrokerRegistry::instance().find(""), nullptr);
}

// Register a broker for this test so it also works with SLICK_TRADER_ENABLE_*=OFF.
TEST(BrokerRegistryTest, DuplicateRegistrationKeepsTheFirst) {
    auto first = [](const json&, slick::stream_buffer_multiplexer&, slick::queue<uint8_t>&,
                    slick::queue<uint8_t>&, slick::queue<uint8_t>&) -> std::shared_ptr<IBroker> {
        return nullptr;
    };
    auto impostor = [](const json&, slick::stream_buffer_multiplexer&, slick::queue<uint8_t>&,
                       slick::queue<uint8_t>&, slick::queue<uint8_t>&) -> std::shared_ptr<IBroker> {
        return nullptr;
    };
    BrokerRegistry::instance().add("registry-test-duplicate", first);
    const auto keys = BrokerRegistry::instance().keys();
    const auto original = BrokerRegistry::instance().find("registry-test-duplicate");
    ASSERT_NE(original, nullptr);

    EXPECT_FALSE(BrokerRegistry::instance().add("registry-test-duplicate", impostor));
    EXPECT_EQ(BrokerRegistry::instance().find("registry-test-duplicate"), original);
    EXPECT_EQ(BrokerRegistry::instance().keys().size(), keys.size());
}

// Regression: a broker rejects a config it cannot run with by throwing from its
// constructor. TradingEngine called the factory bare, so the exception left
// initialize() unhandled - in a Debug build an abort() dialog that looked like a hang,
// in Release a crash - and the reason never reached the log.
TEST(BrokerRegistryTest, ABrokerWhoseConstructorThrowsIsReportedNotThrown) {
    auto throwing = [](const json&, slick::stream_buffer_multiplexer&, slick::queue<uint8_t>&,
                       slick::queue<uint8_t>&, slick::queue<uint8_t>&) -> std::shared_ptr<IBroker> {
        throw std::runtime_error("\"sessions\" must list at least one session");
    };
    BrokerRegistry::instance().add("registry-test-throwing", throwing);

    slick::stream_buffer_multiplexer mux(64);
    slick::queue<uint8_t> control(1024), md(1024), orders(1024);
    std::string error;
    std::shared_ptr<IBroker> broker;
    EXPECT_NO_THROW(broker = BrokerRegistry::instance().create("registry-test-throwing", json::object(), mux, control,
                                                              md, orders, error));
    EXPECT_EQ(broker, nullptr);
    EXPECT_EQ(error, "\"sessions\" must list at least one session");

    error.clear();
    EXPECT_EQ(BrokerRegistry::instance().create("no-such-broker", json::object(), mux, control, md, orders, error),
              nullptr);
    EXPECT_NE(error.find("no-such-broker"), std::string::npos);
}

TEST(BrokerRegistryTest, KeysAreListedLowercase) {
    for (const auto& key : BrokerRegistry::instance().keys()) {
        EXPECT_TRUE(std::none_of(key.begin(), key.end(), [](unsigned char c) { return std::isupper(c); }))
            << key;
        EXPECT_NE(BrokerRegistry::instance().find(key), nullptr) << key;
    }
}
