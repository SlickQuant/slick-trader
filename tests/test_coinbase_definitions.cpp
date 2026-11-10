// Coinbase published no instrument definitions, so a ladder, which lists what brokers define, had
// none of its products. Its connector defines each product it can trade, from the REST listing.

#include <gtest/gtest.h>

// The SDK first, as the connector's precompiled header has it.
#include <coinbase/product.hpp>
#include <coinbase/websocket.hpp>
#include <slick-trader/brokers/coinbase/coinbase_md_connector.hpp>

#include <vector>

using namespace slick::trader;

namespace {

coinbase::Product product(const char* id) {
    coinbase::Product p{};
    p.product_id = id;
    p.quote_currency_id = "USD";
    p.price_increment = 0.01;
    p.quote_increment = 0.01;
    p.base_increment = 0.00000001;
    p.base_min_size = 0.0001;
    p.base_max_size = 100;
    return p;
}

} // namespace

TEST(CoinbaseDefinitionsTest, EachTradableProductIsDefined) {
    std::vector<coinbase::Product> products{product("BTC-USD"), product("ETH-USD"), product("OFF-USD"),
                                            product("HALT-USD"), product("VIEW-USD")};
    products[1].price_increment = 0;   // the quote increment stands in
    products[1].quote_increment = 0.05;
    products[2].is_disabled = true;
    products[3].trading_disabled = true;
    products[4].view_only = true;

    const auto definitions = coinbase_definitions(products);
    ASSERT_EQ(definitions.size(), 2u);
    EXPECT_EQ(definitions[0].symbol, "BTC-USD");
    EXPECT_EQ(definitions[0].tick_size, to_price_t(0.01));
    EXPECT_EQ(definitions[0].quantity_increment, to_qty_t(0.00000001));
    EXPECT_EQ(definitions[0].min_quantity, to_qty_t(0.0001));
    EXPECT_EQ(definitions[0].max_quantity, to_qty_t(100));
    EXPECT_EQ(definitions[0].currency, "USD");
    EXPECT_EQ(definitions[0].multiplier, 1.0);
    EXPECT_EQ(definitions[1].symbol, "ETH-USD");
    EXPECT_EQ(definitions[1].tick_size, to_price_t(0.05));
}
