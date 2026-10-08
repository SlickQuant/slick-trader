// Regression test: an OrderStacker config change must reach the ladder without market data.
//
// Bug: on_config() ran on the WebServer thread, so it could only latch the new parameters —
// applying them touches pair_, which is worker-thread-owned. The only places that drained the
// latch were on_quote() and on_trade(), so PUT /api/strategies/OrderStacker/config did nothing
// at all until the instrument next ticked. StrategyManager only publishes a quote when the top
// of book actually moves, so on a quiet book — or a stalled feed — a new level_gap_ticks or
// stack_qty sat unapplied indefinitely while the old ladder kept working.
//
// The fix routes the update over the event bus: the WebServer publishes a STRATEGY_CONFIG event
// and the StrategyManager worker calls set_config() on its own thread, so on_config() applies
// the change in place the moment it arrives. These tests call set_config() the same way the
// worker does, with no quote behind it. test_strategy_config_event.cpp covers the wire format
// that carries it.
//
// order_stacker.cpp is a self-contained plugin translation unit with no header, so the strategy is
// pulled in directly here and driven through its public hooks.

#include <gtest/gtest.h>
#include <slick-trader/core/broker_status.hpp>
#include <slick-trader/core/pnl_calculator.hpp>
#include <slick-trader/core/position_tracker.hpp>
#include <slick-trader/core/risk_manager.hpp>
#include <slick/queue.hpp>

// One test binary cannot carry two plugins' C entry points, and
// test_market_maker_quote_slots.cpp already links a set. The tests drive the class directly,
// so neutralise the macro before pulling the translation unit in. strategy_export.hpp is
// #pragma once, so order_stacker.cpp's own include of it will not undo this.
#include <slick-trader/strategy/strategy_export.hpp>
#undef SLICK_TRADER_EXPORT_STRATEGY
#define SLICK_TRADER_EXPORT_STRATEGY(StrategyClass)

#include "../strategies/order_stacker.cpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr const char* kInstrument = "BTC-USD";
constexpr double      kTick       = 0.01;

// One order request as the broker connector would see it off the queue.
struct Request {
    EventType   type;
    std::string client_order_id;
    std::string order_id;
    Side        side{Side::BUY};
    double      price{0.0};
    double      qty{0.0};
};

// What the venue is holding, keyed by client_order_id.
struct Resting {
    std::string order_id;
    Side        side{Side::BUY};
    double      price{0.0};
    double      qty{0.0};
};

class OrderStackerConfigTest : public ::testing::Test {
protected:
    // levels=2 keeps the ladder small enough to assert price-by-price:
    // three working prices per side (level 0 plus two rungs).
    static nlohmann::json base_params() {
        return {
            {"instrument",      kInstrument},
            {"broker",          "COINBASE"},
            {"tick_size",       kTick},
            {"offset_ticks",    0},
            {"levels",          2},
            {"level_gap_ticks", 1},
            {"stack_qty",       0.001},
            {"top_qty",         0.001},
            {"max_position",    1.0},     // never latch the position guard
            {"post_only",       true},
            {"prefer_modify",   true},
        };
    }

    void SetUp() override {
        auto& reg = BrokerStatusRegistry::instance();
        reg.set_name(BrokerId::COINBASE, "Coinbase");
        reg.set_order_enabled(BrokerId::COINBASE, true);
        reg.set_order_connected(BrokerId::COINBASE, true);

        stacker_.initialize(base_params(), positions_, pnl_, risk_, md_q_, order_q_);
        stacker_.start();
    }

    void send_quote(double bid, double ask) {
        QuoteEvent ev;
        ev.timestamp_ns = ++clock_;
        ev.set_instrument(kInstrument);
        ev.bid_price    = to_price_t(bid);
        ev.ask_price    = to_price_t(ask);
        ev.bid_quantity = to_qty_t(1.0);
        ev.ask_quantity = to_qty_t(1.0);
        stacker_.on_quote(ev);
    }

    // Everything published since the last call.
    std::vector<Request> drain() {
        std::vector<Request> out;
        for (;;) {
            auto [data, size] = order_q_.read(order_cursor_);
            if (!data) break;
            const auto* base = reinterpret_cast<const Event*>(data);
            Request r;
            r.type = base->type;
            switch (base->type) {
            case EventType::NEW_ORDER: {
                const auto& e = *reinterpret_cast<const NewOrderEvent*>(data);
                r.client_order_id = e.client_order_id;
                r.side            = e.side;
                r.price           = from_price_t(e.price);
                r.qty             = from_qty_t(e.quantity);
                break;
            }
            case EventType::MODIFY_ORDER: {
                const auto& e = *reinterpret_cast<const ModifyOrderEvent*>(data);
                r.client_order_id = e.client_order_id;
                r.order_id        = e.order_id;
                r.price           = from_price_t(e.new_price);
                r.qty             = from_qty_t(e.new_quantity);
                break;
            }
            case EventType::CANCEL_ORDER: {
                const auto& e = *reinterpret_cast<const CancelOrderEvent*>(data);
                r.client_order_id = e.client_order_id;
                r.order_id        = e.order_id;
                break;
            }
            default:
                continue;
            }
            out.push_back(std::move(r));
        }
        return out;
    }

    void send_ack(const std::string& cid, const Resting& o) {
        OrderAckEvent ev;
        ev.timestamp_ns  = ++clock_;
        ev.set_instrument(kInstrument);
        ev.set_client_order_id(cid);
        ev.set_order_id(o.order_id);
        ev.order_type    = OrderType::LIMIT;
        ev.time_in_force = TimeInForce::GTC;
        ev.side          = o.side;
        ev.price         = to_price_t(o.price);
        ev.quantity      = to_qty_t(o.qty);
        ev.status        = OrderStatus::ACKNOWLEDGED;
        stacker_.on_order_ack(ev);
    }

    void send_cancel(const std::string& cid, const Resting& o) {
        OrderCancelEvent ev;
        ev.timestamp_ns = ++clock_;
        ev.set_instrument(kInstrument);
        ev.set_client_order_id(cid);
        ev.set_order_id(o.order_id);
        stacker_.on_order_cancel(ev);
    }

    // Plays the venue: answers every request the strategy makes, then lets it react, until it
    // stops asking for anything. Returns the total number of requests answered.
    int settle() {
        int total = 0;
        for (int pass = 0; pass < 32; ++pass) {
            const auto requests = drain();
            if (requests.empty()) return total;
            total += static_cast<int>(requests.size());

            for (const auto& r : requests) {
                switch (r.type) {
                case EventType::NEW_ORDER: {
                    Resting o{"broker-" + r.client_order_id, r.side, r.price, r.qty};
                    venue_[r.client_order_id] = o;
                    send_ack(r.client_order_id, o);
                    break;
                }
                case EventType::MODIFY_ORDER: {
                    auto it = venue_.find(r.client_order_id);
                    if (it == venue_.end()) break;
                    it->second.price = r.price;
                    it->second.qty   = r.qty;
                    send_ack(r.client_order_id, it->second);
                    break;
                }
                case EventType::CANCEL_ORDER: {
                    auto it = venue_.find(r.client_order_id);
                    if (it == venue_.end()) break;
                    const Resting o = it->second;
                    venue_.erase(it);
                    send_cancel(r.client_order_id, o);
                    break;
                }
                default:
                    break;
                }
            }
        }
        ADD_FAILURE() << "the ladder never settled";
        return total;
    }

    // Prices currently resting on one side, rounded to the tick so the comparison is exact.
    std::set<long long> resting_ticks(Side side) const {
        std::set<long long> out;
        for (const auto& [cid, o] : venue_) {
            if (o.side == side) out.insert(std::llround(o.price / kTick));
        }
        return out;
    }

    // Total quantity resting at each price on one side, in units of 0.0001. A level's size is
    // what stack_qty controls; whether the stacker gets there with one order or two is its
    // business.
    std::map<long long, long long> resting_qty_by_tick(Side side) const {
        std::map<long long, long long> out;
        for (const auto& [cid, o] : venue_) {
            if (o.side != side) continue;
            out[std::llround(o.price / kTick)] += std::llround(o.qty / 0.0001);
        }
        return out;
    }

    static std::set<long long> ticks_of(std::initializer_list<double> prices) {
        std::set<long long> out;
        for (double p : prices) out.insert(std::llround(p / kTick));
        return out;
    }

    // The ladder as it stands after the initial quote, fully acknowledged.
    void open_ladder() {
        send_quote(100.00, 100.01);
        settle();
        ASSERT_EQ(resting_ticks(Side::BUY), ticks_of({100.00, 99.99, 99.98}))
            << "contiguous ladder at level_gap_ticks=1";
    }

    OrderStacker    stacker_;
    PositionTracker positions_;
    PnLCalculator   pnl_{positions_};
    RiskManager     risk_{positions_};
    slick::queue<uint8_t> md_q_{4096};
    slick::queue<uint8_t> order_q_{65536};
    std::map<std::string, Resting> venue_;
    uint64_t order_cursor_{0};
    uint64_t clock_{0};
};

// The bug: a config change with no quote behind it.
TEST_F(OrderStackerConfigTest, LevelGapChangeAppliesWithoutMarketData) {
    open_ladder();

    auto params = base_params();
    params["level_gap_ticks"] = 2;
    // No quote, no trade — exactly the state the live process was in: the book had not moved
    // since the ladder went up. Before the fix this produced nothing at all.
    stacker_.set_config(params);
    EXPECT_GT(settle(), 0) << "the config change must reach the venue on its own";

    EXPECT_EQ(resting_ticks(Side::BUY), ticks_of({100.00, 99.98, 99.96}))
        << "every other tick at level_gap_ticks=2";
    EXPECT_EQ(resting_ticks(Side::SELL), ticks_of({100.01, 100.03, 100.05}));
}

TEST_F(OrderStackerConfigTest, StackQtyChangeAppliesWithoutMarketData) {
    open_ladder();

    auto params = base_params();
    params["stack_qty"] = 0.002;   // top_qty stays 0.001, so only the two rungs resize

    stacker_.set_config(params);
    EXPECT_GT(settle(), 0);

    const std::map<long long, long long> expected{
        {10000, 10},   // 100.00 — level 0, still top_qty
        {9999,  20},   //  99.99 — rung 1 at the new stack_qty
        {9998,  20},   //  99.98 — rung 2
    };
    EXPECT_EQ(resting_qty_by_tick(Side::BUY), expected)
        << "level 0 keeps top_qty=0.001; both rungs move to stack_qty=0.002";
}

// Updates arriving back to back each land in full; the last one is what the ladder ends on.
TEST_F(OrderStackerConfigTest, LastOfSeveralUpdatesIsWhatTheLadderEndsOn) {
    open_ladder();

    auto params = base_params();
    params["level_gap_ticks"] = 2;
    stacker_.set_config(params);
    params["level_gap_ticks"] = 3;
    stacker_.set_config(params);
    settle();

    EXPECT_EQ(resting_ticks(Side::BUY), ticks_of({100.00, 99.97, 99.94}));
}

// A stopped strategy must not be quoted back onto the book by a config update.
TEST_F(OrderStackerConfigTest, ConfigUpdateDoesNotRevivePausedStrategy) {
    open_ladder();
    const auto ladder_size = venue_.size();
    ASSERT_GT(ladder_size, 0u);

    stacker_.stop();
    drain();      // discard the cancels stop() issues through the base class
    venue_.clear();

    auto params = base_params();
    params["level_gap_ticks"] = 2;
    stacker_.set_config(params);
    settle();

    EXPECT_TRUE(venue_.empty()) << "a paused strategy holds no ladder";
}


// The live shape from config/order_stacker.config.json (levels=4, slack_levels=1,
// top_qty != stack_qty), reconfigured the way the operator did it: stack_qty first, then
// level_gap_ticks. The dashboard showed an ask rung left one tick behind the touch afterwards,
// belonging to neither the old shape nor the new one. The ladder engine turned out to be
// innocent -- the stranded rung was a market-data bug in the simulator, which never published
// the level update a cancel produced -- so this pins down the sequence that was suspected.
TEST_F(OrderStackerConfigTest, StackQtyThenLevelGapLeavesNoStrayRung) {
    auto params = base_params();
    params["levels"]          = 4;
    params["level_gap_ticks"] = 1;
    params["stack_qty"]       = 0.001;
    params["top_qty"]         = 0.001;
    params["slack_levels"]    = 1;
    params["min_order_qty"]   = 0.0001;
    stacker_.set_config(params);

    send_quote(100.00, 100.01);
    settle();
    ASSERT_EQ(resting_ticks(Side::SELL), ticks_of({100.01, 100.02, 100.03, 100.04, 100.05}));
    ASSERT_EQ(resting_ticks(Side::BUY),  ticks_of({100.00, 99.99, 99.98, 99.97, 99.96}));

    params["stack_qty"] = 0.002;
    stacker_.set_config(params);
    settle();

    params["level_gap_ticks"] = 2;
    stacker_.set_config(params);
    settle();

    EXPECT_EQ(resting_ticks(Side::SELL), ticks_of({100.01, 100.03, 100.05, 100.07, 100.09}));
    EXPECT_EQ(resting_ticks(Side::BUY),  ticks_of({100.00, 99.98, 99.96, 99.94, 99.92}));
}


}  // namespace
