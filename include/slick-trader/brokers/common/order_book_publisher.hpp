#pragma once

#include <slick-trader/brokers/common/level_book.hpp>
#include <slick-trader/brokers/common/order_entry.hpp>
#include <slick-trader/brokers/common/shared_top_of_book.hpp>
#include <slick-trader/events/instrument_status_event.hpp>
#include <slick-trader/events/level2_update_event.hpp>
#include <slick-trader/events/mbo_update_event.hpp>
#include <slick-trader/events/md_subscription.hpp>
#include <slick-trader/utils/order_types.hpp>
#include <slick-trader/utils/types.hpp>
#include <slick/logger.hpp>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slick::trader {

// One order of an order-by-order feed changing, in the exchange's wire units.
struct BookOrderChange {
    enum class Action : uint8_t { ADD, CHANGE, REMOVE };

    uint64_t order_id = 0;
    uint64_t priority = 0;       // place in the queue at its price: lower trades first
    Action action = Action::ADD;
    Side side = Side::BUY;
    Side prev_side = Side::BUY;
    int64_t price = 0;
    int64_t prev_price = 0;
    int64_t qty = 0;             // lots shown; 0 when not on the book
    int64_t prev_qty = 0;
    uint64_t event_time = 0;     // the exchange's time, ns
};

// The exchange-neutral half of a market-by-order channel: from order changes in wire
// units to SlickTrader's price levels, order-by-order updates, trades and trading status.
//
// Every listed instrument's levels are kept, subscribed or not, so a later subscription
// is answered with a snapshot at once. At each end of an exchange event the changed
// levels of every instrument subscribed for levels are published, the order changes of
// every instrument subscribed order by order (SubscriptionType::MBO), and trades as they
// arrive. When the book is built or rebuilt after a gap each subscribed instrument gets
// a full snapshot of levels, orders, or both. Each instrument's best bid and offer
// go to its SharedTopOfBook, if it has one, for the order side.
//
// CRTP, so nothing per order goes through a virtual call. `Derived` supplies:
//   bool live() const                         - the book is in sequence
//   void for_each_resting_order(Key, F&& f)   - f(const BookOrderChange&) per resting order
// `Instrument` has `symbol` (std::string), `scale` (PriceScale) and `top`
// (SharedTopOfBook*, may be null). `Sink` takes, on the thread driving the channel:
//   publish_trade(const Instrument&, price_t, qty_t, Side aggressor, uint64_t time)
//   publish_levels(const Instrument&, const std::vector<PriceLevel>&, bool snapshot, uint64_t time, uint64_t seq)
//   publish_orders(const Instrument&, const std::vector<MboOrder>&, bool snapshot, uint64_t time, uint64_t seq)
//   publish_status(const Instrument&, TradingStatus, bool cancel_allowed, std::string_view reason, uint64_t time)
//
// Single-threaded.
template <typename Derived, typename Instrument, typename Sink, typename Key = int32_t>
class OrderBookPublisher {
public:
    enum class Listing : uint8_t {
        LISTED,    // on this channel
        PENDING,   // not known yet: the channel has not learned its instruments
        UNKNOWN,   // not on this channel
    };

    Listing subscribe(std::string_view symbol, SubscriptionType type) {
        if (!instruments_known_) {
            pending_.emplace_back(std::string(symbol), type);
            return Listing::PENDING;
        }
        auto it = by_symbol_.find(symbol);
        if (it == by_symbol_.end()) {
            return Listing::UNKNOWN;
        }
        apply_subscription(states_[it->second], type);
        return Listing::LISTED;
    }

    void unsubscribe(std::string_view symbol, SubscriptionType type) {
        std::erase_if(pending_, [&](const auto& p) { return p.first == symbol && p.second == type; });
        auto it = by_symbol_.find(symbol);
        if (it == by_symbol_.end()) {
            return;
        }
        auto& state = states_[it->second];
        subscribed(state, type) = false;
        if (!state.orders) {
            state.order_updates.clear();
        }
    }

    [[nodiscard]] const Instrument* find(std::string_view symbol) const {
        auto it = by_symbol_.find(symbol);
        return it == by_symbol_.end() ? nullptr : &states_.at(it->second).instrument;
    }

    // The channel knows which instruments it lists.
    [[nodiscard]] bool instruments_known() const noexcept { return instruments_known_; }

protected:
    struct InstrumentState {
        Instrument instrument;
        Key key{};
        LevelBook book;
        bool trades = false;
        bool levels = false;
        bool orders = false;    // subscribed order by order
        bool touched = false;   // changed in the current event
        TradingStatus status = TradingStatus::UNKNOWN;
        bool cancel_allowed = true;
        // What the instrument's SharedTopOfBook was last given.
        price_t top_bid = SharedTopOfBook::UNKNOWN;
        price_t top_ask = SharedTopOfBook::UNKNOWN;
        uint64_t last_time = 0;
        std::vector<MboOrder> order_updates;   // the current event's, for an order subscriber
    };

    OrderBookPublisher(Sink& sink, std::string who) : sink_(sink), who_(std::move(who)) {}

    // Lists an instrument. It replaces one already listed under `key`.
    InstrumentState& add_instrument(Key key, Instrument instrument) {
        by_symbol_.insert_or_assign(instrument.symbol, key);
        auto& state = states_[key];
        state.instrument = std::move(instrument);
        state.key = key;
        return state;
    }

    // Every instrument is listed: the subscriptions that waited for them are applied.
    void instruments_complete() {
        instruments_known_ = true;
        auto pending = std::move(pending_);
        pending_.clear();
        for (const auto& [symbol, type] : pending) {
            auto it = by_symbol_.find(symbol);
            if (it != by_symbol_.end()) {
                apply_subscription(states_[it->second], type);
            }
        }
    }

    [[nodiscard]] InstrumentState* state_of(Key key) {
        auto it = states_.find(key);
        return it == states_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] std::unordered_map<Key, InstrumentState>& states() noexcept { return states_; }
    [[nodiscard]] Sink& sink() noexcept { return sink_; }
    [[nodiscard]] const std::string& who() const noexcept { return who_; }

    // Room for `n` instruments up front.
    void reserve_instruments(size_t n) { states_.reserve(n); }

    // One order changed. Its instrument publishes at end_of_event(). With
    // `publish_order` false the change reaches the levels only: an order the exchange
    // re-listed under a new id, whose subscribers still know it by its old one.
    void apply_order(Key key, const BookOrderChange& u, bool publish_order = true) {
        auto it = states_.find(key);
        if (it == states_.end()) {
            return;
        }
        auto& state = it->second;
        state.book.apply(u.prev_side, u.prev_price, u.prev_qty, u.side, u.price, u.qty);
        state.last_time = u.event_time;
        // While the book is being rebuilt nothing is published order by order: the
        // snapshot that follows the rebuild carries it whole.
        if (publish_order && state.orders && derived().live()) {
            append_order(state.order_updates, state.instrument, u);
        }
        if (!state.touched) {
            state.touched = true;
            touched_.push_back(key);
        }
    }

    // A trade, published at once to the instrument's trade subscribers.
    void trade(Key key, int64_t wire_price, int64_t lots, Side aggressor, uint64_t event_time) {
        auto it = states_.find(key);
        if (it == states_.end() || !it->second.trades) {
            return;
        }
        const auto& instrument = it->second.instrument;
        price_t price;
        if (!instrument.scale.to_price(wire_price, price)) {
            LOG_WARN("{}: trade price {} of {} is off its scale - dropped", who_, wire_price, instrument.symbol);
            return;
        }
        sink_.publish_trade(instrument, price, lots_to_qty(lots), aggressor, event_time);
    }

    // The exchange event is complete: what it changed is published.
    void end_of_event() {
        const bool in_sequence = derived().live();
        for (const Key key : touched_) {
            auto& state = states_[key];
            state.touched = false;
            // While the book is being rebuilt its top is not the market's: it stays unknown
            // until book_rebuilt().
            if (in_sequence && state.instrument.top) {
                publish_top(state);
            }
            if (!state.order_updates.empty()) {
                sink_.publish_orders(state.instrument, state.order_updates, false, state.last_time, ++order_sequence_);
                state.order_updates.clear();
            }
            if (!state.levels) {
                state.book.flush([](const LevelBook::LevelChange&) {});
                continue;
            }
            levels_.clear();
            state.book.flush([&](const LevelBook::LevelChange& change) {
                bool ok = false;
                const auto level = to_level(state.instrument, change, ok);
                if (ok) {
                    levels_.push_back(level);
                }
            });
            if (!levels_.empty()) {
                sink_.publish_levels(state.instrument, levels_, false, state.last_time, ++level_sequence_);
            }
        }
        touched_.clear();
    }

    // Publishes the instrument's trading state if it changed.
    void set_status(InstrumentState& state, TradingStatus status, bool cancel_allowed, std::string_view reason,
                    uint64_t event_time) {
        if (status == state.status && cancel_allowed == state.cancel_allowed) {
            return;
        }
        state.status = status;
        state.cancel_allowed = cancel_allowed;
        sink_.publish_status(state.instrument, status, cancel_allowed, reason, event_time);
    }

    // The book fell out of sequence: what it showed may no longer be so, and nothing is
    // checked against it now.
    void book_gap() {
        for (auto& [key, state] : states_) {
            if (state.instrument.top) {
                set_top(state, SharedTopOfBook::UNKNOWN, SharedTopOfBook::UNKNOWN);
            }
        }
    }

    // Forgets every level, for a feed that will build its book again from nothing. The
    // tops read unknown until book_rebuilt().
    void clear_books() {
        for (auto& [key, state] : states_) {
            state.book = LevelBook{};
            state.touched = false;
            state.order_updates.clear();
        }
        touched_.clear();
        book_gap();
    }

    // The book is built, or rebuilt after a gap. Its own changes were published as they
    // were applied; a snapshot as well makes every subscriber's book exactly this one,
    // whatever it held.
    void book_rebuilt() {
        for (auto& [key, state] : states_) {
            if (state.instrument.top) {
                publish_top(state);
            }
            if (state.levels) {
                publish_snapshot(state);
            }
            if (state.orders) {
                publish_order_snapshot(state);
            }
        }
    }

private:
    struct SymbolHash {
        using is_transparent = void;
        size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };

    [[nodiscard]] Derived& derived() noexcept { return static_cast<Derived&>(*this); }
    [[nodiscard]] const Derived& derived() const noexcept { return static_cast<const Derived&>(*this); }

    // The flag of `state` a subscription of `type` sets.
    static bool& subscribed(InstrumentState& state, SubscriptionType type) noexcept {
        switch (type) {
        case SubscriptionType::TRADE:
            return state.trades;
        case SubscriptionType::MBO:
            return state.orders;
        default:
            return state.levels;   // quotes and both kinds of book are served from the levels
        }
    }

    void apply_subscription(InstrumentState& state, SubscriptionType type) {
        bool& flag = subscribed(state, type);
        if (flag) {
            return;
        }
        flag = true;
        // A new book subscriber starts from the whole book: at once if it is built,
        // otherwise when it is.
        if (derived().live()) {
            if (&flag == &state.levels) {
                publish_snapshot(state);
            } else if (&flag == &state.orders) {
                publish_order_snapshot(state);
            }
        }
    }

    static PriceLevel to_level(const Instrument& instrument, const LevelBook::LevelChange& change, bool& ok) {
        // PriceLevel is packed: to_price() gets a local, not a reference to its field.
        price_t price = 0;
        ok = instrument.scale.to_price(change.price, price);
        PriceLevel level;
        level.price = price;
        level.quantity = lots_to_qty(change.qty > 0 ? change.qty : 0);
        level.num_orders = change.qty > 0 ? change.orders : 0;
        level.side = change.side;
        return level;
    }

    // Appends `u` to `out` as a subscriber's book applies it.
    static void append_order(std::vector<MboOrder>& out, const Instrument& instrument, const BookOrderChange& u) {
        const MboOrder gone{u.order_id, 0, 0, 0, u.prev_side, MboAction::REMOVE};
        if (u.action == BookOrderChange::Action::REMOVE) {
            out.push_back(gone);
            return;
        }
        const bool changed = u.action == BookOrderChange::Action::CHANGE;
        price_t price;
        if (!instrument.scale.to_price(u.price, price)) {
            // A price the framework cannot express: the order cannot be shown, and one
            // shown at its old price must not stay there.
            if (changed) {
                out.push_back(gone);
            }
            return;
        }
        // An order book keeps an order on its side, so one that changed side leaves and
        // comes back.
        const bool same_side = u.prev_side == u.side;
        if (changed && !same_side) {
            out.push_back(gone);
        }
        out.push_back(MboOrder{u.order_id, u.priority, price, lots_to_qty(u.qty), u.side,
                               changed && same_side ? MboAction::MODIFY : MboAction::ADD});
    }

    void publish_snapshot(InstrumentState& state) {
        levels_.clear();
        state.book.for_each_level([&](const LevelBook::LevelChange& change) {
            bool ok = false;
            const auto level = to_level(state.instrument, change, ok);
            if (ok) {
                levels_.push_back(level);
            }
        });
        sink_.publish_levels(state.instrument, levels_, true, state.last_time, ++level_sequence_);
    }

    void publish_order_snapshot(InstrumentState& state) {
        orders_.clear();
        derived().for_each_resting_order(state.key, [&](const BookOrderChange& order) {
            append_order(orders_, state.instrument, order);
        });
        // Whatever this event has changed so far is in the snapshot already.
        state.order_updates.clear();
        sink_.publish_orders(state.instrument, orders_, true, state.last_time, ++order_sequence_);
    }

    // Stores what changed of the two; the order worker reads them.
    static void set_top(InstrumentState& state, price_t bid, price_t ask) noexcept {
        SharedTopOfBook& top = *state.instrument.top;
        if (bid != state.top_bid) {
            state.top_bid = bid;
            top.bid.store(bid, std::memory_order_release);
        }
        if (ask != state.top_ask) {
            state.top_ask = ask;
            top.ask.store(ask, std::memory_order_release);
        }
    }

    // Gives the instrument's SharedTopOfBook the book's best bid and offer.
    static void publish_top(InstrumentState& state) {
        const auto best = [&](Side side) {
            const int64_t wire = state.book.best(side);
            if (wire == LevelBook::NO_PRICE) {
                return SharedTopOfBook::EMPTY;
            }
            price_t price;
            // A price the framework cannot express is one nothing can be checked against.
            return state.instrument.scale.to_price(wire, price) ? price : SharedTopOfBook::UNKNOWN;
        };
        set_top(state, best(Side::BUY), best(Side::SELL));
    }

    Sink& sink_;
    std::string who_;
    bool instruments_known_ = false;
    std::unordered_map<Key, InstrumentState> states_;
    std::unordered_map<std::string, Key, SymbolHash, std::equal_to<>> by_symbol_;
    // Subscriptions made before the instruments were known.
    std::vector<std::pair<std::string, SubscriptionType>> pending_;
    std::vector<Key> touched_;
    std::vector<PriceLevel> levels_;   // reused for each publish
    std::vector<MboOrder> orders_;     // reused for each order snapshot
    uint64_t level_sequence_ = 0;
    uint64_t order_sequence_ = 0;
};

} // namespace slick::trader
