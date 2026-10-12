// An instrument's trading state, as a broker publishes it, reaches every strategy.

#include <gtest/gtest.h>

#include "strategy_manager_fixture.hpp"

#include <slick-trader/events/instrument_status_event.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace slick::trader;

namespace {

class StatusStrategy : public StrategyBase {
public:
    StatusStrategy() : StrategyBase("Status", "1.0.0") {}

    void on_instrument_status(const InstrumentStatusEvent& event) override { seen.push_back(event); }

    std::vector<InstrumentStatusEvent> seen;
};

class InstrumentStatusTest : public ::testing::Test, protected test::StrategyManagerFixture {
protected:
    void SetUp() override { add_strategy(strategy_); }

    std::shared_ptr<StatusStrategy> strategy_ = std::make_shared<StatusStrategy>();
};

} // namespace

TEST_F(InstrumentStatusTest, AHaltReachesTheStrategyWithItsReason) {
    InstrumentStatusEvent halt;
    halt.broker_id = BrokerId::CME;
    halt.set_instrument("ESZ6");
    halt.status = TradingStatus::HALTED;
    halt.cancel_allowed = false;
    halt.set_reason("MARKET_EVENT");
    halt.event_time_ns = 42;
    manager_.process_event(&halt);

    ASSERT_EQ(strategy_->seen.size(), 1u);
    const InstrumentStatusEvent seen = strategy_->seen[0];
    EXPECT_STREQ(seen.instrument, "ESZ6");
    EXPECT_EQ(seen.broker_id, BrokerId::CME);
    EXPECT_EQ(seen.status, TradingStatus::HALTED);
    EXPECT_FALSE(seen.cancel_allowed);
    EXPECT_STREQ(seen.reason, "MARKET_EVENT");
    EXPECT_EQ(seen.event_time_ns, 42u);
}

TEST_F(InstrumentStatusTest, AReasonLongerThanItsFieldIsCutNotOverrun) {
    InstrumentStatusEvent event;
    event.set_reason(std::string(100, 'x'));
    EXPECT_EQ(std::string(event.reason).size(), MAX_STATUS_REASON_LENGTH - 1);
    EXPECT_STREQ(to_string(TradingStatus::PRE_OPEN), "PRE_OPEN");
    EXPECT_STREQ(to_string(event.status), "UNKNOWN");
}
