// PriceScale: exact conversion between an exchange's integer wire prices and price_t.

#include <gtest/gtest.h>
#include <slick-trader/brokers/common/price_scale.hpp>
#include <limits>

using namespace slick::trader;

TEST(PriceScaleTest, ConvertsBothWaysExactly) {
    // A price quoted in hundredths on the wire: 7523 is 75.23.
    const auto cents = PriceScale::from_ratio(PRICE_SCALE, 100);
    ASSERT_TRUE(cents);
    price_t price = 0;
    ASSERT_TRUE(cents->to_price(7523, price));
    EXPECT_EQ(price, to_price_t(75.23));
    int64_t wire = 0;
    ASSERT_TRUE(cents->to_wire(price, wire));
    EXPECT_EQ(wire, 7523);

    // A 1e-9 mantissa: price_t is a tenth of it.
    const auto d9 = PriceScale::from_ratio(1, 10);
    ASSERT_TRUE(d9->to_price(75'230'000'000, price));
    EXPECT_EQ(price, to_price_t(75.23));
    ASSERT_TRUE(d9->to_wire(price, wire));
    EXPECT_EQ(wire, 75'230'000'000);
}

TEST(PriceScaleTest, KeepsTheRatioReduced) {
    const auto scale = PriceScale::from_ratio(10'000'000, 10'000'000'000);
    ASSERT_TRUE(scale);
    EXPECT_EQ(scale->numerator(), 1);
    EXPECT_EQ(scale->denominator(), 1000);
}

TEST(PriceScaleTest, NegativePricesConvertBothWays) {
    const auto scale = PriceScale::from_ratio(PRICE_SCALE, 100);
    price_t price = 0;
    ASSERT_TRUE(scale->to_price(-75, price));
    EXPECT_EQ(price, to_price_t(-0.75));
    int64_t wire = 0;
    ASSERT_TRUE(scale->to_wire(price, wire));
    EXPECT_EQ(wire, -75);
}

TEST(PriceScaleTest, RefusesNullsOffScaleAndOverflowRatherThanRounding) {
    const auto d9 = PriceScale::from_ratio(1, 10);
    price_t price = 0;
    EXPECT_FALSE(d9->to_price(75'230'000'001, price)) << "a digit price_t cannot hold";
    EXPECT_FALSE(d9->to_price(std::numeric_limits<int64_t>::max(), price)) << "a null";
    EXPECT_FALSE(d9->to_price(std::numeric_limits<int64_t>::min(), price)) << "the other null";

    int64_t wire = 0;
    EXPECT_FALSE(d9->to_wire(NULL_PRICE, wire));
    const auto coarse = PriceScale::from_ratio(PRICE_SCALE, 100);
    EXPECT_FALSE(coarse->to_wire(to_price_t(75.235), wire)) << "finer than the wire";
    EXPECT_FALSE(PriceScale::from_ratio(1'000'000'000, 1)->to_price(std::numeric_limits<int64_t>::max() / 2, price))
        << "too large to hold";
}

TEST(PriceScaleTest, NonPositiveTermsAreUnusable) {
    EXPECT_FALSE(PriceScale::from_ratio(0, 10));
    EXPECT_FALSE(PriceScale::from_ratio(1, 0));
    EXPECT_FALSE(PriceScale::from_ratio(-1, 10));
}
