// Regression tests: what StrategyManager asks the brokers to subscribe.
//
// Three bugs lived in subscribe()/unsubscribe():
//   - Subscriptions were counted by instrument name alone, so the second kind of data for an
//     instrument was taken for one already held and never sent: a strategy subscribing the
//     book and then the trades of BTC-USD got no trade subscription. The same went for the
//     same name at a second broker.
//   - The loop writing the instruments into the request never advanced its slot: every name
//     was written over slot 0 and the other slots went out as whatever the queue held.
//   - unsubscribe() sent its request only when there was nothing to unsubscribe.

#include <gtest/gtest.h>

#include <slick-trader/core/pnl_calculator.hpp>
#include <slick-trader/core/position_tracker.hpp>
#include <slick-trader/core/risk_manager.hpp>
#include <slick-trader/core/strategy_manager.hpp>
#include <slick-trader/events/md_subscription.hpp>
#include <slick-trader/strategy/strategy_base.hpp>
#include <slick/queue.hpp>
#include <slick/stream_buffer_multiplexer.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace slick::trader;

namespace {

class SubscribingStrategy : public StrategyBase {
public:
    SubscribingStrategy(const std::string& name, std::vector<SubscriptionRequest> subscriptions)
        : StrategyBase(name, "1.0.0"), subscriptions_(std::move(subscriptions)) {}

    std::vector<SubscriptionRequest> get_subscriptions() const override { return subscriptions_; }

private:
    std::vector<SubscriptionRequest> subscriptions_;
};

// One request as a broker's market data connector reads it off the queue.
struct Request {
    EventType type;
    BrokerId broker;
    SubscriptionType subscription;
    std::vector<std::string> instruments;
};

class MdSubscriptionTest : public ::testing::Test {
protected:
    void add(const std::string& name, std::vector<SubscriptionRequest> subscriptions) {
        auto strategy = std::make_shared<SubscribingStrategy>(name, std::move(subscriptions));
        strategy->initialize(nlohmann::json::object(), positions_, pnl_, risk_, md_q_, order_q_);
        manager_.add_strategy(strategy);
    }

    // Every request published since the last call.
    std::vector<Request> requests() {
        std::vector<Request> out;
        for (;;) {
            auto [data, size] = md_q_.read(cursor_);
            if (data == nullptr) {
                return out;
            }
            const auto& request = *reinterpret_cast<const MDSubscription*>(data);
            EXPECT_EQ(size, sizeof(MDSubscription) + request.num_instruments * sizeof(char[32]));
            Request r{request.type, request.broker_id, request.subscription_type, {}};
            for (uint32_t i = 0; i < request.num_instruments; ++i) {
                r.instruments.emplace_back(request.instruments[i]);
            }
            out.push_back(std::move(r));
        }
    }

    PositionTracker positions_;
    PnLCalculator   pnl_{positions_};
    RiskManager     risk_{positions_};
    slick::stream_buffer_multiplexer mux_{1024};
    slick::queue<uint8_t> md_q_{65536};
    slick::queue<uint8_t> order_q_{4096};
    uint64_t cursor_ = md_q_.initial_reading_index();
    StrategyManager manager_{nlohmann::json{{"TOB_queue_size", 65536}, {"TOB_control_size", 4096}},
                             positions_, pnl_, risk_, mux_, md_q_, order_q_};
};

} // namespace

TEST_F(MdSubscriptionTest, EachKindOfDataForAnInstrumentReachesTheBroker) {
    // What market_maker and order_stacker ask for: the book, then the trades, of one instrument.
    add("Maker", {SubscriptionRequest(BrokerId::COINBASE, SubscriptionType::LEVEL2, {"BTC-USD"}),
                  SubscriptionRequest(BrokerId::COINBASE, SubscriptionType::TRADE, {"BTC-USD"})});
    manager_.start();

    const auto sent = requests();
    ASSERT_EQ(sent.size(), 2u) << "the trade subscription was dropped as a duplicate of the book's";
    EXPECT_EQ(sent[0].type, EventType::SUBSCRIBE);
    EXPECT_EQ(sent[0].subscription, SubscriptionType::LEVEL2);
    EXPECT_EQ(sent[1].type, EventType::SUBSCRIBE);
    EXPECT_EQ(sent[1].subscription, SubscriptionType::TRADE);
    EXPECT_EQ(sent[1].instruments, std::vector<std::string>{"BTC-USD"});
}

TEST_F(MdSubscriptionTest, TheSameNameAtTwoBrokersIsSubscribedAtBoth) {
    add("Arb", {SubscriptionRequest(BrokerId::COINBASE, SubscriptionType::TRADE, {"BTC"}),
                SubscriptionRequest(BrokerId::HYPERLIQUID, SubscriptionType::TRADE, {"BTC"})});
    manager_.start();

    const auto sent = requests();
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0].broker, BrokerId::COINBASE);
    EXPECT_EQ(sent[1].broker, BrokerId::HYPERLIQUID);
}

TEST_F(MdSubscriptionTest, EveryInstrumentOfARequestGetsItsOwnSlot) {
    const std::vector<std::string> instruments{"BTC-USD", "ETH-USD", "SOL-USD"};
    add("Basket", {SubscriptionRequest(BrokerId::COINBASE, SubscriptionType::TRADE, instruments)});
    manager_.start();

    const auto sent = requests();
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].instruments, instruments) << "every name was written to the first slot";
}

TEST_F(MdSubscriptionTest, ASubscriptionTwoStrategiesShareIsSentOnceAndDroppedOnce) {
    add("First", {SubscriptionRequest(BrokerId::CME, SubscriptionType::MBO, {"ESZ6"})});
    add("Second", {SubscriptionRequest(BrokerId::CME, SubscriptionType::MBO, {"ESZ6", "NQZ6"})});
    manager_.start();

    auto sent = requests();
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0].instruments, std::vector<std::string>{"ESZ6"});
    EXPECT_EQ(sent[1].instruments, std::vector<std::string>{"NQZ6"}) << "ESZ6 is already subscribed";

    manager_.stop();

    // The first strategy lets go of ESZ6 while the second still holds it: nothing to send.
    sent = requests();
    ASSERT_EQ(sent.size(), 1u) << "unsubscribe() published only when it had nothing to unsubscribe";
    EXPECT_EQ(sent[0].type, EventType::UNSUBSCRIBE);
    EXPECT_EQ(sent[0].broker, BrokerId::CME);
    EXPECT_EQ(sent[0].subscription, SubscriptionType::MBO);
    EXPECT_EQ(sent[0].instruments, (std::vector<std::string>{"ESZ6", "NQZ6"}));
}
