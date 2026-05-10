#pragma once

#include <slick-trader/events/event.hpp>
#include <slick-trader/utils/types.hpp>
#include <slick-trader/utils/order_types.hpp>
#include <type_traits>
#include <string_view>
#include <cstring>
#include <algorithm>

namespace slick::trader {

constexpr size_t MAX_ORDER_ID_LENGTH = 64;
constexpr size_t MAX_REJECT_REASON_LENGTH = 128;

#pragma pack(push, 1)
struct OrderEventBase : public Event {
    char client_order_id[64];
    Side side;

    // Base event for order-related events
    OrderEventBase(EventType t)
        : Event(t)
        , client_order_id{}
        , side(Side::BUY)
    {}

    void set_client_order_id(std::string_view id) {
        size_t len = std::min(id.size(), size_t(63));
        std::memcpy(client_order_id, id.data(), len);
        client_order_id[len] = '\0';
    }
};

struct OrderEvent : public OrderEventBase {
    char order_id[MAX_ORDER_ID_LENGTH];

    OrderEvent(EventType t)
        : OrderEventBase(t)
        , order_id{}
    {}

    void set_order_id(std::string_view id) {
        size_t len = std::min(id.size(), size_t(63));
        std::memcpy(order_id, id.data(), len);
        order_id[len] = '\0';
    }
};

struct NewOrderEvent : public OrderEventBase {
    OrderType order_type;
    price_t price;          // Limit price (or take-profit for BRACKET)
    price_t stop_price;     // Stop price (for STOP, STOP_LIMIT, BRACKET)
    qty_t quantity;
    TimeInForce time_in_force;
    bool post_only;

    NewOrderEvent()
        : OrderEventBase(EventType::NEW_ORDER)
        , order_type(OrderType::LIMIT)
        , price(NULL_PRICE)
        , stop_price(NULL_PRICE)
        , quantity(0)
        , time_in_force(TimeInForce::GTD)
        , post_only(true)
    {}
};

struct ModifyOrderEvent : public OrderEvent {
    price_t new_price;
    price_t new_stop_price;
    qty_t new_quantity;
    TimeInForce new_time_in_force;
    bool new_post_only;

    ModifyOrderEvent()
        : OrderEvent(EventType::MODIFY_ORDER)
        , new_price(0)
        , new_stop_price(0)
        , new_quantity(0)
        , new_time_in_force(TimeInForce::GTD)
        , new_post_only(false)
    {}
};

struct CancelOrderEvent : public OrderEvent {
    CancelOrderEvent()
        : OrderEvent(EventType::CANCEL_ORDER)
    {}
};

struct OrderAckEvent : public OrderEvent {
    OrderType order_type;
    price_t price;          // Limit price (or take-profit for BRACKET orders)
    price_t stop_price;     // Stop price (for STOP, STOP_LIMIT, BRACKET orders)
    qty_t quantity;
    OrderStatus status;
    TimeInForce time_in_force;
    bool post_only;

    OrderAckEvent()
        : OrderEvent(EventType::ORDER_ACK)
        , order_type(OrderType::LIMIT)
        , price(0)
        , stop_price(0)
        , quantity(0)
        , status(OrderStatus::ACKNOWLEDGED)
        , time_in_force(TimeInForce::GTD)
        , post_only(false)
    {}
};

// OrderFillEvent
struct OrderFillEvent : public OrderEvent {
    char fill_id[MAX_ORDER_ID_LENGTH];
    price_t fill_price;
    qty_t fill_quantity;
    qty_t cumulative_filled;
    qty_t remaining_quantity;
    price_t fee_amount{0};  // actual fee for this fill (0 if broker doesn't report it)

    OrderFillEvent()
        : OrderEvent(EventType::ORDER_FILL)
        , fill_id{}
        , fill_price(0)
        , fill_quantity(0)
        , cumulative_filled(0)
        , remaining_quantity(0)
        , fee_amount(0)
    {}

    void set_fill_id(std::string_view id) {
        size_t len = std::min(id.size(), MAX_ORDER_ID_LENGTH - 1);
        std::memcpy(fill_id, id.data(), len);
        fill_id[len] = '\0';
    }
};

// OrderRejectEvent
struct OrderRejectEvent : public OrderEvent {
    char reason[MAX_REJECT_REASON_LENGTH];
    price_t price;
    price_t stop_price;
    qty_t quantity;

    OrderRejectEvent()
        : OrderEvent(EventType::ORDER_REJECT)
        , reason{}
        , price(0)
        , stop_price(0)
        , quantity(0) {}

    void set_reason(std::string_view r) {
        size_t len = std::min(r.size(), MAX_REJECT_REASON_LENGTH - 1);
        std::memcpy(reason, r.data(), len);
        reason[len] = '\0';
    }
};

// OrderCancelEvent
struct OrderCancelEvent : public OrderEvent {
    char reason[MAX_REJECT_REASON_LENGTH];
    qty_t remaining_quantity;

    OrderCancelEvent()
        : OrderEvent(EventType::ORDER_CANCEL)
        , reason{}
        , remaining_quantity(0) {}

    void set_reason(std::string_view r) {
        size_t len = std::min(r.size(), MAX_REJECT_REASON_LENGTH - 1);
        std::memcpy(reason, r.data(), len);
        reason[len] = '\0';
    }
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<NewOrderEvent>, "NewOrderEvent must be trivially copyable");
static_assert(std::is_trivially_copyable_v<CancelOrderEvent>, "CancelOrderEvent must be trivially copyable");
static_assert(std::is_trivially_copyable_v<OrderAckEvent>, "OrderAckEvent must be trivially copyable");
static_assert(std::is_trivially_copyable_v<OrderFillEvent>, "OrderFillEvent must be trivially copyable");
static_assert(std::is_trivially_copyable_v<OrderRejectEvent>, "OrderRejectEvent must be trivially copyable");
static_assert(std::is_trivially_copyable_v<OrderCancelEvent>, "OrderCancelEvent must be trivially copyable");

} // namespace slick::trader
