// OrderBookPublisher: from order changes in wire units to price levels, order-by-order
// updates, trades and status. A fake channel drives it here, so the rules every
// market-by-order broker shares are pinned down without any exchange's protocol.
// MdEventWriter: those outputs as events, chunked to fit their records.

#include <gtest/gtest.h>
#include <slick-trader/brokers/common/md_event_writer.hpp>
#include <slick-trader/brokers/common/order_book_publisher.hpp>
#include <slick-trader/brokers/common/price_scale.hpp>
#include <slick/stream_buffer_multiplexer.hpp>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace slick::trader;

namespace {

struct TestInstrument {
    std::string symbol;
    PriceScale scale = *PriceScale::from_ratio(PRICE_SCALE, 100);   // wire prices in hundredths
    SharedTopOfBook* top = nullptr;
};

struct Recorder {
    struct Levels {
        std::string symbol;
        std::vector<PriceLevel> levels;
        bool snapshot;
    };
    struct Orders {
        std::string symbol;
        std::vector<MboOrder> orders;
        bool snapshot;
    };
    void publish_trade(const TestInstrument& i, price_t price, qty_t qty, Side aggressor, uint64_t) {
        trades.push_back({i.symbol, price, qty, aggressor});
    }
    void publish_levels(const TestInstrument& i, const std::vector<PriceLevel>& l, bool snap, uint64_t, uint64_t) {
        levels.push_back({i.symbol, l, snap});
    }
    void publish_orders(const TestInstrument& i, const std::vector<MboOrder>& o, bool snap, uint64_t, uint64_t) {
        orders.push_back({i.symbol, o, snap});
    }
    void publish_status(const TestInstrument& i, TradingStatus s, bool cancel_allowed, std::string_view, uint64_t) {
        statuses.push_back({i.symbol, s, cancel_allowed});
    }

    struct Trade {
        std::string symbol;
        price_t price;
        qty_t qty;
        Side aggressor;
    };
    struct Status {
        std::string symbol;
        TradingStatus status;
        bool cancel_allowed;
    };
    std::vector<Trade> trades;
    std::vector<Levels> levels;
    std::vector<Orders> orders;
    std::vector<Status> statuses;
};

// A channel as a broker writes one: it knows which orders rest (here, a map) and whether
// its feed is in sequence.
class FakeChannel : public OrderBookPublisher<FakeChannel, TestInstrument, Recorder> {
public:
    using Base = OrderBookPublisher<FakeChannel, TestInstrument, Recorder>;
    using Base::Listing;

    explicit FakeChannel(Recorder& sink) : Base(sink, "test channel") {}

    [[nodiscard]] bool live() const noexcept { return live_; }
    template <typename F>
    void for_each_resting_order(int32_t key, F&& f) const {
        for (const auto& [id, order] : resting_) {
            if (order.first == key) {
                f(order.second);
            }
        }
    }

    void list(int32_t key, std::string symbol, SharedTopOfBook* top = nullptr) {
        add_instrument(key, TestInstrument{std::move(symbol), *PriceScale::from_ratio(PRICE_SCALE, 100), top});
    }
    using Base::book_gap;
    using Base::end_of_event;
    using Base::instruments_complete;

    void go_live() {
        live_ = true;
        book_rebuilt();
    }
    void lose_sequence() {
        live_ = false;
        book_gap();
    }

    void add(int32_t key, uint64_t id, Side side, int64_t price, int64_t qty, uint64_t priority = 0) {
        BookOrderChange u;
        u.order_id = id;
        u.priority = priority;
        u.action = BookOrderChange::Action::ADD;
        u.side = u.prev_side = side;
        u.price = u.prev_price = price;
        u.qty = qty;
        resting_[id] = {key, u};
        apply_order(key, u);
    }
    void move(int32_t key, uint64_t id, Side side, int64_t price, int64_t qty) {
        BookOrderChange u = resting_[id].second;
        u.action = BookOrderChange::Action::CHANGE;
        u.prev_side = u.side;
        u.prev_price = u.price;
        u.prev_qty = u.qty;
        u.side = side;
        u.price = price;
        u.qty = qty;
        // As a snapshot lists it: an add, with nothing there before.
        auto& resting = resting_[id].second;
        resting = u;
        resting.action = BookOrderChange::Action::ADD;
        resting.prev_side = side;
        resting.prev_price = price;
        resting.prev_qty = 0;
        apply_order(key, u);
    }
    void remove(int32_t key, uint64_t id) {
        BookOrderChange u = resting_[id].second;
        u.action = BookOrderChange::Action::REMOVE;
        u.prev_side = u.side;
        u.prev_price = u.price;
        u.prev_qty = u.qty;
        u.qty = 0;
        resting_.erase(id);
        apply_order(key, u);
    }
    void print(int32_t key, int64_t price, int64_t qty, Side aggressor) { trade(key, price, qty, aggressor, 0); }
    void status(int32_t key, TradingStatus s, bool cancel_allowed) {
        set_status(*state_of(key), s, cancel_allowed, "", 0);
    }

private:
    bool live_ = false;
    std::map<uint64_t, std::pair<int32_t, BookOrderChange>> resting_;
};

class OrderBookPublisherTest : public ::testing::Test {
protected:
    void SetUp() override {
        channel_.list(1, "BRN", &brn_top_);
        channel_.list(2, "GAS");
    }

    Recorder sink_;
    FakeChannel channel_{sink_};
    SharedTopOfBook brn_top_;
};

} // namespace

TEST_F(OrderBookPublisherTest, ASubscriptionBeforeTheInstrumentsAreKnownWaitsForThem) {
    EXPECT_EQ(channel_.subscribe("BRN", SubscriptionType::ORDER_BOOK), FakeChannel::Listing::PENDING);
    channel_.instruments_complete();
    EXPECT_EQ(channel_.subscribe("XYZ", SubscriptionType::ORDER_BOOK), FakeChannel::Listing::UNKNOWN);
    channel_.add(1, 10, Side::BUY, 7523, 2);
    channel_.go_live();
    ASSERT_EQ(sink_.levels.size(), 1u) << "the waiting subscriber gets its snapshot when the book is built";
    EXPECT_TRUE(sink_.levels[0].snapshot);
    ASSERT_EQ(sink_.levels[0].levels.size(), 1u);
    EXPECT_EQ(sink_.levels[0].levels[0].price, to_price_t(75.23));
    EXPECT_EQ(sink_.levels[0].levels[0].quantity, to_qty_t(2));
}

TEST_F(OrderBookPublisherTest, EachEventPublishesOnlyTheLevelsItChanged) {
    channel_.instruments_complete();
    channel_.go_live();
    EXPECT_EQ(channel_.subscribe("BRN", SubscriptionType::ORDER_BOOK), FakeChannel::Listing::LISTED);
    sink_.levels.clear();

    channel_.add(1, 10, Side::BUY, 7523, 2);
    channel_.add(1, 11, Side::BUY, 7523, 1);
    channel_.add(2, 20, Side::SELL, 500, 1);   // not subscribed
    channel_.end_of_event();
    ASSERT_EQ(sink_.levels.size(), 1u);
    EXPECT_FALSE(sink_.levels[0].snapshot);
    ASSERT_EQ(sink_.levels[0].levels.size(), 1u);
    EXPECT_EQ(sink_.levels[0].levels[0].quantity, to_qty_t(3));
    EXPECT_EQ(sink_.levels[0].levels[0].num_orders, 2u);

    channel_.move(1, 10, Side::BUY, 7524, 2);
    channel_.end_of_event();
    ASSERT_EQ(sink_.levels.size(), 2u);
    EXPECT_EQ(sink_.levels[1].levels.size(), 2u) << "the level left and the level joined";
}

TEST_F(OrderBookPublisherTest, OrderSubscribersSeeAddsModifiesRemovesAndSideChanges) {
    channel_.instruments_complete();
    channel_.go_live();
    channel_.subscribe("BRN", SubscriptionType::MBO);
    ASSERT_EQ(sink_.orders.size(), 1u);
    EXPECT_TRUE(sink_.orders[0].snapshot) << "an empty book is a snapshot too";
    sink_.orders.clear();

    channel_.add(1, 10, Side::BUY, 7523, 2, 5);
    channel_.move(1, 10, Side::BUY, 7523, 1);
    channel_.move(1, 10, Side::SELL, 7530, 1);
    channel_.remove(1, 10);
    channel_.end_of_event();
    ASSERT_EQ(sink_.orders.size(), 1u);
    const auto& o = sink_.orders[0].orders;
    ASSERT_EQ(o.size(), 5u);
    EXPECT_EQ(o[0].action, MboAction::ADD);
    EXPECT_EQ(o[0].priority, 5u);
    EXPECT_EQ(o[1].action, MboAction::MODIFY);
    EXPECT_EQ(o[2].action, MboAction::REMOVE) << "a side change leaves one side";
    EXPECT_EQ(o[3].action, MboAction::ADD) << "and joins the other";
    EXPECT_EQ(o[3].side, Side::SELL);
    EXPECT_EQ(o[4].action, MboAction::REMOVE);
}

TEST_F(OrderBookPublisherTest, AGapMakesTheTopUnknownAndARebuildSnapshotsEverySubscriber) {
    channel_.instruments_complete();
    channel_.go_live();
    channel_.subscribe("BRN", SubscriptionType::ORDER_BOOK);
    channel_.subscribe("BRN", SubscriptionType::MBO);
    channel_.add(1, 10, Side::BUY, 7523, 2);
    channel_.add(1, 11, Side::SELL, 7525, 1);
    channel_.end_of_event();
    EXPECT_EQ(brn_top_.bid.load(), to_price_t(75.23));
    EXPECT_EQ(brn_top_.ask.load(), to_price_t(75.25));

    channel_.lose_sequence();
    EXPECT_EQ(brn_top_.bid.load(), SharedTopOfBook::UNKNOWN);
    channel_.remove(1, 11);
    channel_.end_of_event();
    EXPECT_EQ(brn_top_.ask.load(), SharedTopOfBook::UNKNOWN) << "not while out of sequence";

    sink_.levels.clear();
    sink_.orders.clear();
    channel_.go_live();
    EXPECT_EQ(brn_top_.ask.load(), SharedTopOfBook::EMPTY);
    ASSERT_EQ(sink_.levels.size(), 1u);
    EXPECT_TRUE(sink_.levels[0].snapshot);
    ASSERT_EQ(sink_.orders.size(), 1u);
    EXPECT_TRUE(sink_.orders[0].snapshot);
    EXPECT_EQ(sink_.orders[0].orders.size(), 1u);
}

TEST_F(OrderBookPublisherTest, TradesGoToTradeSubscribersOnlyAndStatusOnlyWhenItChanges) {
    channel_.instruments_complete();
    channel_.go_live();
    channel_.print(1, 7523, 3, Side::SELL);
    EXPECT_TRUE(sink_.trades.empty());
    channel_.subscribe("BRN", SubscriptionType::TRADE);
    channel_.print(1, 7523, 3, Side::SELL);
    ASSERT_EQ(sink_.trades.size(), 1u);
    EXPECT_EQ(sink_.trades[0].price, to_price_t(75.23));
    EXPECT_EQ(sink_.trades[0].qty, to_qty_t(3));
    channel_.print(1, std::numeric_limits<int64_t>::min(), 3, Side::SELL);   // a null price: dropped
    EXPECT_EQ(sink_.trades.size(), 1u);

    channel_.status(1, TradingStatus::OPEN, true);
    channel_.status(1, TradingStatus::OPEN, true);
    channel_.status(1, TradingStatus::OPEN, false);
    EXPECT_EQ(sink_.statuses.size(), 2u);
}

TEST_F(OrderBookPublisherTest, UnsubscribedInstrumentsStillKeepTheirLevelsForALaterSubscriber) {
    channel_.instruments_complete();
    channel_.go_live();
    channel_.add(2, 20, Side::SELL, 500, 4);
    channel_.end_of_event();
    EXPECT_TRUE(sink_.levels.empty());
    channel_.subscribe("GAS", SubscriptionType::ORDER_BOOK);
    ASSERT_EQ(sink_.levels.size(), 1u);
    EXPECT_EQ(sink_.levels[0].levels[0].quantity, to_qty_t(4));
}

TEST(MdEventWriterTest, ABookLargerThanOneRecordIsChunkedAndAnEmptySnapshotStillGoesOut) {
    slick::stream_buffer_multiplexer mux(1024);
    MdEventWriter writer(BrokerId::ICE);
    writer.open(mux, nlohmann::json{{"queue_size", 1u << 22}, {"control_size", 4096}});
    uint64_t cursor = mux.initial_reading_index();

    std::vector<PriceLevel> levels(MdEventWriter::MAX_LEVELS_PER_EVENT + 1, PriceLevel(1, 1, Side::BUY));
    writer.levels("BRN", levels, true, 0, 1);
    std::vector<MboOrder> none;
    writer.orders("BRN", none, true, 0, 1);

    std::vector<std::pair<uint16_t, bool>> level_records;
    std::vector<std::pair<uint16_t, bool>> order_records;
    while (auto record = mux.read(cursor)) {
        const auto* event = reinterpret_cast<const Event*>(record.data);
        EXPECT_EQ(event->broker_id, BrokerId::ICE);
        if (event->type == EventType::LEVEL2_UPDATE) {
            const auto* l2 = reinterpret_cast<const Level2UpdateEvent*>(event);
            level_records.emplace_back(l2->num_levels, l2->is_snapshot);
        } else if (event->type == EventType::MBO_UPDATE) {
            const auto* mbo = reinterpret_cast<const MboUpdateEvent*>(event);
            order_records.emplace_back(mbo->num_orders, mbo->is_snapshot && mbo->is_last);
        }
    }
    ASSERT_EQ(level_records.size(), 2u);
    EXPECT_EQ(level_records[0], std::make_pair(uint16_t{1024}, true));
    EXPECT_EQ(level_records[1], std::make_pair(uint16_t{1}, false)) << "only the first part clears the book";
    ASSERT_EQ(order_records.size(), 1u);
    EXPECT_EQ(order_records[0], std::make_pair(uint16_t{0}, true));
    EXPECT_EQ(md_producer_id(BrokerId::ICE), 11u);
    EXPECT_EQ(order_producer_id(BrokerId::ICE), 12u);
}
