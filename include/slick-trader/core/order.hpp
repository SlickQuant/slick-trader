#pragma once

#include <slick-trader/utils/types.hpp>
#include <slick-trader/utils/order_types.hpp>
#include <string>
#include <cstdint>

namespace slick::trader {

struct Order {
    std::string order_id;
    std::string client_order_id;
    std::string instrument;
    Side side{Side::BUY};
    OrderType type{OrderType::LIMIT};
    TimeInForce time_in_force{TimeInForce::GTC};
    price_t price{0};
    price_t stop_price{0};
    qty_t qty{0};
    qty_t filled_qty{0};
    qty_t remaining_qty{0};
    price_t avg_fill_price{0};   // updated incrementally on each fill
    OrderStatus status{OrderStatus::PENDING_NEW};
    uint64_t created_time{0};
    uint64_t last_update_time{0};
    bool post_only{false};

    bool is_open() const {
        return status == OrderStatus::PENDING_NEW ||
               status == OrderStatus::ACKNOWLEDGED ||
               status == OrderStatus::PARTIALLY_FILLED;
    }

    bool is_done() const {
        return status == OrderStatus::FILLED ||
               status == OrderStatus::CANCELLED ||
               status == OrderStatus::REJECTED;
    }
};

} // namespace slick::trader
