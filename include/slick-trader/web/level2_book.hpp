#pragma once

#include <slick-trader/events/level2_update_event.hpp>
#include <slick-trader/utils/order_types.hpp>
#include <slick-trader/utils/types.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace slick::trader {

// One instrument's price levels as its LEVEL2_UPDATE events leave them, so a browser that
// subscribes after the broker's snapshot went by can still start from the book.
// Single-threaded: the WebServer's event loop owns it. Events are applied as they come, with
// no sequence check: one connector publishes an instrument's book, in order, so a snapshot
// is never late. Two publishing the same instrument would need one.
// It keeps, and hands over, every level: a broker sends a snapshot only as it subscribes, so a
// level left out would be missing from a subscriber's ladder once the price moved to it.
class Level2Book {
public:
    // An event with more levels than this is merged into the book in one pass.
    static constexpr uint16_t MERGE_FROM = 64;

    void apply(const Level2UpdateEvent& event) {
        // Only the first part of a snapshot carries is_snapshot: it clears, the rest add.
        if (event.is_snapshot) {
            bids_.clear();
            asks_.clear();
        }
        // A snapshot, or a part of one, is merged: a broker lists it best first, and set() would
        // put each level in at the front of its side, moving all the others, O(n^2) over a deep
        // book. A delta's few levels, most near the top, are set one by one.
        if (event.num_levels > MERGE_FROM) {
            merge(bids_, event, Side::BUY, std::less<price_t>{});
            merge(asks_, event, Side::SELL, std::greater<price_t>{});
        } else {
            for (uint16_t i = 0; i < event.num_levels; ++i) {
                // Copied out of the packed event before they bind to a reference.
                const price_t price = event.levels[i].price;
                const qty_t qty = event.levels[i].quantity;
                if (event.levels[i].side == Side::BUY) {
                    set(bids_, price, qty, std::less<price_t>{});
                } else {
                    set(asks_, price, qty, std::greater<price_t>{});
                }
            }
        }
        event_time_ns_ = event.event_time_ns;
        sequence_num_ = event.sequence_num;
        broker_ = event.broker_id;
    }

    // Calls `f(price, qty, side)` for every level, bids then asks, each from its best.
    template <typename F>
    void for_each(F&& f) const {
        for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
            f(it->price, it->qty, Side::BUY);
        }
        for (auto it = asks_.rbegin(); it != asks_.rend(); ++it) {
            f(it->price, it->qty, Side::SELL);
        }
    }

    size_t size() const noexcept { return bids_.size() + asks_.size(); }
    BrokerId broker() const noexcept { return broker_; }   // UNKNOWN until an event came
    uint64_t event_time_ns() const noexcept { return event_time_ns_; }
    uint64_t sequence_num() const noexcept { return sequence_num_; }

private:
    struct Level {
        price_t price;
        qty_t qty;
    };

    // Sets one level of a side kept worst first, so the best, where most changes land, moves
    // least. `worse(a, b)`: price a is worse than b, which is lower for bids, higher for asks.
    template <typename Worse>
    static void set(std::vector<Level>& side, price_t price, qty_t qty, Worse worse) {
        const auto it = std::lower_bound(side.begin(), side.end(), price,
                                         [&](const Level& level, price_t p) { return worse(level.price, p); });
        if (it != side.end() && it->price == price) {
            if (qty == 0) {
                side.erase(it);
            } else {
                it->qty = qty;
            }
        } else if (qty != 0) {
            side.insert(it, Level{price, qty});
        }
    }

    // Merges the levels of `event` on side `s` into `side`, as set() would one by one: a price
    // listed twice takes its last quantity, and a quantity of 0 removes its level.
    template <typename Worse>
    static void merge(std::vector<Level>& side, const Level2UpdateEvent& event, Side s, Worse worse) {
        std::vector<Level> changes;
        for (uint16_t i = 0; i < event.num_levels; ++i) {
            if (event.levels[i].side == s) {
                changes.push_back(Level{event.levels[i].price, event.levels[i].quantity});
            }
        }
        if (changes.empty()) {
            return;
        }
        std::stable_sort(changes.begin(), changes.end(),
                         [&](const Level& a, const Level& b) { return worse(a.price, b.price); });
        std::vector<Level> merged;
        merged.reserve(side.size() + changes.size());
        auto kept = side.begin();
        for (size_t i = 0; i < changes.size(); ++i) {
            if (i + 1 < changes.size() && changes[i + 1].price == changes[i].price) {
                continue;   // the price comes again, later in the event
            }
            const Level& change = changes[i];
            while (kept != side.end() && worse(kept->price, change.price)) {
                merged.push_back(*kept++);
            }
            if (kept != side.end() && kept->price == change.price) {
                ++kept;   // set again, or removed
            }
            if (change.qty != 0) {
                merged.push_back(change);
            }
        }
        merged.insert(merged.end(), kept, side.end());
        side.swap(merged);
    }

    std::vector<Level> bids_;   // ascending
    std::vector<Level> asks_;   // descending
    BrokerId broker_{BrokerId::UNKNOWN};
    uint64_t event_time_ns_{0};
    uint64_t sequence_num_{0};
};

// Appends `text` to `out` as a JSON string.
inline void append_json_string(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            constexpr char HEX[] = "0123456789abcdef";
            out += "\\u00";
            out += HEX[(c >> 4) & 0xf];
            out += HEX[c & 0xf];
        } else {
            out += c;
        }
    }
    out += '"';
}

// Appends `value` to `out` as a JSON number, in the fewest digits that read back as it.
// null when it is not finite, as JSON has no NaN.
template <typename T>
void append_json_number(std::string& out, T value) {
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(value)) {
            out += "null";
            return;
        }
    }
    char digits[32];
    out.append(digits, std::to_chars(std::begin(digits), std::end(digits), value).ptr);
}

// Writes the dashboard's level2 message into `out`, cleared first so one buffer and its
// capacity serve every message: {"type":"level2","instrument":...,"timestamp":<ms>,"seq_num":...,
// "is_snapshot":...,"levels":[[price,quantity,side],...]}. `for_each_level(emit)` calls
// emit(price, qty, side) for each level, as Level2Book::for_each() does.
template <typename ForEachLevel>
void write_level2_message(std::string& out, std::string_view instrument, uint64_t event_time_ns,
                          uint64_t sequence_num, bool is_snapshot, ForEachLevel&& for_each_level) {
    out.assign(R"({"type":"level2","instrument":)");
    append_json_string(out, instrument);
    out += R"(,"timestamp":)";
    append_json_number(out, event_time_ns / 1'000'000);
    out += R"(,"seq_num":)";
    append_json_number(out, sequence_num);
    out += is_snapshot ? R"(,"is_snapshot":true,"levels":[)" : R"(,"is_snapshot":false,"levels":[)";
    const size_t first = out.size();
    for_each_level([&](price_t price, qty_t qty, Side side) {
        out += out.size() == first ? "[" : ",[";
        append_json_number(out, from_price_t(price));
        out += ',';
        append_json_number(out, from_qty_t(qty));
        out += ',';
        append_json_number(out, static_cast<int>(side));
        out += ']';
    });
    out += "]}";
}

// Every instrument's level 2 book as it streams, and the clients watching each. A book that has
// streamed is kept, watched or not, for the life of the process: a client that subscribes late
// starts from it. One only asked for goes with its last watcher, so what clients ask for cannot
// grow this beyond what they watch.
// Single-threaded: the WebServer's event loop owns it. `Client` is only compared and hashed.
template <typename Client>
class Level2Streams {
public:
    using Clients = std::unordered_set<Client*>;

    // How many instruments one client may watch. A dashboard watches one, its ladder's.
    static constexpr size_t MAX_STREAMS_PER_CLIENT = 1024;

    // Applies `event` to its instrument's book and returns who watches it.
    const Clients& apply(const Level2UpdateEvent& event) {
        Stream& stream = stream_of(std::string_view(event.instrument));
        stream.streamed = true;
        ++stream.events;
        stream.book.apply(event);
        return stream.clients;
    }

    // Adds `client` to the watchers of `instrument` and returns the book to send it as a
    // snapshot, empty when nothing has streamed. Again on a repeat, as a client asks again when
    // it builds its ladder anew. nullptr when refused: an empty name or one too long for an
    // instrument, or a client watching MAX_STREAMS_PER_CLIENT already. Any other name is taken,
    // as a broker may not have streamed a book it subscribed yet; the cap bounds the rest.
    const Level2Book* subscribe(Client* client, std::string_view instrument) {
        if (instrument.empty() || instrument.size() >= MAX_INSTRUMENT_LENGTH) {
            return nullptr;
        }
        auto it = streams_.find(instrument);
        if (it != streams_.end() && it->second.clients.contains(client)) {
            return &it->second.book;
        }
        auto& watched = watching_[client];
        if (watched.size() >= MAX_STREAMS_PER_CLIENT) {
            return nullptr;
        }
        Stream& stream = it != streams_.end() ? it->second : stream_of(instrument);
        stream.clients.insert(client);
        watched.insert(&stream);
        return &stream.book;
    }

    // Returns whether `client` was watching `instrument`.
    bool unsubscribe(Client* client, std::string_view instrument) {
        const auto it = streams_.find(instrument);
        if (it == streams_.end() || it->second.clients.erase(client) == 0) {
            return false;
        }
        watching_[client].erase(&it->second);
        drop_if_unused(it->second);
        return true;
    }

    // Drops `client` from the streams it watches, and from those alone. `unwatched(instrument)`
    // is called for each it was the last watcher of.
    template <typename F>
    void remove(Client* client, F&& unwatched) {
        const auto it = watching_.find(client);
        if (it == watching_.end()) {
            return;
        }
        for (Stream* stream : it->second) {
            stream->clients.erase(client);
            if (stream->clients.empty()) {
                unwatched(stream->instrument);   // before drop_if_unused() takes the name with it
            }
            drop_if_unused(*stream);
        }
        watching_.erase(it);
    }
    void remove(Client* client) {
        remove(client, [](std::string_view) {});
    }

    // How many clients watch `instrument`.
    size_t watchers(std::string_view instrument) const {
        const auto it = streams_.find(instrument);
        return it != streams_.end() ? it->second.clients.size() : 0;
    }

    // Its broker was told to stop streaming `instrument`: from here, the book goes stale once its
    // events stop. renew(), as the broker is asked for it again, empties a book no event came for
    // since, so a subscriber starts from the snapshot the broker sends, not from what it left. A
    // book whose events kept coming, from a connector that streams on regardless, is current and
    // kept: emptied, it would be built from increments alone.
    void release(std::string_view instrument) {
        if (const auto it = streams_.find(instrument); it != streams_.end()) {
            it->second.released = true;
            it->second.events_at_release = it->second.events;
        }
    }
    void renew(std::string_view instrument) {
        const auto it = streams_.find(instrument);
        if (it == streams_.end() || !it->second.released) {
            return;
        }
        Stream& stream = it->second;
        stream.released = false;
        if (stream.events == stream.events_at_release) {
            stream.book = Level2Book{};
        }
    }

    // How many streams are kept.
    size_t size() const noexcept { return streams_.size(); }

private:
    struct Stream {
        Clients clients;
        Level2Book book;
        std::string_view instrument;   // its key in streams_
        bool streamed = false;         // an event came for it
        uint64_t events = 0;           // how many came
        bool released = false;         // its broker was told to stop, as release() says
        uint64_t events_at_release = 0;
    };

    Stream& stream_of(std::string_view instrument) {
        auto it = streams_.find(instrument);
        if (it == streams_.end()) {
            it = streams_.try_emplace(std::string(instrument)).first;
            it->second.instrument = it->first;
        }
        return it->second;
    }

    void drop_if_unused(Stream& stream) {
        if (!stream.streamed && stream.clients.empty()) {
            streams_.erase(streams_.find(stream.instrument));
        }
    }

    // A map's elements stay put, so the pointers in watching_ hold until their stream is erased,
    // which it is only once nobody watches it.
    std::unordered_map<std::string, Stream, StringHash, std::equal_to<>> streams_;
    std::unordered_map<Client*, std::unordered_set<Stream*>> watching_;
};

} // namespace slick::trader
