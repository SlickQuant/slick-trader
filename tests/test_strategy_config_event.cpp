// Wire format for strategy parameter updates.
//
// PUT /api/strategies/:id/config used to call set_config() straight from the uWS event loop
// thread, which left the strategy applying the change on the wrong thread. It now publishes a
// StrategyConfigEvent onto the same stream_buffer_multiplexer the market data rides, and the
// StrategyManager worker applies it on its own thread.
//
// The parameters travel as JSON text in a trailing flexible array, so the published size is
// size_for(text.size()) rather than sizeof(StrategyConfigEvent). Get that wrong and the payload
// is silently truncated at the far end — which is exactly what these tests pin down.

#include <gtest/gtest.h>
#include <slick-trader/events/event_serializer.hpp>
#include <slick-trader/events/strategy_config_event.hpp>
#include <slick/stream_buffer_multiplexer.hpp>

#include <nlohmann/json.hpp>
#include <string>

namespace slick::trader {
namespace {

using nlohmann::json;

class StrategyConfigEventTest : public ::testing::Test {
protected:
    StrategyConfigEventTest()
        : mux_(1024)
        , producer_(mux_.add_producer(0, 1u << 16, 4096))
        , cursor_(mux_.initial_reading_index())
    {}

    void publish(const std::string& strategy, const std::string& text) {
        EventSerializer::publish<StrategyConfigEvent>(
            *producer_, StrategyConfigEvent::size_for(text.size()),
            [&](StrategyConfigEvent& event) {
                event.set_strategy(strategy);
                event.set_config(text);
            });
    }

    // The next record off the bus, as the StrategyManager worker would see it.
    const StrategyConfigEvent* read() {
        auto record = mux_.read(cursor_);
        if (!record) return nullptr;
        const auto* base = reinterpret_cast<const Event*>(record.data);
        if (base->type != EventType::STRATEGY_CONFIG) return nullptr;
        return reinterpret_cast<const StrategyConfigEvent*>(base);
    }

    slick::stream_buffer_multiplexer mux_;
    std::shared_ptr<slick::stream_buffer_multiplexer::producer_buffer> producer_;
    uint64_t cursor_;
};

TEST_F(StrategyConfigEventTest, CarriesStrategyNameAndParametersIntact) {
    const json params = {
        {"instrument",      "BTC-USD"},
        {"level_gap_ticks", 3},
        {"stack_qty",       0.002},
        {"post_only",       true},
    };
    publish("OrderStacker", params.dump());

    const auto* event = read();
    ASSERT_NE(event, nullptr);
    EXPECT_STREQ(event->strategy, "OrderStacker");
    EXPECT_EQ(json::parse(event->config_text()), params);
}

// The bug this format invites: sizing the record with sizeof() instead of size_for(), which
// publishes the header and drops every byte of JSON behind it.
TEST_F(StrategyConfigEventTest, PayloadSurvivesBeyondTheHeaderLength) {
    json params = json::object();
    for (int i = 0; i < 200; ++i) {
        params["parameter_with_a_long_name_" + std::to_string(i)] = i * 1.5;
    }
    const std::string text = params.dump();
    ASSERT_GT(text.size(), sizeof(StrategyConfigEvent)) << "payload must outgrow the header";

    publish("OrderStacker", text);

    const auto* event = read();
    ASSERT_NE(event, nullptr);
    EXPECT_EQ(event->config_length, text.size());
    EXPECT_EQ(json::parse(event->config_text()), params);
}

TEST_F(StrategyConfigEventTest, LongStrategyNameIsTruncatedNotOverrun) {
    const std::string name(MAX_STRATEGY_NAME_LENGTH * 2, 'x');
    publish(name, "{}");

    const auto* event = read();
    ASSERT_NE(event, nullptr);
    EXPECT_EQ(std::strlen(event->strategy), MAX_STRATEGY_NAME_LENGTH - 1);
    EXPECT_EQ(json::parse(event->config_text()), json::object());
}

// Two updates in flight at once must not blur into each other.
TEST_F(StrategyConfigEventTest, SuccessiveUpdatesArriveInOrder) {
    publish("OrderStacker", json{{"level_gap_ticks", 2}}.dump());
    publish("MarketMaker",  json{{"order_size", 0.005}}.dump());

    const auto* first = read();
    ASSERT_NE(first, nullptr);
    EXPECT_STREQ(first->strategy, "OrderStacker");
    EXPECT_EQ(json::parse(first->config_text())["level_gap_ticks"], 2);

    const auto* second = read();
    ASSERT_NE(second, nullptr);
    EXPECT_STREQ(second->strategy, "MarketMaker");
    EXPECT_DOUBLE_EQ(json::parse(second->config_text())["order_size"].get<double>(), 0.005);

    EXPECT_EQ(read(), nullptr) << "nothing else on the bus";
}

}  // namespace
}  // namespace slick::trader
