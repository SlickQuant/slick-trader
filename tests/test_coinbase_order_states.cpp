// Coinbase reports an order more than once: the REST response and the user channel both ack a new
// order and both report a cancel, and the channel repeated a cancel. Every report went out, so a
// new order came to strategies and the dashboard as three acks and a cancel as three. The channel's
// acks also had qty 0: it gives no base size, and the qty was read off that alone. A strategy's own
// record of the order then held qty 0.

#include <gtest/gtest.h>

#include <slick-trader/brokers/coinbase/coinbase_order_states.hpp>

#include <string>

using namespace slick::trader;

namespace {

const std::string OID = "3b2379f4-1c41-4fcd-97ef-86681d83c08c";
const price_t PRICE = to_price_t(62337.98);
const qty_t QTY = to_qty_t(0.001);

// What ack() made of an update: whether it goes out, and the price, stop and qty it goes out with.
struct Acked {
    bool sent;
    price_t price;
    price_t stop;
    qty_t qty;
};

Acked ack(CoinbaseOrderStates& states, OrderStatus status, price_t price, qty_t qty, price_t stop = NULL_PRICE) {
    const bool sent = states.ack(OID, status, price, stop, qty);
    return {sent, price, stop, qty};
}

} // namespace

// As the log had it: the REST response's pending ack, the channel's pending (no qty) and its open.
TEST(CoinbaseOrderStatesTest, ANewOrderIsAckedOncePerStatusWithItsQty) {
    CoinbaseOrderStates states;
    EXPECT_TRUE(ack(states, OrderStatus::PENDING_NEW, PRICE, QTY).sent);
    EXPECT_FALSE(ack(states, OrderStatus::PENDING_NEW, PRICE, 0).sent) << "the channel's pending says nothing new";
    const auto open = ack(states, OrderStatus::ACKNOWLEDGED, PRICE, 0);
    EXPECT_TRUE(open.sent);
    EXPECT_EQ(open.qty, QTY) << "a qty the update doesn't know is the one known";
    EXPECT_EQ(open.price, PRICE);
    EXPECT_FALSE(ack(states, OrderStatus::ACKNOWLEDGED, PRICE, QTY).sent) << "an open update after a fill repeats it";
}

// The channel can beat the REST response: its open came first, and the response's pending ack
// would take the order back to pending.
TEST(CoinbaseOrderStatesTest, APendingAckAfterTheOrderOpenedIsStale) {
    CoinbaseOrderStates states;
    EXPECT_TRUE(ack(states, OrderStatus::PENDING_NEW, PRICE, 0).sent);
    EXPECT_TRUE(ack(states, OrderStatus::ACKNOWLEDGED, PRICE, QTY).sent);
    EXPECT_FALSE(ack(states, OrderStatus::PENDING_NEW, PRICE, QTY).sent);
}

// The REST response after the channel's pending, which knew no qty: it fills the qty in.
TEST(CoinbaseOrderStatesTest, TheRestResponseFillsInTheQtyThePendingUpdateLacked) {
    CoinbaseOrderStates states;
    EXPECT_TRUE(ack(states, OrderStatus::PENDING_NEW, PRICE, 0).sent);
    const auto rest = ack(states, OrderStatus::PENDING_NEW, PRICE, QTY);
    EXPECT_TRUE(rest.sent);
    EXPECT_EQ(rest.qty, QTY);
}

// A modify's REST response, then the channel's open with the same price and qty.
TEST(CoinbaseOrderStatesTest, AModifyIsAckedOnce) {
    CoinbaseOrderStates states;
    ASSERT_TRUE(ack(states, OrderStatus::ACKNOWLEDGED, PRICE, QTY).sent);
    const price_t moved = to_price_t(62300.0);
    EXPECT_TRUE(ack(states, OrderStatus::ACKNOWLEDGED, moved, QTY).sent);
    EXPECT_FALSE(ack(states, OrderStatus::ACKNOWLEDGED, moved, QTY).sent);
    EXPECT_TRUE(ack(states, OrderStatus::ACKNOWLEDGED, moved, to_qty_t(0.002)).sent);
}

// A market order: the REST ack has NULL_PRICE, the channel's limit price is 0. Neither is a price,
// and the same goes for its stop.
TEST(CoinbaseOrderStatesTest, AnUnknownPriceKeepsTheKnownOne) {
    CoinbaseOrderStates states;
    ASSERT_TRUE(ack(states, OrderStatus::PENDING_NEW, NULL_PRICE, QTY).sent);
    const auto open = ack(states, OrderStatus::ACKNOWLEDGED, 0, QTY, 0);
    EXPECT_TRUE(open.sent);
    EXPECT_EQ(open.price, NULL_PRICE);
    EXPECT_EQ(open.stop, NULL_PRICE);
    EXPECT_FALSE(ack(states, OrderStatus::ACKNOWLEDGED, 0, QTY, 0).sent);
}

// A modify that moved the stop alone said nothing new to ack(): its REST ack and the channel's
// open both dropped, and the strategy's order stayed pending a modify with the old stop.
TEST(CoinbaseOrderStatesTest, AModifyOfTheStopAloneIsAcked) {
    CoinbaseOrderStates states;
    ASSERT_TRUE(ack(states, OrderStatus::ACKNOWLEDGED, PRICE, QTY, to_price_t(62000.0)).sent);
    const price_t moved = to_price_t(61900.0);
    EXPECT_TRUE(ack(states, OrderStatus::ACKNOWLEDGED, PRICE, QTY, moved).sent);
    EXPECT_FALSE(ack(states, OrderStatus::ACKNOWLEDGED, PRICE, QTY, moved).sent) << "the channel's open repeats it";
    const auto resized = ack(states, OrderStatus::ACKNOWLEDGED, PRICE, to_qty_t(0.002));
    EXPECT_TRUE(resized.sent);
    EXPECT_EQ(resized.stop, moved) << "a modify that leaves the stop goes out with the one known";
}

// The cancel's REST response, then the channel's CANCELLED twice: one cancel goes out, and an ack
// that comes late is stale.
TEST(CoinbaseOrderStatesTest, ACancelGoesOutOnce) {
    CoinbaseOrderStates states;
    ASSERT_TRUE(ack(states, OrderStatus::ACKNOWLEDGED, PRICE, QTY).sent);
    EXPECT_FALSE(states.ended(OID));
    EXPECT_TRUE(states.end(OID));
    EXPECT_TRUE(states.ended(OID));
    EXPECT_FALSE(states.end(OID));
    EXPECT_FALSE(states.end(OID));
    EXPECT_FALSE(ack(states, OrderStatus::ACKNOWLEDGED, PRICE, QTY).sent);
}

// An order never acked here, as one placed before a restart, ends once too.
TEST(CoinbaseOrderStatesTest, AnOrderNotKnownEndsOnce) {
    CoinbaseOrderStates states;
    EXPECT_FALSE(states.ended(OID));
    EXPECT_TRUE(states.end(OID));
    EXPECT_FALSE(states.end(OID));
}

// Ended orders are remembered for their late repeats, but not for good: the oldest go.
TEST(CoinbaseOrderStatesTest, EndedOrdersAreForgottenOldestFirst) {
    CoinbaseOrderStates states;
    for (std::size_t i = 0; i <= CoinbaseOrderStates::ENDED_KEPT; ++i) {
        const std::string id = "O" + std::to_string(i);
        price_t price = PRICE;
        price_t stop = NULL_PRICE;
        qty_t qty = QTY;
        ASSERT_TRUE(states.ack(id, OrderStatus::ACKNOWLEDGED, price, stop, qty));
        ASSERT_TRUE(states.end(id));
    }
    EXPECT_EQ(states.size(), CoinbaseOrderStates::ENDED_KEPT);
    EXPECT_FALSE(states.ended("O0"));
    EXPECT_TRUE(states.ended("O1"));
    EXPECT_TRUE(states.ended("O" + std::to_string(CoinbaseOrderStates::ENDED_KEPT)));
}
