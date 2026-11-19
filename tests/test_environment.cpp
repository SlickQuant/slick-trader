// The config's environment: what counts as production, so the dashboard flags everything else.

#include <gtest/gtest.h>
#include <slick-trader/core/environment.hpp>
#include <string>

using namespace slick::trader;
using json = nlohmann::json;

TEST(EnvironmentTest, AConfigWithoutOneIsProduction) {
    std::string env = "stale";
    ASSERT_TRUE(read_environment(json::object(), env));
    EXPECT_EQ(env, PRODUCTION_ENVIRONMENT);
    ASSERT_TRUE(read_environment(json{{"environment", "Sim"}}, env));
    EXPECT_EQ(env, "Sim");
}

// "environment": 1 threw json::type_error out of TradingEngine::initialize(), which nothing caught.
// Now it stops the start with an error, rather than run as production with no bar.
TEST(EnvironmentTest, OneThatIsNotAStringIsRefused) {
    for (const auto& value : {json(1), json(true), json(nullptr), json::array({"Sim"}), json{{"name", "Sim"}}}) {
        std::string env = "untouched";
        EXPECT_FALSE(read_environment(json{{"environment", value}}, env)) << value.dump();
        EXPECT_EQ(env, "untouched");
    }
}

TEST(EnvironmentTest, ProductionIsUnflaggedInAnyCase) {
    EXPECT_TRUE(is_production_environment(PRODUCTION_ENVIRONMENT));
    EXPECT_TRUE(is_production_environment("production"));
    EXPECT_TRUE(is_production_environment("prod"));
    EXPECT_TRUE(is_production_environment("PROD"));
    EXPECT_TRUE(is_production_environment(""));   // "environment": "" is the same as none
}

// A misspelling shows the bar: hiding a sim or cert session is the worse mistake.
TEST(EnvironmentTest, AnythingElseIsFlagged) {
    for (const char* env : {"Sim", "Cert", "NewRelease", "UAT", "Prodution", "production ", "products"}) {
        EXPECT_FALSE(is_production_environment(env)) << env;
    }
}
