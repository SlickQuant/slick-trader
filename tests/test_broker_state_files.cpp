// The files an order connector keeps across restarts: the fill journal (every fill
// written down before it is published) and the slot store (a session's state and its
// working orders, each record updated by writing a second copy and flipping a byte).

#include <gtest/gtest.h>
#include <slick-trader/brokers/common/fill_journal.hpp>
#include <slick-trader/brokers/common/slot_store.hpp>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace slick::trader;

namespace {

constexpr uint64_t CREATED = 1'760'000'000'000'000'000ULL;   // wall-clock ns
constexpr FillJournal::Format JOURNAL{{'S', 'L', 'K', 'T', 'S', 'T', 'J', '1'}, 1, "TestJournal"};
constexpr SlotStoreFormat STORE{{'S', 'L', 'K', 'T', 'S', 'T', 'S', '1'}, 1, "TestStore"};

struct State {
    uint64_t epoch = 7;
    uint32_t next_in = 1;
    uint32_t next_out = 1;
};

struct Record {
    uint64_t id;
    int64_t price;
    uint32_t qty;
    uint32_t cum;
};

using Store = SlotStore<State, Record>;

Store::Identity identity(const char* text) {
    Store::Identity id{};
    std::memcpy(id.data(), text, std::min(std::strlen(text), id.size()));
    return id;
}

class BrokerStateFilesTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = std::filesystem::temp_directory_path() / "slick_trader_state_files_test" / info->name();
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::string path(const char* name) const { return (dir_ / name).string(); }

    static OrderFillEvent fill(const char* id) {
        OrderFillEvent e;
        e.broker_id = BrokerId::ICE;
        e.set_instrument("BRN FMZ0026");
        e.set_fill_id(id);
        e.fill_quantity = to_qty_t(1);
        return e;
    }

    std::filesystem::path dir_;
};

} // namespace

TEST_F(BrokerStateFilesTest, JournalNumbersFillsKeepsThemAndRefusesAReportTwice) {
    {
        FillJournal journal(path("fills.journal"), JOURNAL, 8, CREATED);
        for (uint32_t seq = 1; seq <= 3; ++seq) {
            auto e = fill(std::to_string(seq).c_str());
            ASSERT_TRUE(journal.record(e, {1, 100, seq, 0}));
            EXPECT_EQ(e.journal_seq, CREATED + seq - 1);
        }
        auto again = fill("3");
        EXPECT_FALSE(journal.record(again, {1, 100, 3, 0})) << "the report handed over again";
        EXPECT_TRUE(journal.journaled({1, 100, 2, 0}));
        EXPECT_FALSE(journal.journaled({1, 101, 1, 0})) << "a later epoch starts afresh";
    }
    FillJournal reopened(path("fills.journal"), JOURNAL, 8, CREATED + 1'000'000);
    ASSERT_EQ(reopened.size(), 3u);
    const auto fills = reopened.fills();
    EXPECT_STREQ(fills.front().fill_id, "1");
    EXPECT_STREQ(fills.back().fill_id, "3");
    EXPECT_EQ(reopened.next_seq(), CREATED + 3) << "numbers never repeat";
    EXPECT_TRUE(reopened.journaled({1, 100, 3, 0}));
}

TEST_F(BrokerStateFilesTest, JournalIsARingThatKeepsTheNewest) {
    FillJournal journal(path("fills.journal"), JOURNAL, 2, CREATED);
    for (uint32_t seq = 1; seq <= 5; ++seq) {
        auto e = fill(std::to_string(seq).c_str());
        ASSERT_TRUE(journal.record(e, {1, 100, seq, 0}));
    }
    const auto fills = journal.fills();
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_STREQ(fills[0].fill_id, "4");
    EXPECT_STREQ(fills[1].fill_id, "5");
}

TEST_F(BrokerStateFilesTest, JournalRefusesAnotherFormatsFile) {
    { FillJournal journal(path("fills.journal"), JOURNAL, 4, CREATED); }
    constexpr FillJournal::Format OTHER{{'S', 'L', 'K', 'O', 'T', 'H', 'R', '1'}, 1, "Other"};
    EXPECT_THROW(FillJournal(path("fills.journal"), OTHER, 4, CREATED), std::runtime_error);
    constexpr FillJournal::Format NEWER{{'S', 'L', 'K', 'T', 'S', 'T', 'J', '1'}, 2, "TestJournal"};
    EXPECT_THROW(FillJournal(path("fills.journal"), NEWER, 4, CREATED), std::runtime_error);
}

TEST_F(BrokerStateFilesTest, StoreKeepsStateAndRecordsAcrossReopening) {
    uint32_t a = 0;
    {
        Store store(path("s.state"), STORE, identity("ABC:S01"), "ABC:S01", 4);
        EXPECT_TRUE(store.created());
        EXPECT_EQ(store.state().epoch, 7u) << "a new file starts from State{}";
        store.state().next_in = 42;
        a = store.save(Store::NO_SLOT, {1, 100, 5, 0});
        const uint32_t b = store.save(Store::NO_SLOT, {2, 101, 3, 0});
        store.save(a, {1, 100, 5, 2});   // updated in place
        store.release(b);
        EXPECT_EQ(store.size(), 1u);
    }
    Store store(path("s.state"), STORE, identity("ABC:S01"), "ABC:S01", 4);
    EXPECT_FALSE(store.created());
    EXPECT_EQ(store.state().next_in, 42u);
    int seen = 0;
    store.for_each([&](uint32_t slot, const Record& r) {
        ++seen;
        EXPECT_EQ(slot, a);
        EXPECT_EQ(r.id, 1u);
        EXPECT_EQ(r.cum, 2u) << "the current copy";
    });
    EXPECT_EQ(seen, 1);
}

TEST_F(BrokerStateFilesTest, AFullStoreSaysSoAndKeepsWhatItHas) {
    Store store(path("s.state"), STORE, identity("ABC:S01"), "ABC:S01", 2);
    EXPECT_NE(store.save(Store::NO_SLOT, {1, 1, 1, 0}), Store::NO_SLOT);
    EXPECT_NE(store.save(Store::NO_SLOT, {2, 1, 1, 0}), Store::NO_SLOT);
    EXPECT_EQ(store.save(Store::NO_SLOT, {3, 1, 1, 0}), Store::NO_SLOT);
    EXPECT_EQ(store.size(), 2u);
}

TEST_F(BrokerStateFilesTest, StoreRefusesAnotherSessionsOrFormatsFile) {
    { Store store(path("s.state"), STORE, identity("ABC:S01"), "ABC:S01", 2); }
    EXPECT_THROW((Store(path("s.state"), STORE, identity("ABC:S02"), "ABC:S02", 2)),
                 std::runtime_error);
    constexpr SlotStoreFormat OTHER{{'S', 'L', 'K', 'O', 'T', 'H', 'R', '1'}, 1, "Other"};
    EXPECT_THROW((Store(path("s.state"), OTHER, identity("ABC:S01"), "ABC:S01", 2)),
                 std::runtime_error);
    {
        std::ofstream junk(path("junk.state"), std::ios::binary);
        junk << "not a session store";
    }
    EXPECT_THROW((Store(path("junk.state"), STORE, identity("ABC:S01"), "ABC:S01", 2)),
                 std::runtime_error);
}
