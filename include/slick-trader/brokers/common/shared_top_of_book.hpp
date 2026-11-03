#pragma once

#include <slick-trader/utils/types.hpp>
#include <atomic>
#include <cstddef>
#include <deque>
#include <limits>

namespace slick::trader {

// One instrument's best bid and offer, as a broker's market data worker last built
// them, for its order worker to read. A post-only order is checked against it when
// the exchange has no post-only instruction of its own.
//
// Each side is one atomic: the market data worker stores, the order worker loads,
// neither waits. An order looks at the one side it could cross, so the two need not be
// consistent with each other. Its own cache line, so the market data worker writing
// one instrument does not take the line the order worker is reading another from.
struct alignas(64) SharedTopOfBook {
    // The book is not in sequence: being built, or rebuilt after a gap.
    static constexpr price_t UNKNOWN = std::numeric_limits<price_t>::min();
    // The side has no orders.
    static constexpr price_t EMPTY = NULL_PRICE;

    std::atomic<price_t> bid{UNKNOWN};
    std::atomic<price_t> ask{UNKNOWN};
};

// Every instrument's SharedTopOfBook. An entry never moves once allocated, so a pointer
// handed to another thread stays good for as long as the store lives. allocate() is for
// one thread alone: whichever learns the instruments.
class SharedTopOfBookStore {
public:
    SharedTopOfBook* allocate() { return &tops_.emplace_back(); }
    [[nodiscard]] size_t size() const noexcept { return tops_.size(); }

private:
    std::deque<SharedTopOfBook> tops_;
};

} // namespace slick::trader
