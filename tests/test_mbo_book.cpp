// The market-by-order book StrategyManager keeps from MboUpdateEvents, and the queue
// position a strategy reads from it for one of its own orders.

#include <gtest/gtest.h>

#include <slick-trader/core/broker_status.hpp>
#include <slick-trader/core/pnl_calculator.hpp>
#include <slick-trader/core/position_tracker.hpp>
#include <slick-trader/core/risk_manager.hpp>
#include <slick-trader/core/strategy_manager.hpp>
#include <slick-trader/events/mbo_update_event.hpp>
#include <slick-trader/market_data/queue_position.hpp>
#include <slick-trader/strategy/strategy_base.hpp>
#include <slick/queue.hpp>
#include <slick/stream_buffer_multiplexer.hpp>

#include <memory>
#include <new>
#include <string>
#include <vector>

using namespace slick::trader;

namespace {

class BookStrategy : public StrategyBase {
public:
    BookStrategy() : StrategyBase("Book", "1.0.0") {}

    using StrategyBase::get_queue_position;
    using StrategyBase::submit_order;

    void on_order_book_l3(std::string_view instrument, const slick::orderbook::OrderBookL3& book) override {
        ++books;
        last_instrument = std::string(instrument);
        orders_in_last_book = book.orderCount();
    }

    void on_quote(const QuoteEvent& event) override {
        ++quotes;
        last_quote = event;
    }

    int books = 0;
    int quotes = 0;
    std::string last_instrument;
    size_t orders_in_last_book = 0;
    QuoteEvent last_quote;
};

MboOrder add(uint64_t id, Side side, double price, double quantity, uint64_t priority) {
    return MboOrder{id, priority, to_price_t(price), to_qty_t(quantity), side, MboAction::ADD};
}

MboOrder modify(uint64_t id, Side side, double price, double quantity, uint64_t priority) {
    return MboOrder{id, priority, to_price_t(price), to_qty_t(quantity), side, MboAction::MODIFY};
}

MboOrder removal(uint64_t id) {
    return MboOrder{id, 0, 0, 0, Side::BUY, MboAction::REMOVE};
}

class MboBookTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto& reg = BrokerStatusRegistry::instance();
        reg.set_order_enabled(BrokerId::CME, true);
        reg.set_order_connected(BrokerId::CME, true);
        strategy_->initialize(nlohmann::json::object(), positions_, pnl_, risk_, md_q_, order_q_);
        strategy_->start();
        manager_.add_strategy(strategy_);
    }

    // Dispatches one record exactly as the worker thread would read it off the bus.
    void publish(const std::vector<MboOrder>& orders, bool is_snapshot = false, bool is_last = true,
                 const char* instrument = "ESZ6") {
        std::vector<uint8_t> record(MboUpdateEvent::size_for(orders.size()));
        auto* event = new (record.data()) MboUpdateEvent();
        event->broker_id = BrokerId::CME;
        event->set_instrument(instrument);
        event->event_time_ns = ++clock_;
        event->is_snapshot = is_snapshot;
        event->is_last = is_last;
        event->num_orders = static_cast<uint16_t>(orders.size());
        for (size_t i = 0; i < orders.size(); ++i) {
            event->orders[i] = orders[i];
        }
        manager_.process_event(event);
    }

    const slick::orderbook::OrderBookL3& book(const char* instrument = "ESZ6") {
        const auto* found = manager_.get_order_book_l3(instrument);
        EXPECT_NE(found, nullptr);
        static const slick::orderbook::OrderBookL3 empty(0);
        return found ? *found : empty;
    }

    PositionTracker positions_;
    PnLCalculator   pnl_{positions_};
    RiskManager     risk_{positions_};
    slick::stream_buffer_multiplexer mux_{1024};
    slick::queue<uint8_t> md_q_{4096};
    slick::queue<uint8_t> order_q_{65536};
    StrategyManager manager_{nlohmann::json{{"TOB_queue_size", 65536}, {"TOB_control_size", 4096}},
                             positions_, pnl_, risk_, mux_, md_q_, order_q_};
    std::shared_ptr<BookStrategy> strategy_ = std::make_shared<BookStrategy>();
    uint64_t clock_ = 1'000;
};

} // namespace

TEST_F(MboBookTest, QueuePositionCountsWhatTradesFirst) {
    publish({add(101, Side::BUY, 1000.00, 5, 1),
             add(102, Side::BUY, 1000.00, 3, 2),
             add(103, Side::BUY, 1000.00, 2, 3),
             add(201, Side::SELL, 1000.25, 4, 4)});

    const auto position = queue_position(book(), 103);
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(position->orders_ahead, 2u);
    EXPECT_EQ(position->quantity_ahead, to_qty_t(8));
    EXPECT_EQ(position->quantity, to_qty_t(2));
    EXPECT_EQ(position->level_quantity, to_qty_t(10));
    EXPECT_EQ(position->level_orders, 3u);
    EXPECT_EQ(position->level_index, 0);
    EXPECT_EQ(position->price, to_price_t(1000.00));
    EXPECT_EQ(position->side, Side::BUY);

    const auto front = queue_position(book(), 101);
    ASSERT_TRUE(front.has_value());
    EXPECT_EQ(front->orders_ahead, 0u);
    EXPECT_EQ(front->quantity_ahead, 0);

    EXPECT_FALSE(queue_position(book(), 999).has_value()) << "not in the book";
}

TEST_F(MboBookTest, TheQueueMovesAsOrdersAheadTradeAndCancel) {
    publish({add(101, Side::BUY, 1000.00, 5, 1),
             add(102, Side::BUY, 1000.00, 3, 2),
             add(103, Side::BUY, 1000.00, 2, 3)});

    // A partial fill of the front order: it keeps its place, less is ahead.
    publish({modify(101, Side::BUY, 1000.00, 1, 1)});
    auto position = queue_position(book(), 103);
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(position->orders_ahead, 2u);
    EXPECT_EQ(position->quantity_ahead, to_qty_t(4));

    // The front order is gone.
    publish({removal(101)});
    position = queue_position(book(), 103);
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(position->orders_ahead, 1u);
    EXPECT_EQ(position->quantity_ahead, to_qty_t(3));

    // The order ahead is modified in a way that costs it its place: a new, later priority.
    publish({modify(102, Side::BUY, 1000.00, 6, 9)});
    position = queue_position(book(), 103);
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(position->orders_ahead, 0u) << "102 went to the back of the queue";
    EXPECT_EQ(position->level_quantity, to_qty_t(8));

    // A better bid arrives: same place in its queue, no longer the best price.
    publish({add(104, Side::BUY, 1000.25, 1, 10)});
    position = queue_position(book(), 103);
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(position->level_index, 1);
    EXPECT_EQ(position->orders_ahead, 0u);
}

TEST_F(MboBookTest, OrdersQueueByPriorityNotByArrival) {
    // A snapshot lists orders in no particular order; the exchange's priority places them.
    publish({add(103, Side::SELL, 1000.25, 2, 30),
             add(101, Side::SELL, 1000.25, 5, 10),
             add(102, Side::SELL, 1000.25, 3, 20)},
            /*is_snapshot=*/true);

    const auto position = queue_position(book(), 103);
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(position->orders_ahead, 2u);
    EXPECT_EQ(position->quantity_ahead, to_qty_t(8));
    EXPECT_EQ(position->side, Side::SELL);
}

TEST_F(MboBookTest, ASnapshotStartsTheBookOver) {
    publish({add(101, Side::BUY, 1000.00, 5, 1), add(102, Side::BUY, 1000.00, 3, 2)});
    publish({add(102, Side::BUY, 1000.00, 3, 2), add(105, Side::SELL, 1000.50, 1, 7)}, /*is_snapshot=*/true);

    EXPECT_EQ(book().orderCount(), 2u);
    EXPECT_FALSE(queue_position(book(), 101).has_value()) << "an order the snapshot does not list is gone";
    const auto position = queue_position(book(), 102);
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(position->orders_ahead, 0u);

    publish({}, /*is_snapshot=*/true);
    EXPECT_EQ(book().orderCount(), 0u) << "an empty snapshot is an empty book";
}

TEST_F(MboBookTest, StrategiesSeeTheBookOnlyWhenAnExchangeEventIsComplete) {
    publish({add(101, Side::BUY, 1000.00, 5, 1)}, /*is_snapshot=*/true, /*is_last=*/false);
    publish({add(102, Side::BUY, 1000.00, 3, 2)}, /*is_snapshot=*/false, /*is_last=*/false);
    EXPECT_EQ(strategy_->books, 0) << "the book is half applied";

    publish({add(201, Side::SELL, 1000.25, 4, 3)});
    EXPECT_EQ(strategy_->books, 1);
    EXPECT_EQ(strategy_->last_instrument, "ESZ6");
    EXPECT_EQ(strategy_->orders_in_last_book, 3u);
}

TEST_F(MboBookTest, EachInstrumentHasItsOwnBook) {
    publish({add(101, Side::BUY, 1000.00, 5, 1)});
    publish({add(101, Side::BUY, 18000.00, 2, 1)}, false, true, "NQZ6");

    EXPECT_EQ(strategy_->last_instrument, "NQZ6");
    EXPECT_EQ(queue_position(book("ESZ6"), 101)->price, to_price_t(1000.00));
    EXPECT_EQ(queue_position(book("NQZ6"), 101)->price, to_price_t(18000.00));
    EXPECT_EQ(manager_.get_order_book_l3("CLZ6"), nullptr);
}

TEST_F(MboBookTest, AStrategyFindsItsOwnOrderByClientOrderId) {
    publish({add(101, Side::BUY, 1000.00, 5, 1), add(102, Side::BUY, 1000.00, 3, 2)});

    const auto client_id = strategy_->submit_order("ESZ6", BrokerId::CME, Side::BUY, to_qty_t(1), OrderType::LIMIT,
                                                   to_price_t(1000.00), TimeInForce::GTC, false);
    ASSERT_FALSE(client_id.empty());
    EXPECT_FALSE(strategy_->get_queue_position(book(), client_id).has_value()) << "not acknowledged yet";

    // The exchange acknowledges it as order 103, and the feed shows 103 join the queue.
    OrderAckEvent ack;
    ack.broker_id = BrokerId::CME;
    ack.set_instrument("ESZ6");
    ack.set_client_order_id(client_id.c_str());
    ack.set_order_id("103");
    ack.side = Side::BUY;
    ack.price = to_price_t(1000.00);
    ack.quantity = to_qty_t(1);
    ack.status = OrderStatus::ACKNOWLEDGED;
    strategy_->on_order_ack(ack);
    EXPECT_FALSE(strategy_->get_queue_position(book(), client_id).has_value()) << "not on the feed yet";

    publish({add(103, Side::BUY, 1000.00, 1, 3)});
    const auto position = strategy_->get_queue_position(book(), client_id);
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(position->orders_ahead, 2u);
    EXPECT_EQ(position->quantity_ahead, to_qty_t(8));

    EXPECT_FALSE(strategy_->get_queue_position(book(), "Book_unknown").has_value());
}

TEST_F(MboBookTest, AnOrderIdThatIsNotANumberHasNoQueuePosition) {
    publish({add(101, Side::BUY, 1000.00, 5, 1)});

    EXPECT_TRUE(queue_position(book(), std::string_view("101")).has_value());
    EXPECT_FALSE(queue_position(book(), std::string_view("")).has_value());
    EXPECT_FALSE(queue_position(book(), std::string_view("101x")).has_value());
    EXPECT_FALSE(queue_position(book(), std::string_view("0b1f2c9e-uuid")).has_value());
}

// Regression test. A book reports a side with nothing on it as price 0, and since zero and
// negative prices became valid marks (futures spreads) that 0 was taken for a price: a bid
// alone marked the position at half the bid, an emptied book at zero.
TEST_F(MboBookTest, ASideWithNothingOnItHasNoPriceAndMarksNothing) {
    publish({add(101, Side::BUY, 1000.00, 5, 1)});
    ASSERT_EQ(strategy_->quotes, 1);
    EXPECT_EQ(strategy_->last_quote.bid_price, to_price_t(1000.00));
    EXPECT_EQ(strategy_->last_quote.ask_price, NULL_PRICE) << "no offers is not an offer at 0";
    EXPECT_FALSE(pnl_.has_mark_price("ESZ6")) << "half a market is not a mark";

    publish({add(201, Side::SELL, 1000.50, 4, 2)});
    ASSERT_TRUE(pnl_.has_mark_price("ESZ6"));
    EXPECT_EQ(pnl_.get_mark_price("ESZ6"), to_price_t(1000.25));

    // The book empties: the last two-sided market stays the mark.
    publish({removal(101), removal(201)});
    EXPECT_EQ(strategy_->last_quote.bid_price, NULL_PRICE);
    EXPECT_EQ(strategy_->last_quote.ask_price, NULL_PRICE);
    EXPECT_EQ(pnl_.get_mark_price("ESZ6"), to_price_t(1000.25));
}

// The same rule for a price-level book, which shares the top-of-book path.
TEST_F(MboBookTest, AOneSidedLevelBookMarksNothingEither) {
    std::vector<std::string> instruments{"BTC-USD"};
    manager_.subscribe_book_update(instruments);

    std::vector<uint8_t> record(sizeof(Level2UpdateEvent) + sizeof(PriceLevel));
    auto* event = new (record.data()) Level2UpdateEvent();
    event->set_instrument("BTC-USD");
    event->is_snapshot = true;
    event->num_levels = 1;
    event->levels[0] = PriceLevel(to_price_t(60000.0), to_qty_t(0.5), Side::BUY);
    manager_.process_event(event);

    ASSERT_EQ(strategy_->quotes, 1);
    EXPECT_EQ(strategy_->last_quote.bid_price, to_price_t(60000.0));
    EXPECT_EQ(strategy_->last_quote.ask_price, NULL_PRICE);
    EXPECT_FALSE(pnl_.has_mark_price("BTC-USD"));
}

// Regression test. The manager publishes each top of book on the event bus for the dashboard,
// and reads that bus itself. The record went out typed QUOTE, so it came back and every
// strategy was given every quote twice - while the dashboard, which waits for TOB, got none.
TEST_F(MboBookTest, ATopOfBookReachesAStrategyOnceAndTheBusAsTob) {
    uint64_t cursor = mux_.initial_reading_index();
    publish({add(101, Side::BUY, 1000.00, 5, 1), add(201, Side::SELL, 1000.25, 4, 2)});
    ASSERT_EQ(strategy_->quotes, 1);

    // What the worker thread reads next is the manager's own record.
    const auto record = mux_.read(cursor);
    ASSERT_TRUE(static_cast<bool>(record)) << "the top of book was not published";
    ASSERT_EQ(record.length, sizeof(QuoteEvent));
    const QuoteEvent published = *reinterpret_cast<const QuoteEvent*>(record.data);
    EXPECT_EQ(published.type, EventType::TOB) << "what the dashboard refreshes PnL on";
    EXPECT_STREQ(published.instrument, "ESZ6");
    EXPECT_EQ(published.bid_price, to_price_t(1000.00));
    EXPECT_EQ(published.ask_price, to_price_t(1000.25));

    manager_.process_event(reinterpret_cast<const Event*>(record.data));
    EXPECT_EQ(strategy_->quotes, 1) << "the manager's own record was handed to the strategies again";
    EXPECT_FALSE(static_cast<bool>(mux_.read(cursor))) << "and reading it back published nothing";
}

TEST_F(MboBookTest, AnInstrumentWithOnlyAnMboBookStillGetsQuotes) {
    // Nothing else publishes this instrument's top of book, so the order book does - once per
    // change of the top, not once per order.
    publish({add(101, Side::BUY, 1000.00, 5, 1), add(201, Side::SELL, 1000.25, 4, 2)});
    ASSERT_EQ(strategy_->quotes, 1);
    EXPECT_EQ(strategy_->last_quote.bid_price, to_price_t(1000.00));
    EXPECT_EQ(strategy_->last_quote.bid_quantity, to_qty_t(5));
    EXPECT_EQ(strategy_->last_quote.ask_price, to_price_t(1000.25));
    EXPECT_EQ(strategy_->last_quote.ask_quantity, to_qty_t(4));

    publish({add(102, Side::BUY, 999.75, 7, 3)});
    EXPECT_EQ(strategy_->quotes, 1) << "the top of the book did not change";

    publish({add(103, Side::BUY, 1000.00, 2, 4)});
    ASSERT_EQ(strategy_->quotes, 2);
    EXPECT_EQ(strategy_->last_quote.bid_quantity, to_qty_t(7));
}
