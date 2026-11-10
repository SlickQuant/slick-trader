// Regression tests: what a browser subscribing to an instrument's level 2 starts from.
//
// The WebServer only forwarded LEVEL2_UPDATE events, so a browser that subscribed after the
// broker's snapshot went by never got it: a ladder added when MANUAL adopted an instrument
// stayed empty. The WebServer now keeps every streamed book and sends it on subscribe.
// Level2Streams holds those books and who watches each, and decides what a subscribe is sent.

#include <gtest/gtest.h>

#include <slick-trader/web/level2_book.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <new>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace slick::trader;

namespace {

// A LEVEL2_UPDATE as a broker publishes it, levels trailing the header.
std::vector<uint8_t> update(bool is_snapshot, uint64_t sequence, const std::vector<PriceLevel>& levels) {
    std::vector<uint8_t> buffer(sizeof(Level2UpdateEvent) + levels.size() * sizeof(PriceLevel));
    auto* event = new (buffer.data()) Level2UpdateEvent();
    event->is_snapshot = is_snapshot;
    event->sequence_num = sequence;
    event->event_time_ns = sequence * 1000;
    event->num_levels = static_cast<uint16_t>(levels.size());
    // An empty vector's data() may be null, and memcpy from null is UB even for 0 bytes.
    if (!levels.empty()) {
        std::memcpy(event->levels, levels.data(), levels.size() * sizeof(PriceLevel));
    }
    return buffer;
}

std::vector<uint8_t> update(bool is_snapshot, uint64_t sequence, std::initializer_list<PriceLevel> levels) {
    return update(is_snapshot, sequence, std::vector<PriceLevel>(levels));
}

void feed(Level2Book& book, const std::vector<uint8_t>& buffer) {
    book.apply(*reinterpret_cast<const Level2UpdateEvent*>(buffer.data()));
}

using Level = std::tuple<Side, price_t, qty_t>;

std::set<Level> levels_of(const Level2Book& book) {
    std::set<Level> levels;
    book.for_each([&](price_t price, qty_t qty, Side side) {
        EXPECT_TRUE(levels.emplace(side, price, qty).second) << "a level was reported twice";
    });
    return levels;
}

// In the order for_each() hands them over.
std::vector<Level> levels_in_order(const Level2Book& book) {
    std::vector<Level> levels;
    book.for_each([&](price_t price, qty_t qty, Side side) { levels.emplace_back(side, price, qty); });
    return levels;
}

} // namespace

// The book kept every level a broker sent in a hash map, so a snapshot went out in no order. It
// keeps each side in price order, and hands it over best first.
TEST(Level2BookTest, ItKeepsItsLevelsBestFirst) {
    Level2Book book;
    feed(book, update(true, 1, {PriceLevel(99, 1, Side::BUY), PriceLevel(103, 4, Side::SELL),
                                 PriceLevel(101, 2, Side::BUY), PriceLevel(102, 5, Side::SELL),
                                 PriceLevel(100, 3, Side::BUY)}));
    EXPECT_EQ(levels_in_order(book), (std::vector<Level>{{Side::BUY, 101, 2}, {Side::BUY, 100, 3},
                                                         {Side::BUY, 99, 1}, {Side::SELL, 102, 5},
                                                         {Side::SELL, 103, 4}}));
}

// The book kept the best 256 levels a side, and then handed over only those: a broker sends a
// snapshot only as it subscribes, so once the price moved past them, a later subscriber's ladder
// thinned out where the rest were. It keeps and hands over every level.
TEST(Level2BookTest, ItKeepsAndHandsOverEveryLevel) {
    constexpr price_t N = 256;
    std::vector<PriceLevel> snapshot;
    for (price_t i = 0; i < 2 * N; ++i) {
        snapshot.emplace_back(1000 - i, 1, Side::BUY);    // 1000 down
        snapshot.emplace_back(1001 + i, 1, Side::SELL);   // 1001 up
    }
    Level2Book book;
    feed(book, update(true, 1, snapshot));
    auto sent = levels_in_order(book);
    ASSERT_EQ(sent.size(), static_cast<size_t>(4 * N));
    EXPECT_EQ(sent.front(), Level(Side::BUY, 1000, 1));
    EXPECT_EQ(sent[2 * N - 1], Level(Side::BUY, 1000 - 2 * N + 1, 1));
    EXPECT_EQ(sent[2 * N], Level(Side::SELL, 1001, 1));
    EXPECT_EQ(sent.back(), Level(Side::SELL, 1000 + 2 * N, 1));

    // The price falls N levels: the bids handed over are those past the first N, and one more.
    std::vector<PriceLevel> taken;
    for (price_t i = 0; i < N; ++i) {
        taken.emplace_back(1000 - i, 0, Side::BUY);
    }
    taken.emplace_back(1000 - 2 * N, 3, Side::BUY);
    feed(book, update(false, 2, taken));
    sent = levels_in_order(book);
    ASSERT_EQ(sent.size(), static_cast<size_t>(3 * N + 1));
    EXPECT_EQ(sent.front(), Level(Side::BUY, 1000 - N, 1));
    EXPECT_EQ(sent[N - 1], Level(Side::BUY, 1000 - 2 * N + 1, 1)) << "a level past the first snapshot's 256";
    EXPECT_EQ(sent[N], Level(Side::BUY, 1000 - 2 * N, 3));

    // A level into a gap goes in its place, one past all the others at the end, and the order holds.
    feed(book, update(false, 3, {PriceLevel(1000 - N - 1, 0, Side::BUY)}));
    feed(book, update(false, 4, {PriceLevel(1000 - N - 1, 9, Side::BUY), PriceLevel(2000, 9, Side::SELL)}));
    const auto ordered = levels_in_order(book);
    ASSERT_EQ(ordered.size(), static_cast<size_t>(3 * N + 2));   // N + 1 bids, 2N + 1 asks
    EXPECT_EQ(ordered[1], Level(Side::BUY, 1000 - N - 1, 9));
    EXPECT_EQ(ordered.back(), Level(Side::SELL, 2000, 9));
    EXPECT_TRUE(std::is_sorted(ordered.begin(), ordered.begin() + N + 1,
                               [](const Level& a, const Level& b) { return std::get<1>(a) > std::get<1>(b); }));
    EXPECT_TRUE(std::is_sorted(ordered.begin() + N + 1, ordered.end(),
                               [](const Level& a, const Level& b) { return std::get<1>(a) < std::get<1>(b); }));
}

// A level2 message was a json tree with a node per level. It is written into one buffer now,
// and reads the same: names escaped, prices and quantities as numbers, side as 0 or 1.
TEST(Level2MessageTest, ItReadsAsTheDashboardExpects) {
    std::string out = "what the last message left";
    write_level2_message(out, "ZN \"Z6\"\\\x01", 2'500'000'000, 7, true, [](auto&& emit) {
        emit(to_price_t(110.5), to_qty_t(3.0), Side::BUY);
        emit(NULL_PRICE, to_qty_t(0.25), Side::SELL);
    });
    const auto message = nlohmann::json::parse(out);
    EXPECT_EQ(message["type"], "level2");
    EXPECT_EQ(message["instrument"], "ZN \"Z6\"\\\x01");
    EXPECT_EQ(message["timestamp"], 2500);
    EXPECT_EQ(message["seq_num"], 7);
    EXPECT_EQ(message["is_snapshot"], true);
    EXPECT_EQ(message["levels"], nlohmann::json::parse("[[110.5, 3, 0], [null, 0.25, 1]]"));

    write_level2_message(out, "ZNZ6", 0, 8, false, [](auto&&) {});
    EXPECT_EQ(out, R"({"type":"level2","instrument":"ZNZ6","timestamp":0,"seq_num":8,"is_snapshot":false,"levels":[]})");
}

TEST(Level2BookTest, ALateSubscriberGetsTheBookAsItStands) {
    Level2Book book;
    feed(book, update(true, 1, {PriceLevel(100, 5, Side::BUY), PriceLevel(99, 3, Side::BUY),
                                 PriceLevel(101, 4, Side::SELL)}));
    feed(book, update(false, 2, {PriceLevel(99, 0, Side::BUY), PriceLevel(102, 1, Side::SELL),
                                  PriceLevel(100, 7, Side::BUY)}));

    EXPECT_EQ(levels_of(book), (std::set<Level>{{Side::BUY, 100, 7}, {Side::SELL, 101, 4}, {Side::SELL, 102, 1}}))
        << "updates after the snapshot change, add and remove levels";
    EXPECT_EQ(book.sequence_num(), 2u);
    EXPECT_EQ(book.event_time_ns(), 2000u);
}

TEST(Level2BookTest, ASnapshotReplacesTheBookAndItsLaterPartsAddToIt) {
    Level2Book book;
    feed(book, update(true, 1, {PriceLevel(100, 5, Side::BUY), PriceLevel(101, 4, Side::SELL)}));

    // A recovered book, published in two parts: only the first is marked a snapshot.
    feed(book, update(true, 2, {PriceLevel(98, 2, Side::BUY)}));
    feed(book, update(false, 2, {PriceLevel(103, 6, Side::SELL)}));

    EXPECT_EQ(levels_of(book), (std::set<Level>{{Side::BUY, 98, 2}, {Side::SELL, 103, 6}}));
}

TEST(Level2BookTest, AnEmptySnapshotEmptiesTheBook) {
    Level2Book book;
    feed(book, update(true, 1, {PriceLevel(100, 5, Side::BUY)}));
    feed(book, update(true, 2, {}));
    EXPECT_TRUE(levels_of(book).empty());
}

// The WebServer's streams, from the browsers' side: what each is sent when it subscribes, and
// that one closing is left in no stream. The WebServer sends what these return.

namespace {

struct Tab {};
using Streams = Level2Streams<Tab>;

const Level2UpdateEvent& for_instrument(std::vector<uint8_t>& buffer, const char* instrument) {
    auto& event = *reinterpret_cast<Level2UpdateEvent*>(buffer.data());
    event.set_instrument(instrument);
    return event;
}

const std::set<Level> ZNZ6_BOOK{{Side::BUY, to_price_t(110.5), to_qty_t(3.0)}};

void stream_znz6(Streams& streams) {
    auto snapshot = update(true, 1, {PriceLevel(to_price_t(110.5), to_qty_t(3.0), Side::BUY)});
    streams.apply(for_instrument(snapshot, "ZNZ6"));
}

// Who watches `instrument`, as an update with no level that the WebServer forwards finds them.
Streams::Clients watchers(Streams& streams, const char* instrument) {
    auto nothing = update(false, 0, {});
    return streams.apply(for_instrument(nothing, instrument));
}

} // namespace

// A repeated subscribe was sent no snapshot, but the dashboard asks again for a ladder it built
// anew on the same socket: that ladder got only the updates after. Every subscribe gets the book.
TEST(Level2StreamsTest, EverySubscribeGetsTheBook) {
    Streams streams;
    stream_znz6(streams);

    Tab tab;
    const Level2Book* book = streams.subscribe(&tab, "ZNZ6");
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(levels_of(*book), ZNZ6_BOOK);
    book = streams.subscribe(&tab, "ZNZ6");
    ASSERT_NE(book, nullptr) << "a repeated subscribe was sent no snapshot";
    EXPECT_EQ(levels_of(*book), ZNZ6_BOOK);
    EXPECT_EQ(watchers(streams, "ZNZ6"), Streams::Clients{&tab}) << "and watches it once";
}

TEST(Level2StreamsTest, ASubscribeBeforeAnyEventGetsAnEmptyBook) {
    Streams streams;
    Tab tab;
    const Level2Book* book = streams.subscribe(&tab, "ZNZ6");
    ASSERT_NE(book, nullptr) << "a browser subscribing before the first event was sent nothing to start from";
    EXPECT_TRUE(levels_of(*book).empty());

    auto later = update(false, 2, {PriceLevel(to_price_t(110.5), to_qty_t(3.0), Side::BUY)});
    EXPECT_TRUE(streams.apply(for_instrument(later, "ZNZ6")).contains(&tab)) << "and it watches what follows";
}

TEST(Level2StreamsTest, EachBrowserGetsASnapshotOfItsOwn) {
    Streams streams;
    stream_znz6(streams);

    Tab first;
    Tab second;
    ASSERT_NE(streams.subscribe(&first, "ZNZ6"), nullptr);
    const Level2Book* book = streams.subscribe(&second, "ZNZ6");
    ASSERT_NE(book, nullptr) << "the second browser was taken for one already watching";
    EXPECT_EQ(levels_of(*book), ZNZ6_BOOK);
    EXPECT_EQ(watchers(streams, "ZNZ6").size(), 2u);
}

TEST(Level2StreamsTest, AClosedBrowserIsLeftInNoStream) {
    Streams streams;
    Tab tab;
    Tab other;
    streams.subscribe(&tab, "ZNZ6");
    streams.subscribe(&tab, "ZBZ6");
    streams.subscribe(&other, "ZBZ6");

    streams.remove(&tab);
    EXPECT_TRUE(watchers(streams, "ZNZ6").empty());
    EXPECT_EQ(watchers(streams, "ZBZ6"), Streams::Clients{&other});
    EXPECT_FALSE(streams.unsubscribe(&tab, "ZNZ6"));
    EXPECT_NE(streams.subscribe(&tab, "ZNZ6"), nullptr) << "a browser coming back starts from the book again";
}

// Any name a client sent became a stream, kept for good: a page looping over random names grew
// the server without bound. A name too long for an instrument is refused, a client watches a
// bounded number, and a stream only asked for goes with its last watcher.
TEST(Level2StreamsTest, WhatClientsAskForStaysBounded) {
    Streams streams;
    Tab tab;
    EXPECT_EQ(streams.subscribe(&tab, ""), nullptr);
    EXPECT_EQ(streams.subscribe(&tab, std::string(MAX_INSTRUMENT_LENGTH, 'Z')), nullptr) << "longer than any instrument";

    for (size_t i = 0; i < Streams::MAX_STREAMS_PER_CLIENT; ++i) {
        ASSERT_NE(streams.subscribe(&tab, "I" + std::to_string(i)), nullptr);
    }
    EXPECT_EQ(streams.subscribe(&tab, "OVER"), nullptr);
    EXPECT_NE(streams.subscribe(&tab, "I0"), nullptr) << "one it watches still gets its book";

    stream_znz6(streams);
    EXPECT_TRUE(streams.unsubscribe(&tab, "I0"));
    EXPECT_EQ(streams.size(), Streams::MAX_STREAMS_PER_CLIENT) << "I0 had never streamed";
    streams.remove(&tab);
    EXPECT_EQ(streams.size(), 1u) << "only the book that streamed is kept";
}

// MANUAL subscribes an instrument while a ladder shows it, so the WebServer is told of a stream's
// first watcher and of its last one going: a client closing reports each it was the last on.
TEST(Level2StreamsTest, TheLastWatcherOfEachStreamIsReported) {
    Streams streams;
    Tab tab;
    Tab other;
    streams.subscribe(&tab, "ZNZ6");
    EXPECT_EQ(streams.watchers("ZNZ6"), 1u);
    streams.subscribe(&other, "ZNZ6");
    streams.subscribe(&tab, "ZBZ6");
    EXPECT_EQ(streams.watchers("ZNZ6"), 2u);

    std::vector<std::string> unwatched;
    streams.remove(&tab, [&](std::string_view instrument) { unwatched.emplace_back(instrument); });
    EXPECT_EQ(unwatched, std::vector<std::string>{"ZBZ6"}) << "ZNZ6 has a watcher left";
    EXPECT_EQ(streams.watchers("ZNZ6"), 1u);
    EXPECT_EQ(streams.watchers("ZBZ6"), 0u);
    EXPECT_TRUE(streams.unsubscribe(&other, "ZNZ6"));
    EXPECT_EQ(streams.watchers("ZNZ6"), 0u);
}

// A ladder held again after its instrument was let go at the broker started from the book the
// broker last left, levels long gone, until the snapshot came. It starts empty now.
TEST(Level2StreamsTest, ABookThatStoppedSinceItWasLetGoIsEmptiedWhenHeldAgain) {
    Streams streams;
    Tab tab;
    stream_znz6(streams);
    streams.release("ZNZ6");
    const Level2Book* book = streams.subscribe(&tab, "ZNZ6");
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(levels_of(*book), ZNZ6_BOOK) << "emptied before the broker was asked for it again";
    streams.renew("ZNZ6");
    EXPECT_TRUE(levels_of(*book).empty());

    stream_znz6(streams);
    streams.renew("ZNZ6");
    EXPECT_EQ(levels_of(*book), ZNZ6_BOOK) << "a book not let go was emptied";
}

// A connector that streams on after it is told to stop keeps the book current: emptied as it is
// held again, it would be built from deltas alone, as no new snapshot comes.
TEST(Level2StreamsTest, ABookStillStreamingAfterItWasLetGoIsKept) {
    Streams streams;
    stream_znz6(streams);
    streams.release("ZNZ6");
    auto delta = update(false, 2, {PriceLevel(to_price_t(110.25), to_qty_t(1.0), Side::BUY)});
    streams.apply(for_instrument(delta, "ZNZ6"));

    Tab tab;
    const Level2Book* book = streams.subscribe(&tab, "ZNZ6");
    streams.renew("ZNZ6");
    EXPECT_EQ(levels_of(*book).size(), 2u);
}

// A snapshot went in a level at a time, each kept worst first. A broker lists one best first, so
// every level went in at the front of its side and moved all the others: a full-depth book took
// the server's event loop seconds. It is taken in whole now, and sorted once.
TEST(Level2BookTest, ADeepSnapshotListedBestFirstIsTakenInAtOnce) {
    constexpr price_t PER_SIDE = 30000;
    std::vector<PriceLevel> snapshot;
    for (price_t i = 0; i < PER_SIDE; ++i) {
        snapshot.emplace_back(100000 - i, 1, Side::BUY);
        snapshot.emplace_back(100001 + i, 1, Side::SELL);
    }
    const auto event = update(true, 1, snapshot);
    Level2Book book;
    const auto start = std::chrono::steady_clock::now();
    feed(book, event);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));

    const auto sent = levels_in_order(book);
    ASSERT_EQ(sent.size(), static_cast<size_t>(2 * PER_SIDE));
    EXPECT_EQ(sent.front(), Level(Side::BUY, 100000, 1));
    EXPECT_EQ(sent[PER_SIDE - 1], Level(Side::BUY, 100000 - PER_SIDE + 1, 1));
    EXPECT_EQ(sent[PER_SIDE], Level(Side::SELL, 100001, 1));
    EXPECT_EQ(sent.back(), Level(Side::SELL, 100000 + PER_SIDE, 1));
}

// Taken in whole, a snapshot reads as it did a level at a time: a price listed twice keeps its
// last quantity, and an empty level is no level.
TEST(Level2BookTest, ASnapshotListingAPriceTwiceKeepsItsLast) {
    Level2Book book;
    feed(book, update(true, 1, {PriceLevel(100, 1, Side::BUY), PriceLevel(99, 2, Side::BUY),
                                 PriceLevel(100, 3, Side::BUY), PriceLevel(98, 0, Side::BUY),
                                 PriceLevel(101, 4, Side::SELL), PriceLevel(101, 0, Side::SELL),
                                 PriceLevel(102, 5, Side::SELL)}));
    EXPECT_EQ(levels_in_order(book),
              (std::vector<Level>{{Side::BUY, 100, 3}, {Side::BUY, 99, 2}, {Side::SELL, 102, 5}}));
}

// A snapshot over 1,024 levels comes in parts, as md_event_writer splits it, and only the first
// is a snapshot. The rest went in a level at a time, each at the front of its side.
TEST(Level2BookTest, ASnapshotInPartsIsTakenInAtOnce) {
    constexpr price_t PER_SIDE = 60000;   // deep enough that a level at a time takes seconds
    std::vector<PriceLevel> snapshot;
    for (price_t i = 0; i < PER_SIDE; ++i) {
        snapshot.emplace_back(100000 - i, 1, Side::BUY);
        snapshot.emplace_back(100001 + i, 1, Side::SELL);
    }
    std::vector<std::vector<uint8_t>> parts;
    for (size_t offset = 0; offset < snapshot.size(); offset += 1024) {
        const auto first = snapshot.begin() + static_cast<std::ptrdiff_t>(offset);
        const auto last = snapshot.begin() + static_cast<std::ptrdiff_t>(std::min(snapshot.size(), offset + 1024));
        parts.push_back(update(offset == 0, 1, std::vector<PriceLevel>(first, last)));
    }
    Level2Book book;
    const auto start = std::chrono::steady_clock::now();
    for (const auto& part : parts) {
        feed(book, part);
    }
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));

    const auto sent = levels_in_order(book);
    ASSERT_EQ(sent.size(), static_cast<size_t>(2 * PER_SIDE));
    EXPECT_EQ(sent.front(), Level(Side::BUY, 100000, 1));
    EXPECT_EQ(sent[PER_SIDE - 1], Level(Side::BUY, 100000 - PER_SIDE + 1, 1));
    EXPECT_EQ(sent[PER_SIDE], Level(Side::SELL, 100001, 1));
    EXPECT_EQ(sent.back(), Level(Side::SELL, 100000 + PER_SIDE, 1));
}

// An event of many levels is merged in one pass, and leaves the book as its levels set one by one
// would: changed, removed, new at either end and in between, and a price listed twice.
TEST(Level2BookTest, ManyLevelsAtOnceReadAsOneByOne) {
    std::vector<PriceLevel> start;
    for (price_t i = 0; i < 200; i += 2) {
        start.emplace_back(1000 - i, 1, Side::BUY);
        start.emplace_back(1001 + i, 1, Side::SELL);
    }
    std::vector<PriceLevel> changes;
    for (price_t i = 0; i < 300; i += 3) {
        changes.emplace_back(1000 - i, i % 2 ? 0 : 7, Side::BUY);    // on a kept level or a gap
        changes.emplace_back(1001 + i, i % 2 ? 9 : 0, Side::SELL);
    }
    changes.emplace_back(1000, 4, Side::BUY);   // again, later
    changes.emplace_back(2000, 2, Side::SELL);
    ASSERT_GT(changes.size(), Level2Book::MERGE_FROM);

    Level2Book merged;
    Level2Book one_by_one;
    feed(merged, update(true, 1, start));
    feed(one_by_one, update(true, 1, start));
    feed(merged, update(false, 2, changes));
    for (const PriceLevel& change : changes) {
        feed(one_by_one, update(false, 2, {change}));
    }
    EXPECT_EQ(levels_in_order(merged), levels_in_order(one_by_one));
}
