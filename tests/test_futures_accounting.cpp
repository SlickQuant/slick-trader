// Futures support in position, PnL, fee and risk accounting.
//
// Regressions covered here:
//   - PositionTracker skipped every fill at a price <= 0 and PnLCalculator ignored such marks,
//     so a calendar spread trading through zero never moved its position or PnL.
//   - to_string(BrokerId::HYPERLIQUID) returned "HYPERLIMIT", so a broker id written out by
//     name could not be read back with to_broker_id().
// And the contract multiplier: without it, ES PnL and order notional are 50x too small.

#include <gtest/gtest.h>
#include <slick-trader/core/pnl_calculator.hpp>
#include <slick-trader/core/position_tracker.hpp>
#include <slick-trader/core/risk_manager.hpp>
#include <slick-trader/events/instrument_definition_event.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick-trader/utils/fee_structure.hpp>
#include <slick-trader/utils/order_types.hpp>
#include <limits>
#include <string>

using namespace slick::trader;

namespace {

constexpr const char* STRATEGY = "FuturesTest";

OrderFillEvent make_fill(const char* instrument, Side side, double price, double qty, double fee = 0.0) {
    OrderFillEvent ev;
    ev.set_instrument(instrument);
    ev.side = side;
    ev.fill_price = to_price_t(price);
    ev.fill_quantity = to_qty_t(qty);
    ev.fee_amount = to_price_t(fee);
    return ev;
}

} // namespace

// ── Contract multiplier ──────────────────────────────────────────────────────

TEST(FuturesAccountingTest, RealizedPnlAppliesContractMultiplier) {
    PositionTracker tracker;
    ASSERT_TRUE(tracker.set_contract_multiplier("ESZ6", 50.0));

    tracker.on_fill(STRATEGY, make_fill("ESZ6", Side::BUY, 5000.00, 1));
    tracker.on_fill(STRATEGY, make_fill("ESZ6", Side::SELL, 5001.25, 1));

    // 1.25 points x 1 contract x $50 = $62.50
    const auto pos = tracker.get_position(STRATEGY, "ESZ6");
    EXPECT_TRUE(pos.is_flat());
    EXPECT_DOUBLE_EQ(from_price_t(pos.realized_pnl), 62.5);
}

TEST(FuturesAccountingTest, UnrealizedPnlAppliesContractMultiplier) {
    PositionTracker tracker;
    tracker.set_contract_multiplier("ESZ6", 50.0);
    tracker.on_fill(STRATEGY, make_fill("ESZ6", Side::BUY, 5000.00, 2));

    // -0.25 points x 2 contracts x $50 = -$25
    const auto pos = tracker.get_position(STRATEGY, "ESZ6");
    EXPECT_DOUBLE_EQ(pos.get_unrealized_pnl(to_price_t(4999.75)), -25.0);
}

TEST(FuturesAccountingTest, MultiplierSetLaterRevaluesHeldPositions) {
    PositionTracker tracker;
    tracker.on_fill(STRATEGY, make_fill("NQZ6", Side::SELL, 20000.0, 1));
    EXPECT_DOUBLE_EQ(tracker.get_position(STRATEGY, "NQZ6").multiplier, 1.0);

    tracker.set_contract_multiplier("NQZ6", 20.0);
    const auto pos = tracker.get_position(STRATEGY, "NQZ6");
    EXPECT_DOUBLE_EQ(pos.multiplier, 20.0);
    // Short 1 from 20000, mark 19990: +10 points x $20 = $200
    EXPECT_DOUBLE_EQ(pos.get_unrealized_pnl(to_price_t(19990.0)), 200.0);
}

TEST(FuturesAccountingTest, SpotInstrumentsKeepMultiplierOfOne) {
    PositionTracker tracker;
    EXPECT_DOUBLE_EQ(tracker.get_contract_multiplier("BTC-USD"), 1.0);

    tracker.on_fill(STRATEGY, make_fill("BTC-USD", Side::BUY, 60000.0, 0.5));
    tracker.on_fill(STRATEGY, make_fill("BTC-USD", Side::SELL, 61000.0, 0.5));
    EXPECT_DOUBLE_EQ(from_price_t(tracker.get_position(STRATEGY, "BTC-USD").realized_pnl), 500.0);
}

TEST(FuturesAccountingTest, InvalidMultiplierIsRejected) {
    PositionTracker tracker;
    EXPECT_FALSE(tracker.set_contract_multiplier("ESZ6", 0.0));
    EXPECT_FALSE(tracker.set_contract_multiplier("ESZ6", -50.0));
    EXPECT_FALSE(tracker.set_contract_multiplier("ESZ6", std::numeric_limits<double>::infinity()));
    EXPECT_DOUBLE_EQ(tracker.get_contract_multiplier("ESZ6"), 1.0);
}

// ── Zero and negative prices (regression) ────────────────────────────────────

TEST(FuturesAccountingTest, SpreadFillsThroughZeroAreAccounted) {
    PositionTracker tracker;
    tracker.set_contract_multiplier("CLZ6-CLF7", 1000.0);

    // Bought the spread at -0.50, sold it at +0.25: 0.75 x 1 x $1000 = $750
    tracker.on_fill(STRATEGY, make_fill("CLZ6-CLF7", Side::BUY, -0.50, 1));
    EXPECT_EQ(tracker.get_position(STRATEGY, "CLZ6-CLF7").quantity, to_qty_t(1));
    EXPECT_EQ(tracker.get_position(STRATEGY, "CLZ6-CLF7").avg_price, to_price_t(-0.50));

    tracker.on_fill(STRATEGY, make_fill("CLZ6-CLF7", Side::SELL, 0.25, 1));
    const auto pos = tracker.get_position(STRATEGY, "CLZ6-CLF7");
    EXPECT_TRUE(pos.is_flat());
    EXPECT_DOUBLE_EQ(from_price_t(pos.realized_pnl), 750.0);
}

TEST(FuturesAccountingTest, FillAtZeroPriceIsAccounted) {
    PositionTracker tracker;
    tracker.on_fill(STRATEGY, make_fill("CLZ6-CLF7", Side::SELL, 0.0, 2));
    EXPECT_EQ(tracker.get_position(STRATEGY, "CLZ6-CLF7").quantity, to_qty_t(-2));
}

TEST(FuturesAccountingTest, FillWithoutPriceIsStillSkipped) {
    PositionTracker tracker;
    auto fill = make_fill("ESZ6", Side::BUY, 0.0, 1);
    fill.fill_price = NULL_PRICE;
    tracker.on_fill(STRATEGY, fill);
    EXPECT_TRUE(tracker.get_position(STRATEGY, "ESZ6").is_flat());
}

TEST(FuturesAccountingTest, NegativeMarkPriceValuesPositions) {
    PositionTracker tracker;
    tracker.set_contract_multiplier("CLZ6-CLF7", 1000.0);
    PnLCalculator pnl(tracker);

    tracker.on_fill(STRATEGY, make_fill("CLZ6-CLF7", Side::BUY, -0.50, 1));
    pnl.update_top_of_book("CLZ6-CLF7", to_price_t(-0.40), to_price_t(-0.35), 0);
    ASSERT_TRUE(pnl.has_mark_price("CLZ6-CLF7"));

    // Long marks at the bid: (-0.40 - -0.50) x 1 x $1000 = $100
    const auto snapshot = pnl.calculate_pnl(STRATEGY, 0);
    EXPECT_DOUBLE_EQ(snapshot.get_unrealized(), 100.0);
}

TEST(FuturesAccountingTest, MarkPriceSentinelsAreStillRejected) {
    PositionTracker tracker;
    PnLCalculator pnl(tracker);
    pnl.update_mark_price("ESZ6", NULL_PRICE, 0);
    pnl.update_mark_price("NQZ6", NULL_PRICE / 10, 0);
    pnl.update_mark_price("RTYZ6", -(NULL_PRICE / 10), 0);
    EXPECT_FALSE(pnl.has_mark_price("ESZ6"));
    EXPECT_FALSE(pnl.has_mark_price("NQZ6"));
    EXPECT_FALSE(pnl.has_mark_price("RTYZ6"));

    pnl.update_mark_price("CLZ6", to_price_t(-37.63), 0);
    EXPECT_EQ(pnl.get_mark_price("CLZ6"), to_price_t(-37.63));
}

// ── Fees ─────────────────────────────────────────────────────────────────────

TEST(FuturesAccountingTest, PerUnitFeeIsPerContract) {
    const FeeStructure fee{FeeType::PER_UNIT, 1.18};
    EXPECT_DOUBLE_EQ(from_price_t(compute_fee(to_price_t(5000.0), to_qty_t(3), fee)), 3.54);
    EXPECT_DOUBLE_EQ(from_price_t(compute_fee(to_price_t(-0.5), to_qty_t(-3), fee)), 3.54);
}

TEST(FuturesAccountingTest, PerUnitFallbackFeeReachesPositions) {
    PositionTracker tracker;
    tracker.set_fee_structure(FeeStructure{FeeType::PER_UNIT, 1.18});
    tracker.on_fill(STRATEGY, make_fill("ESZ6", Side::BUY, 5000.0, 2));
    EXPECT_DOUBLE_EQ(from_price_t(tracker.get_position(STRATEGY, "ESZ6").open_fees), 2.36);
}

// ── Risk ─────────────────────────────────────────────────────────────────────

TEST(FuturesAccountingTest, OrderValueLimitAppliesContractMultiplier) {
    PositionTracker tracker;
    RiskManager risk(tracker);
    RiskLimits limits;
    limits.max_order_value = to_price_t(100000.0);
    risk.set_limits(STRATEGY, limits);

    // Unit multiplier: 1 x 5000 = $5,000 notional - within the limit.
    std::string reason;
    EXPECT_TRUE(risk.validate_order(STRATEGY, "ESZ6", Side::BUY, to_qty_t(1), to_price_t(5000.0), &reason)) << reason;

    // ES: 1 x 5000 x $50 = $250,000 notional - over it.
    tracker.set_contract_multiplier("ESZ6", 50.0);
    EXPECT_FALSE(risk.validate_order(STRATEGY, "ESZ6", Side::BUY, to_qty_t(1), to_price_t(5000.0), &reason));
    EXPECT_NE(reason.find("Order value limit exceeded"), std::string::npos) << reason;
}

TEST(FuturesAccountingTest, OrderValueOfNegativePriceIsItsMagnitude) {
    PositionTracker tracker;
    tracker.set_contract_multiplier("CLZ6-CLF7", 1000.0);
    RiskManager risk(tracker);
    RiskLimits limits;
    limits.max_order_value = to_price_t(10000.0);
    risk.set_limits(STRATEGY, limits);

    // 20 x -0.75 x $1000 = $15,000 of notional, however the sign falls.
    std::string reason;
    EXPECT_FALSE(risk.validate_order(STRATEGY, "CLZ6-CLF7", Side::BUY, to_qty_t(20), to_price_t(-0.75), &reason));
}

// ── Enums the web API and configs depend on ──────────────────────────────────

TEST(FuturesAccountingTest, BrokerIdNamesRoundTrip) {
    for (auto id : {BrokerId::COINBASE, BrokerId::ALPACA, BrokerId::HYPERLIQUID, BrokerId::SIMULATED, BrokerId::CME}) {
        EXPECT_EQ(to_broker_id(to_string(id)), id) << to_string(id);
    }
    EXPECT_STREQ(to_string(BrokerId::HYPERLIQUID), "HYPERLIQUID");
    EXPECT_STREQ(to_string(BrokerId::CME), "CME");
}

TEST(FuturesAccountingTest, EnumValuesOnTheWireAreStable) {
    // The web order API and the dashboard send these as raw numbers.
    EXPECT_EQ(static_cast<int>(BrokerId::COINBASE), 1);
    EXPECT_EQ(static_cast<int>(BrokerId::HYPERLIQUID), 3);
    EXPECT_EQ(static_cast<int>(BrokerId::SIMULATED), 4);
    EXPECT_EQ(static_cast<int>(BrokerId::CME), 5);
    EXPECT_EQ(static_cast<int>(TimeInForce::GTC), 0);
    EXPECT_EQ(static_cast<int>(TimeInForce::IOC), 1);
    EXPECT_EQ(static_cast<int>(TimeInForce::FOK), 2);
    EXPECT_EQ(static_cast<int>(TimeInForce::GTD), 3);
    EXPECT_EQ(static_cast<int>(TimeInForce::DAY), 4);
    EXPECT_EQ(to_string(TimeInForce::DAY), "DAY");
}

TEST(FuturesAccountingTest, InstrumentDefinitionDefaults) {
    InstrumentDefinitionEvent ev;
    EXPECT_EQ(ev.type, EventType::INSTRUMENT_DEFINITION);
    EXPECT_DOUBLE_EQ(ev.contract_multiplier, 1.0);
    ev.set_currency("USD");
    EXPECT_STREQ(ev.currency, "USD");
    ev.set_currency("TOO-LONG-CODE");
    EXPECT_EQ(std::string(ev.currency).size(), MAX_CURRENCY_LENGTH - 1);
}
