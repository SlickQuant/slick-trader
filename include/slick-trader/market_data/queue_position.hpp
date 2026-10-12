#pragma once

#include <slick-trader/market_data/order_book.hpp>
#include <slick-trader/utils/types.hpp>
#include <slick-trader/utils/order_types.hpp>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace slick::trader {

// Where one resting order stands in the queue at its price, read from a market-by-order book.
struct QueuePosition {
    uint32_t orders_ahead{0};       // orders at the same price that trade before this one
    qty_t quantity_ahead{0};        // their displayed quantity: what must trade or cancel first
    qty_t quantity{0};              // the order's own displayed quantity
    qty_t level_quantity{0};        // everything displayed at the price, this order included
    uint32_t level_orders{0};       // orders at the price, this one included
    uint16_t level_index{0};        // 0 = the best price on its side
    price_t price{0};
    Side side{Side::BUY};
};

// The queue position of `order_id`, or nothing when the book does not hold it - not yet
// acknowledged on the feed, filled, or cancelled. Costs one hop per order ahead.
inline std::optional<QueuePosition> queue_position(const slick::orderbook::OrderBookL3& book,
                                                   slick::orderbook::OrderId order_id) noexcept {
    const auto* order = book.findOrder(order_id);
    if (order == nullptr) {
        return std::nullopt;
    }

    QueuePosition position;
    position.quantity = static_cast<qty_t>(order->quantity);
    position.price    = static_cast<price_t>(order->price);
    position.side     = order->side == slick::orderbook::Side::Buy ? Side::BUY : Side::SELL;

    // A level keeps its orders in priority order: everything reached through `prev` trades first.
    for (const auto* ahead = order->prev; ahead != nullptr; ahead = ahead->prev) {
        ++position.orders_ahead;
        position.quantity_ahead += static_cast<qty_t>(ahead->quantity);
    }

    const auto [level, index] = book.getLevel(order->side, order->price);
    if (level != nullptr) [[likely]] {
        position.level_quantity = static_cast<qty_t>(level->getTotalQuantity());
        position.level_orders   = static_cast<uint32_t>(level->orderCount());
        position.level_index    = index;
    }
    return position;
}

// The same, by the order id as order events carry it: text. Nothing when the id is not a
// number, which is how brokers without a market-by-order feed name their orders.
inline std::optional<QueuePosition> queue_position(const slick::orderbook::OrderBookL3& book,
                                                   std::string_view order_id) noexcept {
    slick::orderbook::OrderId id = 0;
    const auto* end = order_id.data() + order_id.size();
    const auto [parsed, error] = std::from_chars(order_id.data(), end, id);
    if (error != std::errc{} || parsed != end || order_id.empty()) {
        return std::nullopt;
    }
    return queue_position(book, id);
}

} // namespace slick::trader
