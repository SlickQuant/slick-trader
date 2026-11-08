#pragma once

#include <slick-trader/brokers/common/shared_top_of_book.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick-trader/utils/order_types.hpp>
#include <slick-trader/utils/types.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

// Order-entry rules that do not depend on any exchange's protocol: what a post-only
// order becomes where the exchange has no such instruction, whole-lot quantities, a
// request rate limit, per-contract fees, and the local reject of a new order.
namespace slick::trader {

// What becomes of an order the strategy marked post-only.
enum class PostOnlyPolicy : uint8_t {
    // Checked against the book before it is sent, and rejected locally if it would
    // trade, or if the book is not known well enough to tell. The check is against the
    // book as market data last showed it: an order can still trade against a price that
    // arrived in between.
    REJECT_IF_CROSSING,
    ACCEPT,    // sent as an ordinary order, which can take liquidity
    REJECT,    // rejected locally
    NATIVE,    // sent with the exchange's own instruction, where it has one
};

// Reads a `post_only_policy` config value. `native` is the exchange's own word for
// NATIVE ("passive", say), or empty when it has none. Throws for anything else.
inline PostOnlyPolicy parse_post_only_policy(const std::string& value, std::string_view native,
                                             std::string_view who) {
    if (value == "reject_if_crossing") {
        return PostOnlyPolicy::REJECT_IF_CROSSING;
    }
    if (value == "accept") {
        return PostOnlyPolicy::ACCEPT;
    }
    if (value == "reject") {
        return PostOnlyPolicy::REJECT;
    }
    if (!native.empty() && value == native) {
        return PostOnlyPolicy::NATIVE;
    }
    throw std::runtime_error(std::format(
        "{}: post_only_policy must be \"reject_if_crossing\", \"accept\" or \"reject\"{}, not \"{}\"", who,
        native.empty() ? std::string() : std::format(" or \"{}\"", native), value));
}

// Only an order that is to rest can be post-only. A market order, a stop waiting for its
// trigger and an order that is killed unless it fills are takers by nature.
inline bool rests(OrderType type, TimeInForce tif) noexcept {
    return type == OrderType::LIMIT && tif != TimeInForce::IOC && tif != TimeInForce::FOK;
}

// Whole lots, or 0 when `qty` is not a positive whole number of them.
inline uint32_t whole_lots(qty_t qty) noexcept {
    if (qty <= 0 || qty % QTY_SCALE != 0 || qty / QTY_SCALE > std::numeric_limits<uint32_t>::max()) {
        return 0;
    }
    return static_cast<uint32_t>(qty / QTY_SCALE);
}

inline qty_t lots_to_qty(int64_t lots) noexcept {
    return static_cast<qty_t>(lots) * QTY_SCALE;
}

// Why a post-only order at `price` cannot be sent under `policy`, or null when it can.
// `top` is the instrument's book as market data keeps it, null when there is none.
// `warned` makes ACCEPT's warning a one-off; `who` names the session in it.
const char* post_only_problem(PostOnlyPolicy policy, const SharedTopOfBook* top, Side side, OrderType type,
                              TimeInForce tif, price_t price, bool& warned, std::string_view who);

// Requests allowed per one-second window; 0 for no limit. One over is refused, not
// queued: a request held back would reach the exchange later than its sender thinks.
class RateLimiter {
public:
    explicit RateLimiter(uint32_t per_sec = 0) noexcept : per_sec_(per_sec) {}

    // False when this request would go over the limit.
    [[nodiscard]] bool allow(uint64_t now_ns) noexcept {
        if (per_sec_ == 0) {
            return true;
        }
        const uint64_t window = now_ns / 1'000'000'000ULL;
        if (window != window_) {
            window_ = window;
            count_ = 0;
        }
        return ++count_ <= per_sec_;
    }

    [[nodiscard]] uint32_t per_sec() const noexcept { return per_sec_; }

private:
    uint32_t per_sec_;
    uint64_t window_ = 0;
    uint32_t count_ = 0;
};

// Exchange and clearing fees per contract, by whatever names an instrument goes by
// (symbol, product), with a default for the rest. Looked up without building a string.
class FeeTable {
public:
    // `{ "default": 1.18, "ES": 1.18, "MES": 0.35 }`; empty or absent is no fees.
    static FeeTable from_json(const nlohmann::json& config) {
        FeeTable table;
        if (!config.is_object()) {
            return table;
        }
        for (const auto& [key, value] : config.items()) {
            if (key == "default") {
                table.default_fee_ = value.get<double>();
            } else {
                table.fees_[key] = value.get<double>();
            }
        }
        return table;
    }

    // The first of `names` the table lists, else the default.
    [[nodiscard]] double per_contract(std::initializer_list<std::string_view> names) const {
        for (const auto name : names) {
            auto it = fees_.find(name);
            if (it != fees_.end()) {
                return it->second;
            }
        }
        return default_fee_;
    }

    [[nodiscard]] double default_fee() const noexcept { return default_fee_; }
    void set(std::string_view name, double fee) { fees_.insert_or_assign(std::string(name), fee); }
    void set_default(double fee) noexcept { default_fee_ = fee; }

private:
    std::unordered_map<std::string, double, StringHash, std::equal_to<>> fees_;
    double default_fee_ = 0.0;
};

// The reject a new order gets when it is refused before it reaches the exchange.
inline OrderRejectEvent reject_of(const NewOrderEvent& request, BrokerId broker, std::string_view reason) {
    OrderRejectEvent event;
    event.broker_id = broker;
    event.set_instrument(request.instrument);
    event.set_client_order_id(request.client_order_id);
    event.side = request.side;
    event.price = request.price;
    event.stop_price = request.stop_price;
    event.quantity = request.quantity;
    event.set_reason(reason);
    return event;
}

} // namespace slick::trader
