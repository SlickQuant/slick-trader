// The dashboard's workspaces: named sets of tool windows, kept in one JSON file the engine reads
// back as it starts.

#include <gtest/gtest.h>

#include <slick-trader/web/workspace_store.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace slick::trader;
using nlohmann::json;

namespace {

class WorkspaceStoreTest : public ::testing::Test {
protected:
    void SetUp() override { std::filesystem::create_directories(dir_); }
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(dir_, ignored);
    }

    static json ladder(int left, const char* instrument) {
        return {{"tool", "ladder"}, {"left", left}, {"top", 10}, {"width", 360}, {"height", 720},
                {"params", {{"instrument", instrument}}}};
    }

    std::filesystem::path dir_ = std::filesystem::temp_directory_path() /
        ("slick-trader-workspaces-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string path_ = (dir_ / "state" / "workspaces.json").string();
};

} // namespace

TEST_F(WorkspaceStoreTest, NoFileIsDefaultAlone) {
    WorkspaceStore store;
    store.load(path_);
    EXPECT_EQ(store.doc(), (json{{"last", "Default"}, {"workspaces", {{"Default", {{"windows", json::array()}}}}}}));
}

// What is saved is what the engine reads back after a restart, the last one used too. The state
// dir is made on the first write.
TEST_F(WorkspaceStoreTest, ASaveSurvivesARestart) {
    const json windows = {ladder(-1920, "ZNZ6"), ladder(400, "ESZ6")};
    {
        WorkspaceStore store;
        store.load(path_);
        ASSERT_TRUE(store.save("Morning", windows).ok());
    }
    WorkspaceStore store;
    store.load(path_);
    EXPECT_EQ(store.doc()["last"], "Morning");
    EXPECT_EQ(store.doc()["workspaces"]["Morning"]["windows"], windows);
    EXPECT_TRUE(store.doc()["workspaces"].contains("Default"));
}

// Only the fields a window reopens with are kept.
TEST_F(WorkspaceStoreTest, AWindowKeepsItsOwnFieldsOnly) {
    WorkspaceStore store;
    store.load(path_);
    json window = ladder(0, "ZNZ6");
    window["junk"] = "x";
    ASSERT_TRUE(store.save("A", json::array({window})).ok());
    window.erase("junk");
    EXPECT_EQ(store.doc()["workspaces"]["A"]["windows"], json::array({window}));
}

TEST_F(WorkspaceStoreTest, DefaultCanBeSavedToButNotDeleted) {
    WorkspaceStore store;
    store.load(path_);
    ASSERT_TRUE(store.save("Default", json::array({ladder(0, "ZNZ6")})).ok());
    EXPECT_EQ(store.remove("Default").status, 400);
    EXPECT_EQ(store.doc()["workspaces"]["Default"]["windows"].size(), 1u);
}

TEST_F(WorkspaceStoreTest, DeletingTheLastUsedFallsBackToDefault) {
    WorkspaceStore store;
    store.load(path_);
    ASSERT_TRUE(store.save("Morning", json::array()).ok());
    ASSERT_TRUE(store.remove("Morning").ok());
    EXPECT_EQ(store.doc()["last"], "Default");
    EXPECT_EQ(store.remove("Morning").status, 404);
}

TEST_F(WorkspaceStoreTest, UseMarksTheLastUsed) {
    WorkspaceStore store;
    store.load(path_);
    ASSERT_TRUE(store.save("Morning", json::array()).ok());
    ASSERT_TRUE(store.use("Default").ok());
    EXPECT_EQ(store.doc()["last"], "Default");
    EXPECT_EQ(store.use("Nope").status, 404);
    WorkspaceStore reread;
    reread.load(path_);
    EXPECT_EQ(reread.doc()["last"], "Default");
}

TEST_F(WorkspaceStoreTest, BadNamesAndWindowsAreRefused) {
    WorkspaceStore store;
    store.load(path_);
    EXPECT_EQ(store.save("", json::array()).status, 400);
    EXPECT_EQ(store.save(std::string(65, 'x'), json::array()).status, 400);
    EXPECT_EQ(store.save("a\nb", json::array()).status, 400);
    EXPECT_EQ(store.save("A", json::object()).status, 400);
    EXPECT_EQ(store.save("A", json::array({{{"tool", "ladder"}, {"left", 0}}})).status, 400);
    EXPECT_EQ(store.save("A", json::array({{{"tool", "x"}, {"left", 0.5}, {"top", 0}, {"width", 1}, {"height", 1}}})).status, 400);
    json number_param = ladder(0, "ZNZ6");
    number_param["params"]["instrument"] = 5;
    EXPECT_EQ(store.save("A", json::array({number_param})).status, 400);
    EXPECT_FALSE(store.doc()["workspaces"].contains("A"));
}

// A save the file can't take is refused whole: the store keeps what the file has.
TEST_F(WorkspaceStoreTest, AFailedWriteChangesNothing) {
    std::ofstream(dir_ / "state") << "a file where the dir should be";
    WorkspaceStore store;
    store.load(path_);
    const auto result = store.save("Morning", json::array());
    EXPECT_EQ(result.status, 500);
    EXPECT_FALSE(store.doc()["workspaces"].contains("Morning"));
    EXPECT_EQ(store.doc()["last"], "Default");
}

TEST_F(WorkspaceStoreTest, AnUnreadableFileLoadsAsDefaultAlone) {
    std::filesystem::create_directories(dir_ / "state");
    std::ofstream(path_) << "{ not json";
    WorkspaceStore store;
    store.load(path_);
    EXPECT_EQ(store.doc()["workspaces"].size(), 1u);
    EXPECT_EQ(store.doc()["last"], "Default");
}

// A hand edit that broke one workspace drops that one, and a last naming it falls back to Default.
// The dashboard trims a name; the API did not, so "Morning " sat next to Morning.
TEST_F(WorkspaceStoreTest, BlanksAroundANameAreNotPartOfIt) {
    WorkspaceStore store;
    store.load(path_);
    ASSERT_TRUE(store.save(" Morning\t", json::array({ladder(10, "ZNZ6")})).ok());
    ASSERT_TRUE(store.save("Morning", json::array({ladder(20, "ESZ6")})).ok());
    EXPECT_EQ(store.doc()["workspaces"].size(), 2u);
    EXPECT_EQ(store.doc()["workspaces"]["Morning"]["windows"][0]["left"], 20);
    ASSERT_TRUE(store.save("Default ", json::array({ladder(30, "ZNZ6")})).ok());
    EXPECT_EQ(store.doc()["workspaces"].size(), 2u);
    EXPECT_EQ(store.doc()["workspaces"]["Default"]["windows"][0]["left"], 30);
    EXPECT_TRUE(store.use("Morning ").ok());
    EXPECT_EQ(store.doc()["last"], "Morning");
    EXPECT_EQ(store.save("\xE3\x80\x80", json::array()).status, 400);   // an ideographic space alone
    EXPECT_EQ(store.remove(" Default").status, 400);
    EXPECT_TRUE(store.remove("\xC2\xA0Morning").ok());                  // after a no-break space
    EXPECT_FALSE(store.doc()["workspaces"].contains("Morning"));
}

TEST_F(WorkspaceStoreTest, ANameWithBlanksInTheFileLoadsTrimmed) {
    std::filesystem::create_directories(dir_ / "state");
    std::ofstream(path_) << json{{"last", "Evening "},
                                 {"workspaces", {{"Morning", {{"windows", {ladder(1, "ZNZ6")}}}},
                                                 {"Morning ", {{"windows", {ladder(2, "ESZ6")}}}},
                                                 {"Evening ", {{"windows", json::array()}}}}}}.dump();
    WorkspaceStore store;
    store.load(path_);
    const auto& workspaces = store.doc()["workspaces"];
    EXPECT_EQ(workspaces.size(), 3u);
    EXPECT_EQ(workspaces["Morning"]["windows"][0]["left"], 1);
    EXPECT_TRUE(workspaces.contains("Evening"));
    EXPECT_EQ(store.doc()["last"], "Evening");
    EXPECT_TRUE(store.remove("Evening").ok());
}

TEST_F(WorkspaceStoreTest, ABrokenWorkspaceInTheFileIsDroppedAlone) {
    std::filesystem::create_directories(dir_ / "state");
    std::ofstream(path_) << json{{"last", "Broken"},
                                 {"workspaces", {{"Good", {{"windows", {ladder(0, "ZNZ6")}}}},
                                                 {"Broken", {{"windows", {{{"tool", "ladder"}}}}}}}}}.dump();
    WorkspaceStore store;
    store.load(path_);
    EXPECT_TRUE(store.doc()["workspaces"].contains("Good"));
    EXPECT_FALSE(store.doc()["workspaces"].contains("Broken"));
    EXPECT_EQ(store.doc()["last"], "Default");
}
