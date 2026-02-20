#pragma once

#include <slick-trader/events/event.hpp>
#include <slick-trader/events/trade_event.hpp>
#include <slick-trader/events/quote_event.hpp>
#include <slick-trader/events/order_book_event.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick/queue.h>
#include <cstring>
#include <type_traits>

namespace slick::trader {

// Event serialization utilities for SlickQueue<uint8_t>
class EventSerializer {
public:
    // Publish event to queue
    template<typename T, typename populate_event_fn>
    static bool publish(slick::SlickQueue<uint8_t>& queue, populate_event_fn &&populate_event_func) {
        static_assert(std::is_trivially_copyable_v<T>, "Event must be trivially copyable");
        auto index = queue.reserve(sizeof(T));
        auto *e = reinterpret_cast<T*>(queue[index]);
        populate_event_func(*e);
        queue.publish(index, sizeof(T));
        return true;
    }

    // Publish event to queue
    template<typename T>
    static bool publish(slick::SlickQueue<uint8_t>& queue, const T& event) {
        static_assert(std::is_trivially_copyable_v<T>, "Event must be trivially copyable");
        auto index = queue.reserve(sizeof(T));
        auto *e = reinterpret_cast<T*>(queue[index]);
        *e = event;
        queue.publish(index, sizeof(T));
        return true;
    }

    // Consume event from queue
    template<typename T>
    static T* consume(slick::SlickQueue<uint8_t>& queue, uint64_t& cursor) {
        static_assert(std::is_trivially_copyable_v<T>, "Event must be trivially copyable");
        auto [data, size] = queue.read(cursor);
        if (data && size >= sizeof(T)) {
            return reinterpret_cast<T*>(data);
        }
        return nullptr;
    }
};

} // namespace slick::trader
