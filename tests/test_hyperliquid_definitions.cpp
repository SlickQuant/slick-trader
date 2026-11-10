// Hyperliquid published no instrument definitions, so a ladder, which lists what brokers define, had
// none of its coins. Its connector defines each perpetual in the metadata it loads.

#include <gtest/gtest.h>

#include <slick-trader/brokers/hyperliquid/hyperliquid_md_connector.hpp>

#include <string>
#include <unordered_map>

using namespace slick::trader;

TEST(HyperliquidDefinitionsTest, EachPerpIsDefinedWithItsSizeStep) {
    const std::unordered_map<std::string, int> coin_to_asset{{"ETH", 1}, {"BTC", 0}, {"@0", 10000}};
    const std::unordered_map<int, int> sz_decimals{{0, 5}, {1, 4}, {10000, 2}};

    const auto definitions = hyperliquid_definitions(coin_to_asset, sz_decimals);
    ASSERT_EQ(definitions.size(), 2u) << "a spot market was defined";
    EXPECT_EQ(definitions[0].symbol, "BTC");
    EXPECT_EQ(definitions[0].quantity_increment, to_qty_t(0.00001));
    EXPECT_EQ(definitions[0].tick_size, 0) << "a perp's tick depends on its price";
    EXPECT_EQ(definitions[0].currency, "USDC");
    EXPECT_EQ(definitions[1].symbol, "ETH");
    EXPECT_EQ(definitions[1].quantity_increment, to_qty_t(0.0001));
}
