#pragma once

#include <slick-trader/events/event.hpp>
#include <slick-trader/utils/types.hpp>
#include <cstring>

namespace slick::trader {

constexpr size_t MAX_STRATEGY_ID_LENGTH = 64;

#pragma pack(push, 1)

// Position update event (published to position_update_queue)
struct PositionUpdateEvent : public Event {
    char strategy_id[MAX_STRATEGY_ID_LENGTH];
    qty_t quantity;           // Current position
    price_t avg_price;        // Average entry price
    price_t realized_pnl;     // Realized PnL
    uint64_t last_update_time;

    PositionUpdateEvent()
        : Event(EventType::POSITION_UPDATE)
        , strategy_id{}
        , quantity(0)
        , avg_price(0)
        , realized_pnl(0)
        , last_update_time(0) {}

    void set_strategy_id(std::string_view id) {
        size_t len = std::min(id.size(), MAX_STRATEGY_ID_LENGTH - 1);
        std::memcpy(strategy_id, id.data(), len);
        strategy_id[len] = '\0';
    }
};

// PnL snapshot event (published to pnl_update_queue)
struct PnLUpdateEvent : public Event {
    char strategy_id[MAX_STRATEGY_ID_LENGTH];
    price_t realized_pnl;
    price_t unrealized_pnl;
    price_t total_pnl;

    PnLUpdateEvent()
        : Event(EventType::PNL_UPDATE)
        , strategy_id{}
        , realized_pnl(0)
        , unrealized_pnl(0)
        , total_pnl(0) {}

    void set_strategy_id(std::string_view id) {
        size_t len = std::min(id.size(), MAX_STRATEGY_ID_LENGTH - 1);
        std::memcpy(strategy_id, id.data(), len);
        strategy_id[len] = '\0';
    }
};

#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<PositionUpdateEvent>);
static_assert(std::is_trivially_copyable_v<PnLUpdateEvent>);

} // namespace slick::trader
