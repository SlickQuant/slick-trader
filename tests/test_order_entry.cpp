// Order-entry rules brokers share: post-only where the exchange has none, whole lots,
// the request rate limit, per-contract fees, local rejects, and routing order requests
// to their own broker only.

#include <gtest/gtest.h>
#include <slick-trader/brokers/common/order_entry.hpp>
#include <slick-trader/brokers/common/order_event_writer.hpp>
#include <slick-trader/events/event_serializer.hpp>
#include <slick/queue.hpp>
#include <stdexcept>
#include <string>
#include <vector>

using namespace slick::trader;

namespace {

const char* check(PostOnlyPolicy policy, const SharedTopOfBook* top, Side side, double price,
                  OrderType type = OrderType::LIMIT, TimeInForce tif = TimeInForce::GTC) {
    bool warned = false;
    return post_only_problem(policy, top, side, type, tif, to_price_t(price), warned, "test");
}

bool contains(const char* text, const char* part) {
    return text && std::string(text).find(part) != std::string::npos;
}

} // namespace

TEST(OrderEntryTest, RejectIfCrossingChecksTheSideAnOrderCouldTrade) {
    SharedTopOfBook top;
    top.bid = to_price_t(99.0);
    top.ask = to_price_t(101.0);
    EXPECT_EQ(check(PostOnlyPolicy::REJECT_IF_CROSSING, &top, Side::BUY, 100.0), nullptr);
    EXPECT_TRUE(contains(check(PostOnlyPolicy::REJECT_IF_CROSSING, &top, Side::BUY, 101.0), "cross"));
    EXPECT_TRUE(contains(check(PostOnlyPolicy::REJECT_IF_CROSSING, &top, Side::SELL, 99.0), "cross"));
    EXPECT_EQ(check(PostOnlyPolicy::REJECT_IF_CROSSING, &top, Side::SELL, 100.0), nullptr);

    top.ask = SharedTopOfBook::EMPTY;
    EXPECT_EQ(check(PostOnlyPolicy::REJECT_IF_CROSSING, &top, Side::BUY, 1000.0), nullptr) << "nothing to trade";
    top.ask = SharedTopOfBook::UNKNOWN;
    EXPECT_TRUE(contains(check(PostOnlyPolicy::REJECT_IF_CROSSING, &top, Side::BUY, 1.0), "not in sequence"));
    EXPECT_TRUE(contains(check(PostOnlyPolicy::REJECT_IF_CROSSING, nullptr, Side::BUY, 1.0), "no market data"));
}

TEST(OrderEntryTest, OnlyAnOrderThatRestsIsHeldToPostOnly) {
    SharedTopOfBook top;
    top.ask = to_price_t(101.0);
    EXPECT_EQ(check(PostOnlyPolicy::REJECT, &top, Side::BUY, 102.0, OrderType::LIMIT, TimeInForce::IOC), nullptr);
    EXPECT_EQ(check(PostOnlyPolicy::REJECT, &top, Side::BUY, 102.0, OrderType::MARKET), nullptr);
    EXPECT_TRUE(contains(check(PostOnlyPolicy::REJECT, &top, Side::BUY, 100.0), "post-only"));
    EXPECT_EQ(check(PostOnlyPolicy::ACCEPT, &top, Side::BUY, 102.0), nullptr);
    EXPECT_EQ(check(PostOnlyPolicy::NATIVE, &top, Side::BUY, 102.0), nullptr);
    EXPECT_TRUE(rests(OrderType::LIMIT, TimeInForce::DAY));
    EXPECT_FALSE(rests(OrderType::STOP_LIMIT, TimeInForce::GTC));
}

TEST(OrderEntryTest, ParsesThePolicyWithTheExchangesOwnWordForNative) {
    EXPECT_EQ(parse_post_only_policy("reject_if_crossing", "", "x"), PostOnlyPolicy::REJECT_IF_CROSSING);
    EXPECT_EQ(parse_post_only_policy("accept", "", "x"), PostOnlyPolicy::ACCEPT);
    EXPECT_EQ(parse_post_only_policy("reject", "", "x"), PostOnlyPolicy::REJECT);
    EXPECT_EQ(parse_post_only_policy("passive", "passive", "x"), PostOnlyPolicy::NATIVE);
    EXPECT_THROW(parse_post_only_policy("passive", "", "x"), std::runtime_error) << "no native word, no NATIVE";
    EXPECT_THROW(parse_post_only_policy("bogus", "passive", "x"), std::runtime_error);
}

TEST(OrderEntryTest, WholeLotsOnly) {
    EXPECT_EQ(whole_lots(to_qty_t(3)), 3u);
    EXPECT_EQ(whole_lots(to_qty_t(2.5)), 0u);
    EXPECT_EQ(whole_lots(0), 0u);
    EXPECT_EQ(whole_lots(-to_qty_t(1)), 0u);
    EXPECT_EQ(lots_to_qty(7), to_qty_t(7));
}

TEST(OrderEntryTest, RateLimiterRefusesWhatGoesOverItsSecond) {
    RateLimiter limiter(2);
    constexpr uint64_t S = 1'000'000'000;
    EXPECT_TRUE(limiter.allow(5 * S));
    EXPECT_TRUE(limiter.allow(5 * S + 1));
    EXPECT_FALSE(limiter.allow(5 * S + 2));
    EXPECT_TRUE(limiter.allow(6 * S)) << "a new second";
    RateLimiter unlimited;
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(unlimited.allow(5 * S));
    }
}

TEST(OrderEntryTest, FeesAreLookedUpByTheFirstNameListed) {
    const auto fees = FeeTable::from_json(nlohmann::json{{"default", 1.5}, {"ES", 1.18}, {"MES", 0.35}});
    EXPECT_DOUBLE_EQ(fees.per_contract({"ESZ6", "ES"}), 1.18);
    EXPECT_DOUBLE_EQ(fees.per_contract({"MESZ6", "MES"}), 0.35);
    EXPECT_DOUBLE_EQ(fees.per_contract({"NQZ6", "NQ"}), 1.5);
    EXPECT_DOUBLE_EQ(FeeTable::from_json(nlohmann::json()).per_contract({"ES"}), 0.0);
}

TEST(OrderEntryTest, ALocalRejectRestatesTheRequest) {
    NewOrderEvent request;
    request.broker_id = BrokerId::CME;
    request.set_instrument("ESZ6");
    request.set_client_order_id("s_1");
    request.side = Side::SELL;
    request.price = to_price_t(5000.25);
    request.quantity = to_qty_t(2);
    const auto reject = reject_of(request, BrokerId::CME, "the quantity must be whole");
    EXPECT_EQ(reject.broker_id, BrokerId::CME);
    EXPECT_STREQ(reject.instrument, "ESZ6");
    EXPECT_STREQ(reject.client_order_id, "s_1");
    EXPECT_EQ(reject.side, Side::SELL);
    EXPECT_STREQ(reject.reason, "the quantity must be whole");
}

// Regression: a connector routed a modify or cancel by "does one of my sessions own this
// client_order_id?" alone. Every connector reads every request, so the broker a request
// is for has to be the first filter, for every request type.
TEST(OrderEntryTest, DispatchHandsAConnectorOnlyItsOwnBrokersRequests) {
    slick::queue<uint8_t> queue(1u << 16);
    uint64_t cursor = queue.initial_reading_index();
    auto publish = [&](auto event, BrokerId broker) {
        event.broker_id = broker;
        event.set_client_order_id("s_1");
        EventSerializer::publish(queue, event);
    };
    publish(NewOrderEvent{}, BrokerId::COINBASE);
    publish(ModifyOrderEvent{}, BrokerId::COINBASE);
    publish(CancelOrderEvent{}, BrokerId::COINBASE);
    publish(NewOrderEvent{}, BrokerId::ICE);
    publish(ModifyOrderEvent{}, BrokerId::ICE);
    publish(CancelOrderEvent{}, BrokerId::ICE);

    std::vector<EventType> seen;
    int read = 0;
    while (dispatch_order_request(
        queue, cursor, BrokerId::ICE, [&](const NewOrderEvent& e) { seen.push_back(e.type); },
        [&](const ModifyOrderEvent& e) { seen.push_back(e.type); },
        [&](const CancelOrderEvent& e) { seen.push_back(e.type); })) {
        ++read;
    }
    EXPECT_EQ(read, 6) << "every request is read";
    EXPECT_EQ(seen, (std::vector<EventType>{EventType::NEW_ORDER, EventType::MODIFY_ORDER, EventType::CANCEL_ORDER}))
        << "but only ICE's are handled";
}
