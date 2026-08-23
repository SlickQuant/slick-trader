#pragma once

#include <slick-trader/events/event.hpp>
#include <algorithm>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace slick::trader {

constexpr size_t MAX_STRATEGY_NAME_LENGTH = 64;

// Upper bound on the JSON text one event may carry. Nothing structural — the producer
// buffer would take far more — but a strategy's "parameters" block is a handful of
// scalars, so anything approaching this is a caller mistake worth rejecting at the API
// edge rather than silently truncating on the wire.
constexpr size_t MAX_STRATEGY_CONFIG_LENGTH = 64 * 1024;

// A strategy parameter update, handed from the WebServer thread to the StrategyManager
// worker thread over the event bus.
//
// The parameters travel as JSON *text* in a trailing flexible array, the same idiom
// Level2UpdateEvent uses for its price levels: the bus carries trivially copyable records
// only, and nlohmann::json is neither trivially copyable nor fixed-size. The consumer
// parses it back on the worker thread, which is the whole point — the strategy's
// on_config() then runs on the thread that owns its state.
//
// Publish with size_for(json.size()), never sizeof(StrategyConfigEvent).
#pragma pack(push, 1)
struct StrategyConfigEvent : public Event {
    char     strategy[MAX_STRATEGY_NAME_LENGTH];
    uint32_t config_length;   // bytes of JSON text in `config`, unterminated
    char     config[0];       // JSON text, published inline behind the header

    StrategyConfigEvent()
        : Event(EventType::STRATEGY_CONFIG)
        , strategy{}
        , config_length(0)
    {}

    void set_strategy(std::string_view name) {
        const size_t len = std::min(name.size(), MAX_STRATEGY_NAME_LENGTH - 1);
        std::memcpy(strategy, name.data(), len);
        strategy[len] = '\0';
    }

    void set_config(std::string_view json_text) {
        config_length = static_cast<uint32_t>(json_text.size());
        std::memcpy(config, json_text.data(), json_text.size());
    }

    std::string_view config_text() const {
        return {config, config_length};
    }

    static uint32_t size_for(size_t json_length) {
        return static_cast<uint32_t>(sizeof(StrategyConfigEvent) + json_length);
    }
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<StrategyConfigEvent>,
              "StrategyConfigEvent must be trivially copyable");

} // namespace slick::trader
