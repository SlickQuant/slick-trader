// Positions kept across a restart: the memory-mapped file, the store built on it, and the
// tracker that takes its positions up again.

#include <gtest/gtest.h>
#include <slick-trader/core/position_store.hpp>
#include <slick-trader/core/position_tracker.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick-trader/utils/mapped_file.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace slick::trader;

namespace {

constexpr const char* STRATEGY = "StoreTest";

OrderFillEvent make_fill(const char* instrument, Side side, double price, double qty, double fee = 0.0) {
    OrderFillEvent ev;
    ev.set_instrument(instrument);
    ev.side = side;
    ev.fill_price = to_price_t(price);
    ev.fill_quantity = to_qty_t(qty);
    ev.fee_amount = to_price_t(fee);
    return ev;
}

// Each test gets its own file, removed before and after.
class PositionStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = std::filesystem::temp_directory_path() / "slick_trader_position_store_test" / info->name();
        std::filesystem::remove_all(dir_);
        path_ = (dir_ / "positions.dat").string();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    struct Kept {
        Position position;
        std::vector<PositionLot> lots;
    };

    // Everything `store` holds, by "strategy/instrument".
    static std::map<std::string, Kept> contents(const PositionStore& store) {
        std::map<std::string, Kept> out;
        store.for_each([&](uint32_t, std::string_view strategy_id, const Position& position,
                           std::span<const PositionLot> lots) {
            out[std::string(strategy_id) + "/" + position.instrument] = {position, {lots.begin(), lots.end()}};
        });
        return out;
    }

    std::filesystem::path dir_;
    std::string path_;
};

} // namespace

// ── MappedFile ───────────────────────────────────────────────────────────────

TEST_F(PositionStoreTest, MappedFileKeepsWhatWasWritten) {
    {
        MappedFile file(path_, 4096);   // creates the missing directory too
        ASSERT_TRUE(file.is_open());
        EXPECT_TRUE(file.created());
        EXPECT_EQ(file.size(), 4096u);
        EXPECT_EQ(file.data()[0], 0);      // a new file is zeros
        EXPECT_EQ(file.data()[4095], 0);
        std::memcpy(file.data() + 100, "kept", 4);
    }
    MappedFile again(path_, 4096);
    EXPECT_FALSE(again.created());
    EXPECT_EQ(std::memcmp(again.data() + 100, "kept", 4), 0);
}

TEST_F(PositionStoreTest, MappedFileGrowsWithZerosAndNeverShrinks) {
    {
        MappedFile file(path_, 1024);
        std::memcpy(file.data(), "head", 4);
    }
    {
        MappedFile bigger(path_, 8192);
        EXPECT_EQ(bigger.size(), 8192u);
        EXPECT_EQ(std::memcmp(bigger.data(), "head", 4), 0);
        EXPECT_EQ(bigger.data()[8191], 0);
        bigger.data()[8191] = 7;
    }
    MappedFile smaller(path_, 16);   // asking for less maps what is there
    EXPECT_EQ(smaller.size(), 8192u);
    EXPECT_EQ(smaller.data()[8191], 7);
}

TEST_F(PositionStoreTest, MappedFileMovesItsMapping) {
    MappedFile first(path_, 512);
    first.data()[1] = 9;
    MappedFile second(std::move(first));
    EXPECT_FALSE(first.is_open());
    ASSERT_TRUE(second.is_open());
    EXPECT_EQ(second.data()[1], 9);
    second.flush();
    second.close();
    EXPECT_FALSE(second.is_open());
}

TEST_F(PositionStoreTest, MappedFileThatCannotBeOpenedThrows) {
    std::filesystem::create_directories(dir_ / "positions.dat");   // a directory where the file should be
    EXPECT_THROW(MappedFile(path_, 4096), std::runtime_error);
    EXPECT_THROW(MappedFile(path_ + ".zero", 0), std::runtime_error);
}

// ── PositionStore ────────────────────────────────────────────────────────────

TEST_F(PositionStoreTest, SavedPositionComesBackAfterReopening) {
    Position position("ESZ6");
    position.quantity = to_qty_t(3);
    position.avg_price = to_price_t(5000.25);
    position.realized_pnl = to_price_t(-125.5);
    position.realized_fees = to_price_t(4.2);
    position.open_fees = to_price_t(2.1);
    position.multiplier = 50.0;
    position.last_update_time = 77;
    const std::vector<PositionLot> lots = {{to_qty_t(1), to_price_t(5000.0), 11}, {to_qty_t(2), to_price_t(5000.375), 12}};
    {
        PositionStore store(path_, 8);
        EXPECT_EQ(store.size(), 0u);
        EXPECT_EQ(store.capacity(), 8u);
        EXPECT_NE(store.save(PositionStore::NO_SLOT, STRATEGY, position, lots), PositionStore::NO_SLOT);
        EXPECT_EQ(store.size(), 1u);
    }
    PositionStore store(path_, 8);
    ASSERT_EQ(store.size(), 1u);
    const auto kept = contents(store);
    ASSERT_EQ(kept.count("StoreTest/ESZ6"), 1u);
    const auto& [restored, restored_lots] = kept.at("StoreTest/ESZ6");
    EXPECT_EQ(restored.quantity, position.quantity);
    EXPECT_EQ(restored.avg_price, position.avg_price);
    EXPECT_EQ(restored.realized_pnl, position.realized_pnl);
    EXPECT_EQ(restored.realized_fees, position.realized_fees);
    EXPECT_EQ(restored.open_fees, position.open_fees);
    EXPECT_DOUBLE_EQ(restored.multiplier, 50.0);
    EXPECT_EQ(restored.last_update_time, 77u);
    ASSERT_EQ(restored_lots.size(), 2u);
    EXPECT_EQ(restored_lots[1].quantity, to_qty_t(2));
    EXPECT_EQ(restored_lots[1].price, to_price_t(5000.375));
    EXPECT_EQ(restored_lots[1].timestamp, 12u);
}

TEST_F(PositionStoreTest, UpdatingAPositionKeepsItsSlotAndReplacesItWhole) {
    PositionStore store(path_, 4);
    Position position("ESZ6");
    position.quantity = to_qty_t(1);
    const std::vector<PositionLot> one = {{to_qty_t(1), to_price_t(10.0), 1}};
    const uint32_t slot = store.save(PositionStore::NO_SLOT, STRATEGY, position, one);

    // Three more updates: the slot's two copies are each overwritten at least once.
    for (int qty = 2; qty <= 4; ++qty) {
        position.quantity = to_qty_t(qty);
        const std::vector<PositionLot> lots(static_cast<size_t>(qty), PositionLot{to_qty_t(1), to_price_t(10.0), 1});
        EXPECT_EQ(store.save(slot, STRATEGY, position, lots), slot);
    }
    EXPECT_EQ(store.size(), 1u);
    const auto kept = contents(store);
    EXPECT_EQ(kept.at("StoreTest/ESZ6").position.quantity, to_qty_t(4));
    EXPECT_EQ(kept.at("StoreTest/ESZ6").lots.size(), 4u);

    // Flat again: the lots go, the slot and its realized PnL stay.
    position.quantity = 0;
    position.realized_pnl = to_price_t(12.0);
    EXPECT_EQ(store.save(slot, STRATEGY, position, {}), slot);
    const auto flat = contents(store);
    EXPECT_TRUE(flat.at("StoreTest/ESZ6").position.is_flat());
    EXPECT_TRUE(flat.at("StoreTest/ESZ6").lots.empty());
    EXPECT_EQ(flat.at("StoreTest/ESZ6").position.realized_pnl, to_price_t(12.0));
}

TEST_F(PositionStoreTest, AFullStoreRefusesNewPositionsButStillUpdatesTheOnesItHas) {
    Position a("A"), b("B"), c("C");
    {
        PositionStore store(path_, 2);
        const uint32_t slot_a = store.save(PositionStore::NO_SLOT, STRATEGY, a, {});
        ASSERT_NE(store.save(PositionStore::NO_SLOT, STRATEGY, b, {}), PositionStore::NO_SLOT);
        EXPECT_EQ(store.save(PositionStore::NO_SLOT, STRATEGY, c, {}), PositionStore::NO_SLOT);
        EXPECT_EQ(store.size(), 2u);

        a.quantity = to_qty_t(5);
        EXPECT_EQ(store.save(slot_a, STRATEGY, a, {}), slot_a);
        EXPECT_EQ(contents(store).at("StoreTest/A").position.quantity, to_qty_t(5));
    }

    // Reopened with more room, the same file takes the third.
    PositionStore bigger(path_, 4);
    EXPECT_EQ(bigger.capacity(), 4u);
    EXPECT_EQ(bigger.size(), 2u);
    EXPECT_NE(bigger.save(PositionStore::NO_SLOT, STRATEGY, c, {}), PositionStore::NO_SLOT);
    EXPECT_EQ(contents(bigger).size(), 3u);
}

TEST_F(PositionStoreTest, ErasingAStrategyFreesItsSlotsAndLeavesTheOthers) {
    PositionStore store(path_, 4);
    Position a("A"), b("B");
    store.save(PositionStore::NO_SLOT, "Mine", a, {});
    store.save(PositionStore::NO_SLOT, "Mine", b, {});
    store.save(PositionStore::NO_SLOT, "Other", a, {});
    store.erase_strategy("Mine");
    EXPECT_EQ(store.size(), 1u);
    const auto kept = contents(store);
    EXPECT_EQ(kept.count("Other/A"), 1u);
    EXPECT_EQ(kept.count("Mine/A"), 0u);

    // The freed slots are used again.
    EXPECT_NE(store.save(PositionStore::NO_SLOT, "New", a, {}), PositionStore::NO_SLOT);
    EXPECT_NE(store.save(PositionStore::NO_SLOT, "New", b, {}), PositionStore::NO_SLOT);
    EXPECT_NE(store.save(PositionStore::NO_SLOT, "Newer", a, {}), PositionStore::NO_SLOT);
    EXPECT_EQ(store.save(PositionStore::NO_SLOT, "Newest", a, {}), PositionStore::NO_SLOT);

    store.clear();
    EXPECT_EQ(store.size(), 0u);
    EXPECT_TRUE(contents(store).empty());
}

TEST_F(PositionStoreTest, MoreLotsThanASlotHoldsKeepTheOldestAndAverageTheRest) {
    PositionStore store(path_, 2);
    Position position("ESZ6");
    std::vector<PositionLot> lots;
    for (size_t i = 0; i < PositionStore::MAX_LOTS + 2; ++i) {
        lots.push_back({to_qty_t(1), to_price_t(100.0 + static_cast<double>(i)), i});
    }
    position.quantity = to_qty_t(static_cast<double>(lots.size()));
    store.save(PositionStore::NO_SLOT, STRATEGY, position, lots);

    const auto kept = contents(store).at("StoreTest/ESZ6").lots;
    ASSERT_EQ(kept.size(), PositionStore::MAX_LOTS);
    EXPECT_EQ(kept.front().price, to_price_t(100.0));                              // the oldest, untouched
    EXPECT_EQ(kept[PositionStore::MAX_LOTS - 2].price, to_price_t(130.0));
    // The last three (131, 132, 133) as one lot of 3 at their average.
    EXPECT_EQ(kept.back().quantity, to_qty_t(3));
    EXPECT_EQ(kept.back().price, to_price_t(132.0));
    qty_t total = 0;
    for (const auto& lot : kept) {
        total += lot.quantity;
    }
    EXPECT_EQ(total, position.quantity);   // nothing of the position is lost
}

TEST_F(PositionStoreTest, AFileThatIsNotAPositionStoreIsRefusedNotOverwritten) {
    std::filesystem::create_directories(dir_);
    {
        std::ofstream other(path_, std::ios::binary);
        other << "this is somebody else's file, and it must stay that way";
    }
    EXPECT_THROW(PositionStore(path_, 4), std::runtime_error);
    std::ifstream check(path_, std::ios::binary);
    std::string first(4, '\0');
    check.read(first.data(), 4);
    EXPECT_EQ(first, "this");
}

TEST_F(PositionStoreTest, ANameTooLongForASlotIsNotKept) {
    PositionStore store(path_, 2);
    Position position("ESZ6");
    EXPECT_EQ(store.save(PositionStore::NO_SLOT, std::string(PositionStore::MAX_KEY_LENGTH + 1, 's'), position, {}),
              PositionStore::NO_SLOT);
    EXPECT_NE(store.save(PositionStore::NO_SLOT, std::string(PositionStore::MAX_KEY_LENGTH, 's'), position, {}),
              PositionStore::NO_SLOT);
}

// ── PositionTracker with a store ─────────────────────────────────────────────

// The point of it all: a restart carries on from the position held, and FIFO accounting
// closes the lot it would have closed had the process never stopped.
TEST_F(PositionStoreTest, TrackerCarriesItsPositionsAndLotsAcrossARestart) {
    {
        PositionStore store(path_, 16);
        PositionTracker tracker;
        tracker.attach_store(store);
        tracker.set_contract_multiplier("ESZ6", 50.0);
        tracker.on_fill(STRATEGY, make_fill("ESZ6", Side::BUY, 5000.0, 1, 1.25));
        tracker.on_fill(STRATEGY, make_fill("ESZ6", Side::BUY, 5010.0, 1, 1.25));
        tracker.on_fill("Other", make_fill("NQZ6", Side::SELL, 18000.0, 2));
    }

    PositionStore store(path_, 16);
    PositionTracker tracker;
    tracker.attach_store(store);

    auto pos = tracker.get_position(STRATEGY, "ESZ6");
    EXPECT_EQ(pos.quantity, to_qty_t(2));
    EXPECT_DOUBLE_EQ(from_price_t(pos.avg_price), 5005.0);
    EXPECT_DOUBLE_EQ(pos.multiplier, 50.0);
    EXPECT_DOUBLE_EQ(from_price_t(pos.open_fees), 2.5);
    EXPECT_DOUBLE_EQ(tracker.get_contract_multiplier("ESZ6"), 50.0);   // known again before its definition arrives
    EXPECT_EQ(tracker.get_position("Other", "NQZ6").quantity, to_qty_t(-2));

    // Selling one closes the 5000 lot, not an average: 20 points x $50.
    tracker.on_fill(STRATEGY, make_fill("ESZ6", Side::SELL, 5020.0, 1));
    pos = tracker.get_position(STRATEGY, "ESZ6");
    EXPECT_EQ(pos.quantity, to_qty_t(1));
    EXPECT_DOUBLE_EQ(from_price_t(pos.realized_pnl), 1000.0);

    // And that fill is itself kept.
    EXPECT_EQ(contents(store).at("StoreTest/ESZ6").position.realized_pnl, to_price_t(1000.0));
    EXPECT_EQ(contents(store).at("StoreTest/ESZ6").lots.size(), 1u);
    EXPECT_EQ(contents(store).at("StoreTest/ESZ6").lots[0].price, to_price_t(5010.0));
}

TEST_F(PositionStoreTest, AMultiplierLearnedLaterIsKeptWithThePosition) {
    {
        PositionStore store(path_, 16);
        PositionTracker tracker;
        tracker.attach_store(store);
        tracker.on_fill(STRATEGY, make_fill("ESZ6", Side::BUY, 5000.0, 1));   // before the definition
        tracker.set_contract_multiplier("ESZ6", 50.0);
    }
    PositionStore store(path_, 16);
    EXPECT_DOUBLE_EQ(contents(store).at("StoreTest/ESZ6").position.multiplier, 50.0);
}

TEST_F(PositionStoreTest, ClearingPositionsClearsThemOnDiskToo) {
    {
        PositionStore store(path_, 16);
        PositionTracker tracker;
        tracker.attach_store(store);
        tracker.on_fill(STRATEGY, make_fill("ESZ6", Side::BUY, 5000.0, 1));
        tracker.on_fill("Other", make_fill("ESZ6", Side::BUY, 5000.0, 1));
        tracker.clear_strategy_positions(STRATEGY);
    }
    PositionStore store(path_, 16);
    PositionTracker tracker;
    tracker.attach_store(store);
    EXPECT_TRUE(tracker.get_position(STRATEGY, "ESZ6").is_flat());
    EXPECT_EQ(tracker.get_position("Other", "ESZ6").quantity, to_qty_t(1));
    tracker.clear_all();
    EXPECT_EQ(store.size(), 0u);
}

// Without a store nothing changes: positions live in memory, as they always have.
TEST_F(PositionStoreTest, WithoutAStoreNothingIsWritten) {
    PositionTracker tracker;
    tracker.on_fill(STRATEGY, make_fill("ESZ6", Side::BUY, 5000.0, 1));
    EXPECT_EQ(tracker.get_position(STRATEGY, "ESZ6").quantity, to_qty_t(1));
    EXPECT_FALSE(std::filesystem::exists(path_));
}

// ── A broker's fill journal ──────────────────────────────────────────────────

// How far the strategies have got into a broker's journal survives a restart - also when
// the process died between booking a fill and marking it had: the position says so.
TEST_F(PositionStoreTest, HowFarIntoABrokersJournalTheStrategiesHaveGotIsKept) {
    {
        PositionStore store(path_, 4);
        EXPECT_EQ(store.journaled(BrokerId::CME), 0u);
        store.mark_journaled(BrokerId::CME, 90);
        Position position("ESZ6");
        position.quantity = to_qty_t(1);
        const uint32_t slot = store.save(PositionStore::NO_SLOT, STRATEGY, position, {}, BrokerId::CME, 100);
        position.quantity = to_qty_t(2);
        store.save(slot, STRATEGY, position, {});   // a fill with no journal number keeps the last one
        store.mark_journaled(BrokerId::COINBASE, 7);
    }
    {
        PositionStore store(path_, 4);
        EXPECT_EQ(store.journaled(BrokerId::CME), 100u) << "booked from fill 100, so the strategies had it";
        EXPECT_EQ(store.journaled(BrokerId::COINBASE), 7u);
        EXPECT_EQ(store.journaled(BrokerId::ALPACA), 0u);
        store.mark_journaled(BrokerId::CME, 50);   // never goes back
        EXPECT_EQ(store.journaled(BrokerId::CME), 100u);
        store.erase_strategy(STRATEGY);
    }
    PositionStore store(path_, 4);
    EXPECT_EQ(store.journaled(BrokerId::CME), 100u) << "the position is gone; how far the strategies got is not";
}

// The whole chain at a restart: the tracker takes up where the strategies had got, and a
// fill replayed from before that is known for one they had.
TEST_F(PositionStoreTest, TheTrackerKnowsAReplayedFillTheStrategiesHadBeforeARestart) {
    auto fill = make_fill("ESZ6", Side::BUY, 5000.0, 1);
    fill.broker_id = BrokerId::CME;
    fill.journal_seq = 41;
    {
        PositionStore store(path_, 4);
        PositionTracker tracker;
        tracker.attach_store(store);
        EXPECT_FALSE(tracker.fill_handled(fill));
        tracker.on_fill(STRATEGY, fill);
        // The process dies here, before the strategy manager marks the fill dispatched.
    }
    PositionStore store(path_, 4);
    PositionTracker tracker;
    tracker.attach_store(store);
    EXPECT_TRUE(tracker.fill_handled(fill)) << "booked: its replay is dropped";
    auto replayed = fill;
    replayed.replayed = true;
    EXPECT_TRUE(tracker.fill_handled(replayed));
    auto next = replayed;
    next.journal_seq = 42;
    EXPECT_FALSE(tracker.fill_handled(next)) << "never booked: its replay is booked";
    auto unjournaled = fill;
    unjournaled.journal_seq = 0;
    EXPECT_FALSE(tracker.fill_handled(unjournaled));
    tracker.fill_dispatched(next);
    EXPECT_TRUE(tracker.fill_handled(next));
    EXPECT_EQ(store.journaled(BrokerId::CME), 42u);
}

// A fill replayed at startup is booked before the instrument's definition says what a
// contract is worth: it says so itself.
TEST_F(PositionStoreTest, AFillBookedBeforeItsDefinitionIsValuedWithTheMultiplierItCarries) {
    PositionTracker tracker;
    auto buy = make_fill("ESZ6", Side::BUY, 5000.0, 1);
    buy.contract_multiplier = 50.0;
    auto sell = make_fill("ESZ6", Side::SELL, 5010.0, 1);
    sell.contract_multiplier = 50.0;
    tracker.on_fill(STRATEGY, buy);
    tracker.on_fill(STRATEGY, sell);
    EXPECT_DOUBLE_EQ(from_price_t(tracker.get_position(STRATEGY, "ESZ6").realized_pnl), 500.0)
        << "10 points on a 50-dollar contract, not on a 1-dollar one";
    EXPECT_DOUBLE_EQ(tracker.get_contract_multiplier("ESZ6"), 50.0);

    auto unsaid = make_fill("ESZ6", Side::BUY, 5000.0, 1);   // a broker that does not say
    tracker.on_fill(STRATEGY, unsaid);
    EXPECT_DOUBLE_EQ(tracker.get_position(STRATEGY, "ESZ6").multiplier, 50.0) << "keeps what it knows";
}
