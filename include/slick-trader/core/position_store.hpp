#pragma once

#include <slick-trader/core/position.hpp>
#include <slick-trader/events/event.hpp>
#include <slick-trader/utils/mapped_file.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace slick::trader {

// One open lot of a position: what FIFO accounting closes first.
struct PositionLot {
    qty_t quantity;
    price_t price;
    uint64_t timestamp;
};

// Positions kept across restarts.
//
// Every position - quantity, average price, realized PnL and fees, and the open lots FIFO
// accounting still has to close - lives in one fixed slot of a memory-mapped file
// (MappedFile), so recording a fill is a copy into memory and never waits for the disk. A
// position that was recorded is in the file even if the process is killed the next instant.
//
// A slot holds two copies of its position and a byte saying which is current. An update
// writes the other copy in full, then flips the byte: a process that dies mid-update leaves
// the position as it was before the fill, never half of each.
//
// It also keeps how far into each broker's fill journal the strategies have got (see
// OrderFillEvent::journal_seq), which is what lets a broker replay its journal after a
// crash without a fill being booked twice. A position records the journal number of the
// fill it was last booked from in the same copy as the position itself, so a fill booked
// is never one the store can think still to come.
//
// Single-threaded: owned by PositionTracker's writer thread.
class PositionStore {
public:
    // Open lots kept per position. A position built from more keeps the oldest
    // MAX_LOTS - 1 as they are and the rest as one lot at their average price.
    static constexpr std::size_t MAX_LOTS = 32;
    // Longest strategy id or instrument name a slot can be keyed by.
    static constexpr std::size_t MAX_KEY_LENGTH = 63;
    static constexpr std::size_t DEFAULT_CAPACITY = 1024;
    static constexpr uint32_t NO_SLOT = UINT32_MAX;
    // Brokers whose journal positions the file has room for.
    static constexpr std::size_t MAX_BROKERS = 16;
    static_assert(static_cast<std::size_t>(BrokerId::_BROKER_ID_COUNT_) <= MAX_BROKERS);

    // Opens the store at `path`, creating it with room for `capacity` positions. An
    // existing store keeps what it holds, and grows if `capacity` is larger than it was.
    // Throws std::runtime_error for a file that is not a position store, or one written
    // with a different layout.
    explicit PositionStore(const std::string& path, std::size_t capacity = DEFAULT_CAPACITY);

    using Visitor = std::function<void(uint32_t slot, std::string_view strategy_id, const Position& position,
                                       std::span<const PositionLot> lots)>;
    // Every position held, with the slot it is in.
    void for_each(const Visitor& visit) const;

    // Records the position as it stands. `slot` is where it was recorded before - what the
    // last call for it returned, or what for_each() reported - or NO_SLOT for one that is
    // new. Returns its slot, or NO_SLOT when it could not be recorded: the store is full,
    // or a key is too long. `journal_seq` is the journal number of the fill that made the
    // position what it is, from `broker`'s journal; 0 for a fill that has none, which
    // leaves the position's last one recorded.
    uint32_t save(uint32_t slot, std::string_view strategy_id, const Position& position,
                  std::span<const PositionLot> lots, BrokerId broker = BrokerId::UNKNOWN,
                  uint64_t journal_seq = 0);

    // How far into `broker`'s fill journal the strategies have got: the highest journal
    // number recorded by mark_journaled(), or by save() for a position booked from it.
    uint64_t journaled(BrokerId broker) const noexcept;
    // The strategies have had every fill of `broker`'s journal up to `journal_seq`.
    void mark_journaled(BrokerId broker, uint64_t journal_seq) noexcept;

    void erase_strategy(std::string_view strategy_id);
    void clear();

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return capacity_; }
    const std::string& path() const noexcept { return path_; }

    // Asks the operating system to write the file out now. Blocks on the disk.
    void flush() noexcept { file_.flush(); }

private:
    struct Slot;
    Slot& slot_at(std::size_t index) noexcept;
    const Slot& slot_at(std::size_t index) const noexcept;

    struct FileHeader;
    FileHeader& header() noexcept;

    std::string path_;
    MappedFile file_;
    uint64_t journaled_[MAX_BROKERS]{};
    std::size_t capacity_{0};
    std::size_t size_{0};
    std::size_t next_free_{0};   // no empty slot before this one
    bool full_logged_{false};
    bool lots_logged_{false};
};

} // namespace slick::trader
