// A Fills window opened after the fills went by had nothing to show: they reached the dashboard
// over the WebSocket alone. The WebServer keeps the last ones, each as the message it was sent as.

#include <gtest/gtest.h>

#include <slick-trader/web/fill_history.hpp>

#include <string>

using namespace slick::trader;

namespace {

nlohmann::json fill(int n) {
    return {{"type", "order_update"}, {"event", "fill"}, {"fill_id", "F" + std::to_string(n)}};
}

} // namespace

TEST(FillHistoryTest, NoneIsAnEmptyArray) {
    FillHistory history(4);
    std::string out = "stale";
    history.write_json(out);
    EXPECT_EQ(out, "[]");
}

// The text kept is the text sent, with its seq, so a page that read the history knows what it has.
TEST(FillHistoryTest, AFillIsKeptAsItIsSent) {
    FillHistory history(4);
    auto message = fill(1);
    const std::string sent = history.push(message);
    EXPECT_EQ(nlohmann::json::parse(sent)["seq"], 1);
    std::string out;
    history.write_json(out);
    EXPECT_EQ(out, "[" + sent + "]");
}

// Full, it drops the oldest, and lists the rest oldest first, their seqs going on.
TEST(FillHistoryTest, PastItsCapacityItKeepsTheLatestInOrder) {
    FillHistory history(3);
    for (int n = 1; n <= 5; ++n) {
        auto message = fill(n);
        history.push(message);
    }
    std::string out;
    history.write_json(out);
    const auto kept = nlohmann::json::parse(out);
    ASSERT_EQ(kept.size(), 3u);
    for (size_t i = 0; i < kept.size(); ++i) {
        EXPECT_EQ(kept[i]["seq"], i + 3);
        EXPECT_EQ(kept[i]["fill_id"], "F" + std::to_string(i + 3));
    }
    EXPECT_EQ(history.size(), 3u);
    EXPECT_EQ(history.last_seq(), 5u);
}
