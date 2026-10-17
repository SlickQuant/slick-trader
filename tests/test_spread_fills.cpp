// A fill of an order for a multi-leg instrument: the order moves at the spread's price, and
// the positions it leaves are in the legs.
//
// What goes wrong without FillScope: a spread fill booked like any other puts a position in
// the spread itself, where nothing is held and nothing clears, and the leg fills that follow
// either go unbooked or are taken for fills of the order and overfill it.

#include <gtest/gtest.h>

#include "strategy_manager_fixture.hpp"

#include <slick-trader/core/broker_status.hpp>
#include <slick-trader/events/order_event.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace slick::trader;

namespace {

constexpr const char* SPREAD = "ESZ6-ESH7";

class SpreadStrategy : public StrategyBase {
public:
    SpreadStrategy() : StrategyBase("Spreader", "1.0.0") {}

    using StrategyBase::get_order;
    using StrategyBase::submit_order;

    std::vector<OrderFillEvent> fills;
    std::vector<OrderFillEvent> leg_fills;

protected:
    void on_fill(const OrderFillEvent& e) override { fills.push_back(e); }
    void on_leg_fill(const OrderFillEvent& e) override { leg_fills.push_back(e); }
};

OrderFillEvent make_fill(const std::string& client_id, const char* instrument, Side side, double price, double qty,
                         FillScope scope) {
    OrderFillEvent ev;
    ev.broker_id = BrokerId::CME;
    ev.set_instrument(instrument);
    ev.set_client_order_id(client_id);
    ev.set_order_id("9001");
    ev.side = side;
    ev.fill_price = to_price_t(price);
    ev.fill_quantity = to_qty_t(qty);
    ev.scope = scope;
    return ev;
}

class SpreadFillTest : public ::testing::Test, protected test::StrategyManagerFixture {
protected:
    void SetUp() override {
        auto& reg = BrokerStatusRegistry::instance();
        reg.set_name(BrokerId::CME, "CME");
        reg.set_order_enabled(BrokerId::CME, true);
        reg.set_order_connected(BrokerId::CME, true);
        add_strategy(strategy_);
        strategy_->start();

        // A buy of 2 calendar spreads at -12.50, acknowledged.
        client_id_ = strategy_->submit_order(SPREAD, BrokerId::CME, Side::BUY, to_qty_t(2), OrderType::LIMIT,
                                             to_price_t(-12.5), TimeInForce::DAY, false);
        ASSERT_FALSE(client_id_.empty());
        OrderAckEvent ack;
        ack.broker_id = BrokerId::CME;
        ack.set_instrument(SPREAD);
        ack.set_client_order_id(client_id_);
        ack.set_order_id("9001");
        ack.side = Side::BUY;
        ack.price = to_price_t(-12.5);
        ack.quantity = to_qty_t(2);
        manager_.process_event(&ack);
    }

    // One spread traded: the order's fill, then the two legs' - buy the near month, sell the far one.
    void fill_one_spread(double remaining) {
        auto order = make_fill(client_id_, SPREAD, Side::BUY, -12.5, 1, FillScope::ORDER);
        order.remaining_quantity = to_qty_t(remaining);
        manager_.process_event(&order);
        const auto near_leg = make_fill(client_id_, "ESZ6", Side::BUY, 5000.0, 1, FillScope::LEG);
        manager_.process_event(&near_leg);
        const auto far_leg = make_fill(client_id_, "ESH7", Side::SELL, 5012.5, 1, FillScope::LEG);
        manager_.process_event(&far_leg);
    }

    std::shared_ptr<SpreadStrategy> strategy_ = std::make_shared<SpreadStrategy>();
    std::string client_id_;
};

} // namespace

TEST_F(SpreadFillTest, TheOrderMovesWithItsOwnFillAndThePositionsAreTheLegs) {
    fill_one_spread(1);

    // The order: half done, at the spread's price - and not moved again by the legs.
    const Order* order = strategy_->get_order(client_id_);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->filled_qty, to_qty_t(1));
    EXPECT_EQ(order->remaining_qty, to_qty_t(1));
    EXPECT_EQ(order->status, OrderStatus::PARTIALLY_FILLED);
    EXPECT_DOUBLE_EQ(from_price_t(order->avg_fill_price), -12.5);

    // The positions: one long near-month contract, one short far-month one, and none in the spread.
    EXPECT_TRUE(positions_.get_position("Spreader", SPREAD).is_flat());
    const auto near_leg = positions_.get_position("Spreader", "ESZ6");
    EXPECT_EQ(near_leg.quantity, to_qty_t(1));
    EXPECT_DOUBLE_EQ(from_price_t(near_leg.avg_price), 5000.0);
    const auto far_leg = positions_.get_position("Spreader", "ESH7");
    EXPECT_EQ(far_leg.quantity, to_qty_t(-1));
    EXPECT_DOUBLE_EQ(from_price_t(far_leg.avg_price), 5012.5);

    // The strategy hears of each for what it is.
    ASSERT_EQ(strategy_->fills.size(), 1u);
    EXPECT_EQ(strategy_->fills[0].scope, FillScope::ORDER);
    EXPECT_STREQ(strategy_->fills[0].instrument, SPREAD);
    ASSERT_EQ(strategy_->leg_fills.size(), 2u);
    EXPECT_STREQ(strategy_->leg_fills[0].instrument, "ESZ6");
    EXPECT_EQ(strategy_->leg_fills[1].side, Side::SELL);
}

TEST_F(SpreadFillTest, LegsOfTheFinalFillAreStillBookedAfterTheOrderIsDone) {
    fill_one_spread(1);
    fill_one_spread(0);   // the order is filled before this fill's legs arrive

    const Order* order = strategy_->get_order(client_id_);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->status, OrderStatus::FILLED);
    EXPECT_EQ(order->filled_qty, to_qty_t(2));
    EXPECT_EQ(positions_.get_position("Spreader", "ESZ6").quantity, to_qty_t(2));
    EXPECT_EQ(positions_.get_position("Spreader", "ESH7").quantity, to_qty_t(-2));
    EXPECT_TRUE(positions_.get_position("Spreader", SPREAD).is_flat());
}

// Every strategy sees every fill: a leg of someone else's spread order books nothing here.
TEST_F(SpreadFillTest, AnotherStrategysLegFillIsIgnored) {
    auto foreign = make_fill("Other_1", "ESZ6", Side::BUY, 5000.0, 1, FillScope::LEG);
    foreign.set_order_id("777");
    manager_.process_event(&foreign);
    EXPECT_TRUE(positions_.get_position("Spreader", "ESZ6").is_flat());
    EXPECT_TRUE(strategy_->leg_fills.empty());
}

// An ordinary fill is what it always was: order and position together.
TEST_F(SpreadFillTest, AnOrdinaryFillStillMovesOrderAndPosition) {
    auto fill = make_fill(client_id_, SPREAD, Side::BUY, -12.5, 2, FillScope::ORDER_AND_POSITION);
    manager_.process_event(&fill);
    EXPECT_EQ(OrderFillEvent().scope, FillScope::ORDER_AND_POSITION);   // the default
    EXPECT_EQ(positions_.get_position("Spreader", SPREAD).quantity, to_qty_t(2));
    EXPECT_EQ(strategy_->get_order(client_id_)->status, OrderStatus::FILLED);
    EXPECT_TRUE(strategy_->leg_fills.empty());
}

// A restart: the strategy tracks no orders, and its broker does not restate one that had
// filled before the process stopped - but that order's leg fills still arrive, and its
// client_order_id says whose they are.
TEST_F(SpreadFillTest, LegFillsOfAnOrderNotTrackedSinceARestartAreStillBooked) {
    const std::string before_restart = "Spreader_1";   // placed by the last run
    auto near_leg = make_fill(before_restart, "ESZ6", Side::BUY, 5000.0, 1, FillScope::LEG);
    near_leg.set_order_id("4242");
    manager_.process_event(&near_leg);
    auto far_leg = make_fill(before_restart, "ESH7", Side::SELL, 5012.5, 1, FillScope::LEG);
    far_leg.set_order_id("4242");
    manager_.process_event(&far_leg);

    EXPECT_EQ(positions_.get_position("Spreader", "ESZ6").quantity, to_qty_t(1));
    EXPECT_EQ(positions_.get_position("Spreader", "ESH7").quantity, to_qty_t(-1));
    EXPECT_EQ(strategy_->leg_fills.size(), 2u);
    EXPECT_EQ(strategy_->get_order(before_restart), nullptr) << "booked, not taken up as an order";
}

TEST_F(SpreadFillTest, AFillOfAnOrderNotTrackedSinceARestartIsStillBooked) {
    auto fill = make_fill("Spreader_1", "ESZ6", Side::SELL, 5001.0, 2, FillScope::ORDER_AND_POSITION);
    fill.set_order_id("4243");
    manager_.process_event(&fill);
    EXPECT_EQ(positions_.get_position("Spreader", "ESZ6").quantity, to_qty_t(-2));
    EXPECT_EQ(strategy_->fills.size(), 1u);
}

// A broker that journals its fills replays them after a restart: those the strategies
// had are dropped before they reach a strategy, and the rest are booked as ever.
TEST_F(SpreadFillTest, AReplayedFillTheStrategiesHadIsDropped) {
    auto fill = make_fill(client_id_, "ESZ6", Side::BUY, 5000.0, 1, FillScope::LEG);
    fill.journal_seq = 7;
    manager_.process_event(&fill);
    manager_.process_event(&fill);   // replayed
    EXPECT_EQ(positions_.get_position("Spreader", "ESZ6").quantity, to_qty_t(1));
    EXPECT_EQ(strategy_->leg_fills.size(), 1u);

    auto next = make_fill(client_id_, "ESZ6", Side::BUY, 5000.0, 1, FillScope::LEG);
    next.journal_seq = 8;
    manager_.process_event(&next);
    EXPECT_EQ(positions_.get_position("Spreader", "ESZ6").quantity, to_qty_t(2));

    auto unjournaled = make_fill(client_id_, "ESZ6", Side::BUY, 5000.0, 1, FillScope::LEG);
    manager_.process_event(&unjournaled);
    manager_.process_event(&unjournaled);
    EXPECT_EQ(positions_.get_position("Spreader", "ESZ6").quantity, to_qty_t(4)) << "only journaled fills replay";
}

// Without a position store positions start flat at a restart: a fill replayed from before it
// has nowhere to go.
TEST_F(SpreadFillTest, WithoutAPositionStoreAReplayedFillIsDropped) {
    auto fill = make_fill(client_id_, "ESZ6", Side::BUY, 5000.0, 1, FillScope::LEG);
    fill.journal_seq = 7;
    fill.replayed = true;
    manager_.process_event(&fill);
    EXPECT_TRUE(positions_.get_position("Spreader", "ESZ6").is_flat());
    EXPECT_TRUE(strategy_->leg_fills.empty());
}
