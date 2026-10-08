// Regression test: the market maker must keep at most one live order per side.
//
// Bug: Coinbase answers an overlapping edit with {"edit_failure_reason":"UNKNOWN"}, which the
// connector republishes as an OrderRejectEvent. on_reject() reset the quote slot for any reject,
// but a failed *edit* leaves the original order resting — so the slot went EMPTY, update_quotes()
// placed a second order beside the live one, and each further edit failure added another. The UI
// showed seven MarketMaker orders where there should have been two.
//
// The overlapping edits came from the same handler pair: while a modify was in flight the venue
// kept streaming the order's pre-edit price as an OPEN update, and on_ack() took that as the
// modify ack, dropped the slot to ACTIVE at the old price, and let the next tick fire a second
// edit on top of the first.
//
// market_maker.cpp is a self-contained plugin translation unit with no header, so the strategy is
// pulled in directly here and driven through the public IStrategy interface.

#include <gtest/gtest.h>
#include <slick-trader/core/broker_status.hpp>
#include <slick-trader/core/pnl_calculator.hpp>
#include <slick-trader/core/position_tracker.hpp>
#include <slick-trader/core/risk_manager.hpp>
#include <slick/queue.hpp>

#include "../strategies/market_maker.cpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char* kInstrument = "BTC-USD";

// One order request as the broker connector would see it off the queue.
struct Request {
    EventType   type;
    std::string client_order_id;
    std::string order_id;
    Side        side{Side::BUY};
    double      price{0.0};
};

// The live state of one side, as the exchange would hold it.
struct RestingOrder {
    std::string client_order_id;
    std::string order_id;
    Side        side{Side::BUY};
    double      price{0.0};
    double      qty{0.005};
};

// Exposes the order bookkeeping StrategyBase keeps protected.
class TestableMarketMaker : public MarketMaker {
public:
    using MarketMaker::on_quote;  // MarketMaker narrows the IStrategy hook to protected
    using StrategyBase::get_order;
    using StrategyBase::get_orders;
};

class MarketMakerQuoteSlotTest : public ::testing::Test {
protected:
    virtual nlohmann::json strategy_params() const {
        return {
            {"instrument",            kInstrument},
            {"broker",                "COINBASE"},
            {"order_size",            0.005},
            {"max_inventory",         0.010001},
            {"min_profit_usd",        0.5},    // half_spread = 0.5 / (2 * 0.005) = $50
            {"refresh_threshold_bps", 1.0},
            {"inventory_skew_factor", 0.0},   // keep the quote arithmetic predictable: fair value
            {"imbalance_sensitivity", 0.0},   // is the mid and the targets sit symmetrically
            {"trend_sensitivity",     0.0},   // around it
            {"fair_value_mode",       "SIMPLE_MID"},
        };
    }

    void SetUp() override {
        auto& reg = BrokerStatusRegistry::instance();
        reg.set_name(BrokerId::COINBASE, "Coinbase");
        reg.set_order_enabled(BrokerId::COINBASE, true);
        reg.set_order_connected(BrokerId::COINBASE, true);

        mm_.initialize(strategy_params(), positions_, pnl_, risk_, md_q_, order_q_);
        mm_.start();
    }

    // Top of book; equal sizes so SIMPLE_MID puts fair value exactly at the mid.
    void send_quote(double bid, double ask) {
        QuoteEvent ev;
        ev.timestamp_ns = ++clock_;
        ev.set_instrument(kInstrument);
        ev.bid_price    = to_price_t(bid);
        ev.ask_price    = to_price_t(ask);
        ev.bid_quantity = to_qty_t(1.0);
        ev.ask_quantity = to_qty_t(1.0);
        mm_.on_quote(ev);
    }

    void send_ack(const RestingOrder& o) {
        OrderAckEvent ev;
        ev.timestamp_ns  = ++clock_;
        ev.set_instrument(kInstrument);
        ev.set_client_order_id(o.client_order_id);
        ev.set_order_id(o.order_id);
        ev.order_type    = OrderType::LIMIT;
        ev.time_in_force = TimeInForce::GTC;
        ev.side          = o.side;
        ev.price         = to_price_t(o.price);
        ev.quantity      = to_qty_t(o.qty);
        ev.status        = OrderStatus::ACKNOWLEDGED;
        mm_.on_order_ack(ev);
    }

    // What the connector publishes when the exchange refuses an edit: the order_id of an order
    // that is still resting, and an opaque reason.
    void send_reject(const RestingOrder& o, const char* reason) {
        OrderRejectEvent ev;
        ev.timestamp_ns = ++clock_;
        ev.set_instrument(kInstrument);
        ev.set_client_order_id(o.client_order_id);
        ev.set_order_id(o.order_id);
        ev.set_reason(reason);
        mm_.on_order_reject(ev);
    }

    void send_edit_reject(const RestingOrder& o) {
        send_reject(o, R"({"edit_failure_reason":"UNKNOWN"})");
    }

    // Moves the strategy's tracked inventory without touching its quote slots, so a test can
    // put it over the limit and watch update_quotes() react.
    void fill_position(Side side, double qty, double price) {
        OrderFillEvent ev;
        ev.timestamp_ns       = ++clock_;
        ev.set_instrument(kInstrument);
        ev.set_order_id("external");
        ev.side               = side;
        ev.fill_price         = to_price_t(price);
        ev.fill_quantity      = to_qty_t(qty);
        ev.cumulative_filled  = to_qty_t(qty);
        ev.remaining_quantity = 0;
        positions_.on_fill("MarketMaker", ev);
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
                break;
            }
            case EventType::MODIFY_ORDER: {
                const auto& e = *reinterpret_cast<const ModifyOrderEvent*>(data);
                r.client_order_id = e.client_order_id;
                r.order_id        = e.order_id;
                r.side            = e.side;
                r.price           = from_price_t(e.new_price);
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

    static int count(const std::vector<Request>& rs, EventType t) {
        int n = 0;
        for (const auto& r : rs) n += (r.type == t);
        return n;
    }

    static int count_side(const std::vector<Request>& rs, EventType t, Side side) {
        int n = 0;
        for (const auto& r : rs) n += (r.type == t && r.side == side);
        return n;
    }

    static const RestingOrder& side_of(const std::vector<RestingOrder>& rs, Side side) {
        for (const auto& r : rs) {
            if (r.side == side) return r;
        }
        throw std::runtime_error("no resting order on that side");
    }

    // Sends a quote and acks whatever the strategy did with it, so the slots return to ACTIVE and
    // the next quote can act. Lets a test run the strategy forward over many ticks.
    std::vector<Request> pump(double bid, double ask) {
        send_quote(bid, ask);
        auto requests = drain();
        for (const auto& r : requests) {
            if (r.type != EventType::NEW_ORDER && r.type != EventType::MODIFY_ORDER) continue;
            send_ack({r.client_order_id,
                      r.order_id.empty() ? "broker-" + r.client_order_id : r.order_id,
                      r.side, r.price});
        }
        return requests;
    }

    // Quote both sides and ack them, returning the two resting orders.
    std::vector<RestingOrder> open_both_sides(double bid, double ask) {
        send_quote(bid, ask);
        const auto requests = drain();
        EXPECT_EQ(count(requests, EventType::NEW_ORDER), 2) << "one order per side";

        std::vector<RestingOrder> resting;
        for (const auto& r : requests) {
            if (r.type != EventType::NEW_ORDER) continue;
            resting.push_back({r.client_order_id, "broker-" + r.client_order_id, r.side, r.price});
            send_ack(resting.back());
        }
        return resting;
    }

    PositionTracker positions_;
    PnLCalculator   pnl_{positions_};
    RiskManager     risk_{positions_};
    slick::queue<uint8_t> md_q_{4096};
    slick::queue<uint8_t> order_q_{65536};
    uint64_t order_cursor_{0};
    uint64_t clock_{0};

    TestableMarketMaker mm_;
};

} // namespace

TEST_F(MarketMakerQuoteSlotTest, QuotesBothSidesOnceThenRepricesInPlace) {
    const auto resting = open_both_sides(78000.0, 78002.0);
    ASSERT_EQ(resting.size(), 2u);
    ASSERT_EQ(mm_.get_orders().size(), 2u);
    drain();

    // Well past the 1 bp refresh threshold: both sides reprice, neither re-submits.
    send_quote(78200.0, 78202.0);
    const auto after = drain();
    EXPECT_EQ(count(after, EventType::NEW_ORDER), 0) << "an ACTIVE slot modifies in place";
    EXPECT_EQ(count(after, EventType::MODIFY_ORDER), 2);
    EXPECT_EQ(mm_.get_orders().size(), 2u);
}

// The bug, end to end.
TEST_F(MarketMakerQuoteSlotTest, RejectedEditDoesNotOrphanTheRestingOrder) {
    const auto resting = open_both_sides(78000.0, 78002.0);
    ASSERT_EQ(resting.size(), 2u);
    drain();

    send_quote(78200.0, 78202.0);
    ASSERT_EQ(count(drain(), EventType::MODIFY_ORDER), 2) << "both slots should be PENDING_MODIFY";

    // The exchange refuses both edits. Both orders are still resting at their acked prices.
    for (const auto& o : resting) send_edit_reject(o);

    EXPECT_EQ(mm_.get_orders().size(), 2u)
        << "a failed edit must not make the strategy place a replacement";

    // The next tick retries the reprice on the same orders rather than adding new ones.
    send_quote(78200.0, 78202.0);
    const auto retry = drain();
    EXPECT_EQ(count(retry, EventType::NEW_ORDER), 0);
    EXPECT_EQ(count(retry, EventType::MODIFY_ORDER), 2) << "slots should be ACTIVE again and retry";
    EXPECT_EQ(mm_.get_orders().size(), 2u);

    for (const auto& r : retry) {
        EXPECT_TRUE(r.client_order_id == resting[0].client_order_id ||
                    r.client_order_id == resting[1].client_order_id)
            << "retry must target the orders that are actually resting";
    }
}

// A reject for an order that never reached the book is terminal — the slot has to clear so the
// side gets requoted, otherwise the strategy stops quoting entirely.
TEST_F(MarketMakerQuoteSlotTest, RejectedNewOrderClearsTheSlotAndRequotes) {
    send_quote(78000.0, 78002.0);
    const auto placed = drain();
    ASSERT_EQ(count(placed, EventType::NEW_ORDER), 2);

    for (const auto& r : placed) {
        OrderRejectEvent ev;
        ev.timestamp_ns = ++clock_;
        ev.set_instrument(kInstrument);
        ev.set_client_order_id(r.client_order_id);
        ev.set_reason("POST_ONLY_WOULD_CROSS");
        mm_.on_order_reject(ev);
    }

    send_quote(78000.0, 78002.0);
    EXPECT_EQ(count(drain(), EventType::NEW_ORDER), 2) << "both sides should be requoted";
}

// While an edit is in flight the venue keeps publishing the order's pre-edit price. Acting on it
// produces a second, overlapping edit — the thing that provokes the edit failure to begin with.
TEST_F(MarketMakerQuoteSlotTest, PreEditPriceEchoDoesNotCompleteAModify) {
    const auto resting = open_both_sides(78000.0, 78002.0);
    ASSERT_EQ(resting.size(), 2u);
    drain();

    send_quote(78200.0, 78202.0);
    ASSERT_EQ(count(drain(), EventType::MODIFY_ORDER), 2);

    // Late OPEN updates carrying the price from before the edit.
    for (const auto& o : resting) send_ack(o);

    send_quote(78200.0, 78202.0);
    const auto after = drain();
    EXPECT_EQ(count(after, EventType::MODIFY_ORDER), 0)
        << "slots must stay PENDING_MODIFY until the edit is confirmed";
    EXPECT_EQ(count(after, EventType::NEW_ORDER), 0);
}

// The venue rounds to its own tick, so the confirming ack never matches bit-exactly. It still has
// to close the PENDING_MODIFY, or the slot stalls and that side stops repricing forever.
TEST_F(MarketMakerQuoteSlotTest, TickRoundedAckConfirmsTheModify) {
    const auto resting = open_both_sides(78000.0, 78002.0);
    ASSERT_EQ(resting.size(), 2u);
    drain();

    send_quote(78200.0, 78202.0);
    const auto edits = drain();
    ASSERT_EQ(count(edits, EventType::MODIFY_ORDER), 2);

    // Ack each edit at the requested price rounded to the cent, as the venue would.
    for (const auto& e : edits) {
        send_ack({e.client_order_id, e.order_id, e.side, std::round(e.price * 100.0) / 100.0});
    }

    // Both slots are ACTIVE at the new prices, so a further move reprices in place.
    send_quote(78400.0, 78402.0);
    const auto after = drain();
    EXPECT_EQ(count(after, EventType::MODIFY_ORDER), 2) << "a tick-rounded ack must confirm the edit";
    EXPECT_EQ(count(after, EventType::NEW_ORDER), 0);
}

// An ack that does not settle the pending edit still says where the order rests, and the slot has
// to record it: on a venue that accepts several edits in flight (CME) the state an order falls
// back to when one is rejected is the last edit the venue *accepted*, not the price it started at.
// Getting this wrong makes the strategy reprice against a phantom price after every failed edit.
TEST_F(MarketMakerQuoteSlotTest, NonConfirmingAckStillRecordsWhereTheOrderRests) {
    const auto resting = open_both_sides(78000.0, 78002.0);
    ASSERT_EQ(resting.size(), 2u);
    const auto& bid = side_of(resting, Side::BUY);
    drain();

    // Bid target moves 78000-50 → 78200-50; the slot goes PENDING_MODIFY on 78150.
    send_quote(78200.0, 78202.0);
    const auto edits = drain();
    ASSERT_EQ(count_side(edits, EventType::MODIFY_ORDER, Side::BUY), 1);

    // The venue reports the order resting at 78100 — an earlier edit it accepted, neither the
    // original 77950 nor the 78150 still in flight — and then rejects the in-flight one.
    send_ack({bid.client_order_id, bid.order_id, Side::BUY, 78100.0});
    send_edit_reject(bid);

    // The bid is back to ACTIVE at 78100. A target of 78100 is already correct, so the strategy
    // must leave it alone; against the stale 77950 it would have fired a pointless edit.
    send_quote(78149.0, 78151.0);
    EXPECT_EQ(count_side(drain(), EventType::MODIFY_ORDER, Side::BUY), 0)
        << "slot should have recorded the venue-confirmed 78100, not fallen back to 77950";

    // And it does still reprice once the target genuinely moves away from 78100.
    send_quote(78249.0, 78251.0);
    EXPECT_EQ(count_side(drain(), EventType::MODIFY_ORDER, Side::BUY), 1);
}

// A refused cancel leaves the order resting just as a refused edit does, so the slot must not be
// cleared. It is the more dangerous of the two: the slot only cancels when inventory is over the
// limit, so an orphan here sits in the book on the side the strategy was trying to stop quoting.
TEST_F(MarketMakerQuoteSlotTest, RejectedCancelDoesNotOrphanTheRestingOrder) {
    const auto resting = open_both_sides(78000.0, 78002.0);
    ASSERT_EQ(resting.size(), 2u);
    const auto& bid = side_of(resting, Side::BUY);
    drain();

    // Long past max_inventory, so update_quotes() pulls the bid rather than repricing it.
    fill_position(Side::BUY, 0.011, 78000.0);
    send_quote(78000.0, 78002.0);
    ASSERT_EQ(count_side(drain(), EventType::CANCEL_ORDER, Side::BUY), 1)
        << "over the inventory limit the bid should be cancelled";

    // The venue refuses the cancel — the order is still resting.
    send_reject(bid, "UNKNOWN_CANCEL_FAILURE_REASON");

    send_quote(78000.0, 78002.0);
    const auto after = drain();
    EXPECT_EQ(count_side(after, EventType::CANCEL_ORDER, Side::BUY), 1)
        << "slot should be ACTIVE again and retry the cancel";
    EXPECT_EQ(count_side(after, EventType::NEW_ORDER, Side::BUY), 0);

    // Refuse the retry too, then let inventory fall back under the limit: the strategy resumes
    // quoting that side and must reprice the order that is still resting, not place a second
    // one next to it.
    send_reject(bid, "UNKNOWN_CANCEL_FAILURE_REASON");
    fill_position(Side::SELL, 0.011, 78000.0);
    send_quote(78300.0, 78302.0);
    const auto resumed = drain();
    EXPECT_EQ(count_side(resumed, EventType::NEW_ORDER, Side::BUY), 0)
        << "the original bid is still live; a replacement would leak an order";
    EXPECT_EQ(count_side(resumed, EventType::MODIFY_ORDER, Side::BUY), 1);
}

// bps mode scales by |reference|. A signed reference yields a negative tolerance, and a distance
// compared against a negative tolerance always reads as "too far" or never as "too far" depending
// on the comparison — either way the strategy loses the ability to tell a move from a non-move.
TEST(MarketMakerPriceMath, BpsToleranceScalesByAbsoluteReference) {
    constexpr double kBps = 1.0 / 10000.0;

    EXPECT_NEAR(price_tolerance(100.0, kBps, 0.0), 0.01, 1e-12);

    // A calendar spread quoted at -2.00 has the same tolerance as one quoted at +2.00.
    EXPECT_NEAR(price_tolerance(-2.00, kBps, 0.0), price_tolerance(2.00, kBps, 0.0), 1e-15);
    EXPECT_GT(price_tolerance(-2.00, kBps, 0.0), 0.0) << "tolerance must never go negative";

    // At zero there is nothing to take a fraction of, so the tolerance collapses and every move
    // counts — safe, but the reason a bps threshold is the wrong tool near zero.
    EXPECT_DOUBLE_EQ(price_tolerance(0.0, kBps, 0.0), 0.0);
}

// Tick mode is the fix for that: a fixed distance that does not care where the price sits.
TEST(MarketMakerPriceMath, TickToleranceOverridesBpsAtEveryPriceLevel) {
    constexpr double kBps  = 1.0 / 10000.0;
    constexpr double kTick = 0.25;

    EXPECT_DOUBLE_EQ(price_tolerance(100.0, kBps, kTick), kTick);
    EXPECT_DOUBLE_EQ(price_tolerance(-2.00, kBps, kTick), kTick);
    EXPECT_DOUBLE_EQ(price_tolerance(0.0, kBps, kTick), kTick)
        << "a zero price still has a usable tolerance in tick mode";
}

namespace {

// A futures calendar spread: quoted around -2.00, ticks in cents, reprices on a fixed number of
// ticks rather than a fraction of a price that is barely distinguishable from zero.
class MarketMakerNegativePriceTest : public MarketMakerQuoteSlotTest {
protected:
    static constexpr double kTick       = 0.01;
    static constexpr double kHalfSpread = 0.05;

    nlohmann::json strategy_params() const override {
        auto p = MarketMakerQuoteSlotTest::strategy_params();
        p["allow_negative_price"]    = true;
        p["tick_size"]               = kTick;
        p["refresh_threshold_ticks"] = 2.0;                          // tolerance = 0.02
        p["min_profit_usd"]          = kHalfSpread * 2.0 * 0.005;    // half_spread = 0.05
        return p;
    }
};

} // namespace

// Without allow_negative_price the strategy treats a sub-zero book as bad data — the historical
// behaviour, and still the right default for an instrument that cannot trade there.
TEST_F(MarketMakerQuoteSlotTest, NegativeBookIsRejectedByDefault) {
    send_quote(-2.02, -1.98);
    EXPECT_EQ(count(drain(), EventType::NEW_ORDER), 0);

    send_quote(0.0, 1.0);
    EXPECT_EQ(count(drain(), EventType::NEW_ORDER), 0) << "a zero bid is still bad data here";
}

TEST_F(MarketMakerNegativePriceTest, QuotesBothSidesAroundANegativeMid) {
    send_quote(-2.02, -1.98);   // mid -2.00
    const auto placed = drain();
    ASSERT_EQ(count(placed, EventType::NEW_ORDER), 2);

    double bid_price = 0.0, ask_price = 0.0;
    for (const auto& r : placed) {
        (r.side == Side::BUY ? bid_price : ask_price) = r.price;
    }
    EXPECT_NEAR(bid_price, -2.05, 1e-9) << "bid sits a half spread below the negative mid";
    EXPECT_NEAR(ask_price, -1.95, 1e-9);
    EXPECT_LT(bid_price, ask_price);
}

TEST_F(MarketMakerNegativePriceTest, RepricesOnTicksNotOnAFractionOfThePrice) {
    const auto resting = open_both_sides(-2.02, -1.98);
    ASSERT_EQ(resting.size(), 2u);
    drain();

    // One tick of drift: inside the two-tick tolerance, so the quotes stay put. A bps threshold
    // at this price level would be worth a hundredth of a cent and would have repriced.
    send_quote(-2.01, -1.97);   // mid -1.99, bid target -2.04, one tick from -2.05
    EXPECT_EQ(count(drain(), EventType::MODIFY_ORDER), 0);

    // Five ticks: past the tolerance on both sides.
    send_quote(-1.97, -1.93);   // mid -1.95, bid target -2.00
    const auto after = drain();
    EXPECT_EQ(count(after, EventType::MODIFY_ORDER), 2);
    EXPECT_EQ(count(after, EventType::NEW_ORDER), 0);
}

namespace {

// Same spread, with the trend term switched on and short EMA windows so it warms up quickly.
class MarketMakerTrendTest : public MarketMakerNegativePriceTest {
protected:
    nlohmann::json strategy_params() const override {
        auto p = MarketMakerNegativePriceTest::strategy_params();
        p["trend_sensitivity"] = 0.5;
        p["ema_fast_periods"]  = 3;
        p["ema_slow_periods"]  = 10;
        return p;
    }
};

} // namespace

// The trend term normalises an absolute EMA drift into a [-1, 1] signal, and it used to divide by
// the signed fair value. Below zero that inverts the signal and leans the quotes *against* the
// trend — the worst possible direction for a market maker, and silent, because the quotes still
// look plausible.
TEST_F(MarketMakerTrendTest, TrendLeansQuotesWithTheTrendOnANegativeMid) {
    // Steady upward drift, -2.30 → -2.01, warming the EMAs well past the 10-tick slow window.
    for (int i = 0; i <= 29; ++i) {
        const double mid = -2.30 + 0.01 * i;
        pump(mid - 0.01, mid + 0.01);
    }

    // A five-tick jump, past the two-tick tolerance, so both sides reprice.
    const auto requests = pump(-1.96, -1.94);   // mid -1.95
    ASSERT_EQ(count(requests, EventType::MODIFY_ORDER), 2);

    double bid_price = 0.0;
    for (const auto& r : requests) {
        if (r.side == Side::BUY) bid_price = r.price;
    }

    // With no trend the bid would sit a half spread under the mid, at -2.00. The market is
    // rising, so the reservation price leans up and the bid must land above that — but never
    // through the mid, which is the lean cap at trend_sensitivity 1.0.
    EXPECT_GT(bid_price, -2.00) << "quotes leaned against a rising market";
    EXPECT_LE(bid_price, -1.95) << "and never through the mid";
}
