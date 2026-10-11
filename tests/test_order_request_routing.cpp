// Regression test: an order request must reach only the broker it is for.
//
// Bug: every order connector reads every request on the shared order queue, and none
// checked whom a request was for. With two brokers' order entry enabled, each order was
// sent to both - the wrong broker rejected it as an unknown product and the strategy
// saw its order die. NEW_ORDER carried a broker_id nobody read; MODIFY_ORDER and
// CANCEL_ORDER carried none at all, and a cancel not even its instrument.

#include <gtest/gtest.h>

#include <slick-trader/brokers/iorder_connector.hpp>
#include <slick-trader/core/broker_status.hpp>
#include <slick-trader/core/pnl_calculator.hpp>
#include <slick-trader/core/position_tracker.hpp>
#include <slick-trader/core/risk_manager.hpp>
#include <slick-trader/strategy/strategy_base.hpp>
#include <slick/queue.hpp>

#include <string>

using namespace slick::trader;

namespace {

class RoutingStrategy : public StrategyBase {
public:
    RoutingStrategy() : StrategyBase("Routing", "1.0.0") {}

    using StrategyBase::cancel_order;
    using StrategyBase::modify_order;
    using StrategyBase::submit_order;
};

class OrderRequestRoutingTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto& reg = BrokerStatusRegistry::instance();
        for (const auto broker : {BrokerId::COINBASE, BrokerId::CME}) {
            reg.set_order_enabled(broker, true);
            reg.set_order_connected(broker, true);
        }
        cursor_ = order_q_.initial_reading_index();
        strategy_.initialize(nlohmann::json::object(), positions_, pnl_, risk_, md_q_, order_q_);
        strategy_.start();
    }

    /// The next request on the order queue, as a connector reads it.
    template <typename T>
    const T& next() {
        auto [data, size] = order_q_.read(cursor_);
        EXPECT_NE(data, nullptr) << "no request was published";
        static const T empty{};
        return data ? *reinterpret_cast<const T*>(data) : empty;
    }

    PositionTracker positions_;
    PnLCalculator   pnl_{positions_};
    RiskManager     risk_{positions_};
    slick::queue<uint8_t> md_q_{4096};
    slick::queue<uint8_t> order_q_{65536};
    uint64_t cursor_ = 0;
    RoutingStrategy strategy_;
};

} // namespace

TEST_F(OrderRequestRoutingTest, EveryRequestNamesItsBroker) {
    const auto client_id = strategy_.submit_order("ESZ6", BrokerId::CME, Side::BUY, to_qty_t(1), OrderType::LIMIT,
                                                  to_price_t(1000.0), TimeInForce::GTC, false);
    ASSERT_FALSE(client_id.empty());
    const auto& created = next<NewOrderEvent>();
    EXPECT_EQ(created.type, EventType::NEW_ORDER);
    EXPECT_EQ(created.broker_id, BrokerId::CME);

    strategy_.modify_order(client_id, to_price_t(1000.25), to_qty_t(1));
    const auto& modified = next<ModifyOrderEvent>();
    EXPECT_EQ(modified.type, EventType::MODIFY_ORDER);
    EXPECT_EQ(modified.broker_id, BrokerId::CME);

    strategy_.cancel_order("777", client_id);
    const auto& cancelled = next<CancelOrderEvent>();
    EXPECT_EQ(cancelled.type, EventType::CANCEL_ORDER);
    EXPECT_EQ(cancelled.broker_id, BrokerId::CME);
    EXPECT_STREQ(cancelled.instrument, "ESZ6") << "a connector needs the instrument to cancel";
    EXPECT_EQ(cancelled.side, Side::BUY);
}

TEST_F(OrderRequestRoutingTest, AConnectorActsOnlyOnItsOwnBrokersRequests) {
    strategy_.submit_order("ESZ6", BrokerId::CME, Side::BUY, to_qty_t(1), OrderType::LIMIT, to_price_t(1000.0),
                           TimeInForce::GTC, false);
    const auto& cme_order = next<NewOrderEvent>();
    EXPECT_TRUE(is_request_for(cme_order, BrokerId::CME));
    EXPECT_FALSE(is_request_for(cme_order, BrokerId::COINBASE)) << "Coinbase must not send a CME order";
    EXPECT_FALSE(is_request_for(cme_order, BrokerId::HYPERLIQUID));

    strategy_.submit_order("BTC-USD", BrokerId::COINBASE, Side::SELL, to_qty_t(0.001), OrderType::LIMIT,
                           to_price_t(60000.0), TimeInForce::GTC, false);
    const auto& coinbase_order = next<NewOrderEvent>();
    EXPECT_TRUE(is_request_for(coinbase_order, BrokerId::COINBASE));
    EXPECT_FALSE(is_request_for(coinbase_order, BrokerId::CME));
}

TEST_F(OrderRequestRoutingTest, AnOrderLearnedFromAnAckKeepsItsBroker) {
    // An order this strategy owns but did not submit in this run - replayed by a broker on
    // reconnect - is tracked from its ack, and its cancel must still find its broker.
    OrderAckEvent ack;
    ack.broker_id = BrokerId::COINBASE;
    ack.set_instrument("BTC-USD");
    ack.set_client_order_id("Routing_replayed");
    ack.set_order_id("abc");
    ack.side = Side::SELL;
    ack.status = OrderStatus::ACKNOWLEDGED;
    strategy_.on_order_ack(ack);

    strategy_.cancel_order("abc", "Routing_replayed");
    const auto& cancelled = next<CancelOrderEvent>();
    EXPECT_EQ(cancelled.broker_id, BrokerId::COINBASE);
    EXPECT_STREQ(cancelled.instrument, "BTC-USD");
}
