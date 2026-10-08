// Regression test: a strategy must not adopt another strategy's orders.
//
// Bug: StrategyManager dispatches every order ack/fill/reject/cancel to every strategy,
// and StrategyBase::on_order_ack() adopted any client_order_id it did not recognise
// ("ACK for unknown client_order_id - inserting new entry"). A manual web-UI order
// therefore landed in the MarketMaker's order map, and the subsequent fill was booked
// into the MarketMaker's position and PnL — phantom inventory that then feeds the
// strategy's own inventory limits and quote skew.

#include <gtest/gtest.h>
#include <slick-trader/core/broker_status.hpp>
#include <slick-trader/core/pnl_calculator.hpp>
#include <slick-trader/core/position_tracker.hpp>
#include <slick-trader/core/risk_manager.hpp>
#include <slick-trader/strategy/strategy_base.hpp>
#include <slick/queue.hpp>

#include <string>
#include <vector>

using namespace slick::trader;

namespace {

constexpr const char* kInstrument = "BTC-USD";

// Records which events actually reached the derived hooks.
class RecordingStrategy : public StrategyBase {
public:
    explicit RecordingStrategy(const std::string& name)
        : StrategyBase(name, "1.0.0") {}

    std::vector<std::string> acked;
    std::vector<std::string> filled;
    std::vector<std::string> rejected;
    std::vector<std::string> cancelled;

    using StrategyBase::get_order;
    using StrategyBase::get_orders;
    using StrategyBase::submit_order;

protected:
    void on_ack(const OrderAckEvent& e) override       { acked.emplace_back(e.client_order_id); }
    void on_fill(const OrderFillEvent& e) override     { filled.emplace_back(e.order_id); }
    void on_reject(const OrderRejectEvent& e) override { rejected.emplace_back(e.client_order_id); }
    void on_cancel(const OrderCancelEvent& e) override { cancelled.emplace_back(e.order_id); }
};

OrderAckEvent make_ack(const std::string& client_id, const std::string& order_id) {
    OrderAckEvent ev;
    ev.timestamp_ns = 1;
    ev.set_instrument(kInstrument);
    ev.set_client_order_id(client_id);
    ev.set_order_id(order_id);
    ev.side          = Side::SELL;
    ev.order_type    = OrderType::LIMIT;
    ev.time_in_force = TimeInForce::GTC;
    ev.price         = to_price_t(77950.64);
    ev.quantity      = to_qty_t(0.001);
    ev.status        = OrderStatus::ACKNOWLEDGED;
    return ev;
}

OrderFillEvent make_fill(const std::string& client_id, const std::string& order_id) {
    OrderFillEvent ev;
    ev.timestamp_ns = 2;
    ev.set_instrument(kInstrument);
    ev.set_client_order_id(client_id);
    ev.set_order_id(order_id);
    ev.side               = Side::SELL;
    ev.fill_price         = to_price_t(77951.06);
    ev.fill_quantity      = to_qty_t(0.001);
    ev.cumulative_filled  = to_qty_t(0.001);
    ev.remaining_quantity = 0;
    return ev;
}

class StrategyOwnershipTest : public ::testing::Test {
protected:
    void SetUp() override {
        // submit_order() refuses to enqueue unless the broker looks alive.
        auto& reg = BrokerStatusRegistry::instance();
        reg.set_name(BrokerId::COINBASE, "Coinbase");
        reg.set_order_enabled(BrokerId::COINBASE, true);
        reg.set_order_connected(BrokerId::COINBASE, true);

        for (auto* s : {&market_maker_, &manual_}) {
            s->initialize(nlohmann::json::object(), positions_, pnl_, risk_, md_q_, order_q_);
            s->start();
        }
    }

    PositionTracker positions_;
    PnLCalculator   pnl_{positions_};
    RiskManager     risk_{positions_};
    slick::queue<uint8_t> md_q_{4096};
    slick::queue<uint8_t> order_q_{65536};

    RecordingStrategy market_maker_{"MarketMaker"};
    RecordingStrategy manual_{"MANUAL"};
};

} // namespace

// The exact sequence from the 2026-08-21 log: a MANUAL order acked and filled while
// MarketMaker was also running.
TEST_F(StrategyOwnershipTest, ForeignOrderIsNotAdoptedOrBooked) {
    const std::string oid = "e0906214-ce7b-4109-918f-c4c094c649e2";

    const auto submitted = manual_.submit_order(
        kInstrument, BrokerId::COINBASE, Side::SELL, to_qty_t(0.001),
        OrderType::LIMIT, to_price_t(77950.64), TimeInForce::GTC, false);
    ASSERT_FALSE(submitted.empty()) << "MANUAL order should have been accepted";

    const auto ack  = make_ack(submitted, oid);
    const auto fill = make_fill(submitted, oid);

    // Dispatched to every strategy, exactly as StrategyManager does.
    for (auto* s : {&market_maker_, &manual_}) s->on_order_ack(ack);
    for (auto* s : {&market_maker_, &manual_}) s->on_order_fill(fill);

    // The owner tracks it...
    EXPECT_EQ(manual_.acked.size(), 1u);
    EXPECT_EQ(manual_.filled.size(), 1u);
    EXPECT_NE(manual_.get_order(submitted), nullptr);
    EXPECT_DOUBLE_EQ(from_qty_t(positions_.get_position("MANUAL", kInstrument).quantity), -0.001);

    // ...and the bystander does not.
    EXPECT_TRUE(market_maker_.acked.empty());
    EXPECT_TRUE(market_maker_.filled.empty());
    EXPECT_TRUE(market_maker_.get_orders().empty());
    EXPECT_DOUBLE_EQ(from_qty_t(positions_.get_position("MarketMaker", kInstrument).quantity), 0.0);
}

TEST_F(StrategyOwnershipTest, ForeignRejectAndCancelDoNotReachTheHooks) {
    const std::string cid = "MANUAL_1";
    const std::string oid = "broker-order-1";

    OrderRejectEvent reject;
    reject.timestamp_ns = 1;
    reject.set_instrument(kInstrument);
    reject.set_client_order_id(cid);
    reject.set_order_id(oid);
    reject.set_reason("insufficient funds");

    OrderCancelEvent cancel;
    cancel.timestamp_ns = 2;
    cancel.set_instrument(kInstrument);
    cancel.set_client_order_id(cid);
    cancel.set_order_id(oid);

    market_maker_.on_order_reject(reject);
    market_maker_.on_order_cancel(cancel);

    EXPECT_TRUE(market_maker_.rejected.empty());
    EXPECT_TRUE(market_maker_.cancelled.empty());
}

// An order this strategy placed but has not recorded yet — the reconnect-snapshot case
// the "unknown client_order_id" branch exists for — must still be adopted.
TEST_F(StrategyOwnershipTest, OwnUntrackedOrderIsStillAdopted) {
    const std::string cid = "MarketMaker_33532066729400";
    const std::string oid = "broker-order-2";

    market_maker_.on_order_ack(make_ack(cid, oid));

    ASSERT_EQ(market_maker_.acked.size(), 1u);
    ASSERT_NE(market_maker_.get_order(cid), nullptr);
    EXPECT_EQ(market_maker_.get_order(cid)->order_id, oid);

    market_maker_.on_order_fill(make_fill(cid, oid));
    EXPECT_EQ(market_maker_.filled.size(), 1u);
    EXPECT_DOUBLE_EQ(from_qty_t(positions_.get_position("MarketMaker", kInstrument).quantity), -0.001);
    EXPECT_DOUBLE_EQ(from_qty_t(positions_.get_position("MANUAL", kInstrument).quantity), 0.0);
}

// Coinbase sends a follow-up ack with an empty client_order_id. It is only ours if we
// already know the broker order_id from the first ack.
TEST_F(StrategyOwnershipTest, EmptyClientOrderIdResolvesThroughTheBrokerOrderId) {
    const std::string cid = "MarketMaker_42";
    const std::string oid = "broker-order-3";

    const auto follow_up = make_ack("", oid);
    market_maker_.on_order_ack(follow_up);
    EXPECT_TRUE(market_maker_.acked.empty()) << "unknown order_id must not be adopted";

    market_maker_.on_order_ack(make_ack(cid, oid));
    ASSERT_EQ(market_maker_.acked.size(), 1u);

    market_maker_.on_order_ack(follow_up);
    ASSERT_EQ(market_maker_.acked.size(), 2u);
    EXPECT_EQ(market_maker_.acked.back(), "");
}

// A pre-trade risk reject is synthesised inside submit_order() before the order is in
// the map, so ownership has to fall back to the client_order_id prefix.
TEST_F(StrategyOwnershipTest, LocallySynthesisedRejectStillReachesTheOwner) {
    RiskLimits limits;
    limits.max_position_size = to_qty_t(0.0001);
    risk_.set_limits("MarketMaker", limits);

    const auto cid = market_maker_.submit_order(
        kInstrument, BrokerId::COINBASE, Side::BUY, to_qty_t(1.0),
        OrderType::LIMIT, to_price_t(77950.64), TimeInForce::GTC, true);

    EXPECT_TRUE(cid.empty()) << "risk manager should have rejected the order";
    ASSERT_EQ(market_maker_.rejected.size(), 1u);
    EXPECT_TRUE(market_maker_.rejected.front().starts_with("MarketMaker_"));
}

// Ownership is decided by the "<name>_" prefix, so a strategy must not claim an order
// belonging to a differently named one.
TEST_F(StrategyOwnershipTest, OwnsClientOrderIdMatchesOnlyItsOwnPrefix) {
    market_maker_.on_order_ack(make_ack("MarketMaker_1", "o1"));
    market_maker_.on_order_ack(make_ack("MarketMakerX_1", "o2"));
    market_maker_.on_order_ack(make_ack("MarketMaker", "o3"));
    market_maker_.on_order_ack(make_ack("MarketMaker_", "o4"));
    market_maker_.on_order_ack(make_ack("OtherMarketMaker_1", "o5"));

    ASSERT_EQ(market_maker_.acked.size(), 1u);
    EXPECT_EQ(market_maker_.acked.front(), "MarketMaker_1");
}
