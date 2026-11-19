#pragma once

#include <slick-trader/utils/order_types.hpp>
#include <slick-trader/utils/types.hpp>
#include <cstddef>
#include <deque>
#include <string>
#include <unordered_map>

namespace slick::trader {

// What the Coinbase connector last told the engine of each order, so each change goes out once.
// Coinbase says most things twice: the REST response and the user channel both ack a new order,
// both report a cancel, and the channel can repeat an update. The channel knows no base size,
// and a pending order no qty: a qty, price or stop of 0 (or NULL_PRICE) keeps the one known.
// Used from the connector's worker thread only.
class CoinbaseOrderStates {
public:
    // How many ended orders are remembered, so their late repeats drop. The oldest go first.
    static constexpr std::size_t ENDED_KEPT = 1024;

    // Whether an ack with these tells something new of the order, and records it. A price, stop or
    // qty the update doesn't know comes back as the known one. An ended order's acks are stale, and
    // so is a pending one after the order opened: the REST response can come after the channel's.
    bool ack(const std::string& order_id, OrderStatus status, price_t& price, price_t& stop_price, qty_t& qty) {
        auto [it, added] = orders_.try_emplace(order_id, State{status, price, stop_price, qty, false});
        if (added) {
            return true;
        }
        auto& state = it->second;
        if (state.ended || (status == OrderStatus::PENDING_NEW && state.status != OrderStatus::PENDING_NEW)) {
            return false;
        }
        keep_known(price, state.price);
        keep_known(stop_price, state.stop_price);
        if (qty == 0) {
            qty = state.qty;
        }
        const State next{status, price, stop_price, qty, false};
        if (next == state) {
            return false;
        }
        state = next;
        return true;
    }

    // Whether the order was said to be gone, or filled.
    bool ended(const std::string& order_id) const {
        const auto it = orders_.find(order_id);
        return it != orders_.end() && it->second.ended;
    }

    // Whether this is the first word that the order is gone, or filled.
    bool end(const std::string& order_id) {
        auto [it, added] = orders_.try_emplace(order_id, State{});
        if (it->second.ended) {
            return false;
        }
        it->second.ended = true;
        ended_.push_back(order_id);
        if (ended_.size() > ENDED_KEPT) {
            orders_.erase(ended_.front());
            ended_.pop_front();
        }
        return true;
    }

    std::size_t size() const { return orders_.size(); }

private:
    struct State {
        OrderStatus status{OrderStatus::PENDING_NEW};
        price_t price{0};
        price_t stop_price{0};
        qty_t qty{0};
        bool ended{false};

        bool operator==(const State&) const = default;
    };

    static void keep_known(price_t& price, price_t known) {
        if (price == 0 || price == NULL_PRICE) {
            price = known;
        }
    }

    std::unordered_map<std::string, State> orders_;   // keyed by order_id
    std::deque<std::string> ended_;                   // oldest first
};

} // namespace slick::trader
