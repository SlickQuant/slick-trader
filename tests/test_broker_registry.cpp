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

TEST(BrokerRegistryTest, DuplicateRegistrationKeepsTheFirst) {
    auto keys = BrokerRegistry::instance().keys();
    ASSERT_FALSE(keys.empty()) << "no broker registered at all";
    const auto original = BrokerRegistry::instance().find(keys.front());

    auto impostor = [](const json&, slick::stream_buffer_multiplexer&, slick::queue<uint8_t>&,
                       slick::queue<uint8_t>&, slick::queue<uint8_t>&) -> std::shared_ptr<IBroker> {
        return nullptr;
    };
    EXPECT_FALSE(BrokerRegistry::instance().add(keys.front(), impostor));
    EXPECT_EQ(BrokerRegistry::instance().find(keys.front()), original);
    EXPECT_EQ(BrokerRegistry::instance().keys().size(), keys.size());
}

TEST(BrokerRegistryTest, KeysAreListedLowercase) {
    for (const auto& key : BrokerRegistry::instance().keys()) {
        EXPECT_TRUE(std::none_of(key.begin(), key.end(), [](unsigned char c) { return std::isupper(c); }))
            << key;
        EXPECT_NE(BrokerRegistry::instance().find(key), nullptr) << key;
    }
}
