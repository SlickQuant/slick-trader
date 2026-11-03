#include <slick-trader/brokers/common/order_entry.hpp>
#include <slick/logger.hpp>

namespace slick::trader {

const char* post_only_problem(PostOnlyPolicy policy, const SharedTopOfBook* top, Side side, OrderType type,
                              TimeInForce tif, price_t price, bool& warned, std::string_view who) {
    if (!rests(type, tif)) {
        return nullptr;
    }
    switch (policy) {
    case PostOnlyPolicy::REJECT:
        return "this exchange takes no post-only orders (post_only_policy is \"reject\")";
    case PostOnlyPolicy::ACCEPT:
        if (!warned) {
            warned = true;
            LOG_WARN("{}: \"post_only_policy\" is \"accept\" - post-only orders are sent as ordinary orders and can "
                     "take liquidity",
                     who);
        }
        return nullptr;
    case PostOnlyPolicy::NATIVE:
        return nullptr;   // the exchange itself will not match it on entry
    case PostOnlyPolicy::REJECT_IF_CROSSING:
        break;
    }
    if (!top) {
        return "post-only cannot be checked: the instrument has no market data";
    }
    // The one side the order could trade against, as the market data worker last built it.
    const price_t opposite = (side == Side::BUY ? top->ask : top->bid).load(std::memory_order_acquire);
    if (opposite == SharedTopOfBook::UNKNOWN) {
        return "post-only cannot be checked: the book is not in sequence";
    }
    if (opposite == SharedTopOfBook::EMPTY) {
        return nullptr;   // nothing to trade against
    }
    return (side == Side::BUY ? price >= opposite : price <= opposite) ? "post-only order would cross the book"
                                                                       : nullptr;
}

} // namespace slick::trader
