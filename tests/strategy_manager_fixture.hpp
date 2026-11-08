#pragma once

// A StrategyManager with everything it needs to be driven without its worker thread:
// events go in through manager_.process_event(), market data requests come out on md_qs_,
// a queue for every broker, and what it publishes itself can be read off mux_.

#include <slick-trader/core/md_request_queues.hpp>
#include <slick-trader/core/pnl_calculator.hpp>
#include <slick-trader/core/position_tracker.hpp>
#include <slick-trader/core/risk_manager.hpp>
#include <slick-trader/core/strategy_manager.hpp>
#include <slick-trader/strategy/strategy_base.hpp>
#include <slick/queue.hpp>
#include <slick/stream_buffer_multiplexer.hpp>

#include <memory>

namespace slick::trader::test {

// A request queue of `size` bytes for every broker, as an engine that built them all has.
inline MdRequestQueues every_broker_queues(uint32_t size) {
    MdRequestQueues queues(size);
    for (size_t i = 1; i < MdRequestQueues::BROKER_COUNT; ++i) {
        queues.get(static_cast<BrokerId>(i));
    }
    return queues;
}

struct StrategyManagerFixture {
    // Initializes a strategy as the engine would, with `params` as its config's "parameters",
    // and hands it to the manager.
    template <typename Strategy>
    void add_strategy(const std::shared_ptr<Strategy>& strategy, const nlohmann::json& params = nlohmann::json::object()) {
        strategy->initialize(params, positions_, pnl_, risk_, order_q_);
        manager_.add_strategy(strategy);
    }

    PositionTracker positions_;
    PnLCalculator   pnl_{positions_};
    RiskManager     risk_{positions_};
    slick::stream_buffer_multiplexer mux_{1024};
    MdRequestQueues md_qs_ = every_broker_queues(65536);
    slick::queue<uint8_t> order_q_{65536};
    StrategyManager manager_{nlohmann::json{{"TOB_queue_size", 65536}, {"TOB_control_size", 4096}},
                             positions_, pnl_, risk_, mux_, md_qs_, order_q_};
};

} // namespace slick::trader::test
