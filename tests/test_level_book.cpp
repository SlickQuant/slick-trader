// LevelBook: price levels from order-by-order updates, the changes since the last flush,
// and each side's best price. Exchange-neutral: prices are whatever wire units a broker
// keeps its book in.

#include <gtest/gtest.h>
#include <slick-trader/brokers/common/level_book.hpp>
#include <vector>

using namespace slick::trader;

namespace {

struct Order {
    Side side;
    int64_t price;
    int64_t qty;
};

void add(LevelBook& book, const Order& o) { book.apply(o.side, o.price, 0, o.side, o.price, o.qty); }
void change(LevelBook& book, const Order& from, const Order& to) {
    book.apply(from.side, from.price, from.qty, to.side, to.price, to.qty);
}
void remove(LevelBook& book, const Order& o) { book.apply(o.side, o.price, o.qty, o.side, o.price, 0); }

std::vector<LevelBook::LevelChange> flush(LevelBook& book) {
    std::vector<LevelBook::LevelChange> out;
    book.flush([&](const LevelBook::LevelChange& c) { out.push_back(c); });
    return out;
}

} // namespace

TEST(LevelBookTest, OrdersAggregateIntoLevels) {
    LevelBook book;
    add(book, {Side::BUY, 100, 5});
    add(book, {Side::BUY, 100, 3});
    add(book, {Side::SELL, 101, 2});

    auto changes = flush(book);
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[0].side, Side::BUY);
    EXPECT_EQ(changes[0].qty, 8);
    EXPECT_EQ(changes[0].orders, 2u);
    EXPECT_EQ(changes[1].side, Side::SELL);
    EXPECT_EQ(changes[1].qty, 2);
    EXPECT_FALSE(book.dirty());
}

TEST(LevelBookTest, AMoveReportsBothLevels) {
    LevelBook book;
    add(book, {Side::BUY, 100, 5});
    flush(book);

    change(book, {Side::BUY, 100, 5}, {Side::BUY, 99, 4});
    auto changes = flush(book);
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[0].price, 100);
    EXPECT_EQ(changes[0].qty, 0) << "the old level emptied";
    EXPECT_EQ(changes[1].price, 99);
    EXPECT_EQ(changes[1].qty, 4);
    EXPECT_EQ(book.level_count(), 1u) << "an emptied level is forgotten once reported";
}

TEST(LevelBookTest, RemovalEmptiesTheLevel) {
    LevelBook book;
    add(book, {Side::SELL, 101, 2});
    flush(book);
    remove(book, {Side::SELL, 101, 2});
    auto changes = flush(book);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].qty, 0);
    EXPECT_EQ(changes[0].orders, 0u);
    EXPECT_EQ(book.level_count(), 0u);
}

TEST(LevelBookTest, SnapshotListsOnlyRestingLevels) {
    LevelBook book;
    add(book, {Side::BUY, 100, 5});
    add(book, {Side::SELL, 101, 2});
    remove(book, {Side::SELL, 101, 2});

    std::vector<LevelBook::LevelChange> levels;
    book.for_each_level([&](const LevelBook::LevelChange& c) { levels.push_back(c); });
    ASSERT_EQ(levels.size(), 1u);
    EXPECT_EQ(levels[0].price, 100);
}

// The best price of each side follows the orders, including when the best level empties
// and the next one takes its place. Post-only orders are checked against it.
TEST(LevelBookTest, TheBestPriceOfEachSideFollowsTheBook) {
    LevelBook book;
    EXPECT_EQ(book.best(Side::BUY), LevelBook::NO_PRICE);
    EXPECT_EQ(book.best(Side::SELL), LevelBook::NO_PRICE);

    add(book, {Side::BUY, 100, 5});
    add(book, {Side::BUY, 98, 5});
    add(book, {Side::SELL, 103, 5});
    add(book, {Side::SELL, 105, 5});
    EXPECT_EQ(book.best(Side::BUY), 100);
    EXPECT_EQ(book.best(Side::SELL), 103);

    add(book, {Side::BUY, 101, 1});   // a better bid
    add(book, {Side::BUY, 99, 1});    // and one behind the best
    EXPECT_EQ(book.best(Side::BUY), 101);

    remove(book, {Side::BUY, 101, 1});   // the best level empties: the next takes over
    EXPECT_EQ(book.best(Side::BUY), 100);
    add(book, {Side::BUY, 100, 2});
    remove(book, {Side::BUY, 100, 5});   // one of two orders at the best leaves
    EXPECT_EQ(book.best(Side::BUY), 100);

    // An order that moves away from the best price, and one that moves to a new best.
    change(book, {Side::SELL, 103, 5}, {Side::SELL, 106, 5});
    EXPECT_EQ(book.best(Side::SELL), 105);
    change(book, {Side::SELL, 105, 5}, {Side::SELL, 102, 5});
    EXPECT_EQ(book.best(Side::SELL), 102);
    flush(book);   // flushing forgets the emptied levels, not the best prices
    EXPECT_EQ(book.best(Side::SELL), 102);
    EXPECT_EQ(book.best(Side::BUY), 100);

    // Negative prices are prices: a calendar spread's book.
    LevelBook spread;
    add(spread, {Side::BUY, -7, 1});
    add(spread, {Side::BUY, -5, 1});
    add(spread, {Side::SELL, -3, 1});
    EXPECT_EQ(spread.best(Side::BUY), -5);
    EXPECT_EQ(spread.best(Side::SELL), -3);
    remove(spread, {Side::BUY, -5, 1});
    remove(spread, {Side::BUY, -7, 1});
    EXPECT_EQ(spread.best(Side::BUY), LevelBook::NO_PRICE);
}

TEST(LevelBookTest, AnOrderThatChangesSideLeavesOneSideForTheOther) {
    LevelBook book;
    add(book, {Side::BUY, 100, 5});
    flush(book);
    change(book, {Side::BUY, 100, 5}, {Side::SELL, 100, 5});
    auto changes = flush(book);
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[0].side, Side::BUY);
    EXPECT_EQ(changes[0].qty, 0);
    EXPECT_EQ(changes[1].side, Side::SELL);
    EXPECT_EQ(changes[1].qty, 5);
}
