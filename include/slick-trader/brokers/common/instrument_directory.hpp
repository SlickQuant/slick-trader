#pragma once

#include <slick-trader/utils/string_hash.hpp>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace slick::trader {

// The instruments an order worker knows, by symbol and by the exchange's numeric id
// (`IdMember`, a pointer to the instrument's id field). Looked up by symbol without
// building a string. Entries never move once added, so a pointer to one stays good.
//
// Single-threaded: owned by the order worker.
template <typename Instrument, auto IdMember>
class InstrumentDirectory {
public:
    using Id = std::remove_cvref_t<decltype(std::declval<Instrument&>().*IdMember)>;

    // Adds `instrument`, or replaces the one with the same id.
    const Instrument& add(Instrument instrument) {
        const Id id = instrument.*IdMember;
        by_symbol_.insert_or_assign(instrument.symbol, id);
        auto& slot = by_id_[id];
        slot = std::move(instrument);
        return slot;
    }
    [[nodiscard]] const Instrument* by_symbol(std::string_view symbol) const {
        auto it = by_symbol_.find(symbol);
        return it == by_symbol_.end() ? nullptr : by_id(it->second);
    }
    [[nodiscard]] const Instrument* by_id(Id id) const {
        auto it = by_id_.find(id);
        return it == by_id_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] size_t size() const noexcept { return by_id_.size(); }

    template <typename F>
    void for_each(F&& f) const {
        for (const auto& [id, instrument] : by_id_) {
            f(instrument);
        }
    }

private:
    std::unordered_map<Id, Instrument> by_id_;   // node-based: addresses stay valid
    std::unordered_map<std::string, Id, StringHash, std::equal_to<>> by_symbol_;
};

} // namespace slick::trader
