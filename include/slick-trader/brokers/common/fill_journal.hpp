#pragma once

#include <slick-trader/events/order_event.hpp>
#include <slick-trader/utils/mapped_file.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace slick::trader {

// Every fill an order connector publishes, written down first. A process that dies
// before its strategies have booked a fill can then hand it to them after the restart.
//
// Between an execution report arriving and its fill being booked lies the event queue,
// which does not outlive the process; the report's sequence number, which does, says it
// was received. Without the journal a crash in between loses the fill: the exchange is
// never asked for it again. With it, the connector replays the journal at start-up
// (OrderFillEvent::replayed), and the strategy manager drops what the strategies already
// had (OrderFillEvent::journal_seq, PositionStore::journaled()).
//
// A fill's journal number increases by one per fill, from the wall-clock time in
// nanoseconds at which the file was created: numbers never repeat, a journal that is
// deleted and created again included.
//
// Each fill is kept with the report it came from (Source). The one report a process died
// while handling is handed over again after the restart, and record() knows its fills
// for ones it already has.
//
// A ring of `capacity` fills, memory-mapped: recording one is a copy into memory, never a
// wait for the disk. The oldest are overwritten, so `capacity` is how many fills can still
// be on their way to the strategies when the process dies.
//
// Single-threaded: owned by the order worker.
class FillJournal {
public:
    static constexpr size_t DEFAULT_CAPACITY = 4096;

    // What a broker's journal file is: its magic, layout version, and the name its log
    // lines and errors go by.
    struct Format {
        char magic[8];
        uint32_t version;
        const char* name;
    };

    // The report a fill came from, and which of its fills it is. Reports are ordered by
    // epoch (a session's incarnation: whatever the exchange's sequence numbers restart
    // under), then sequence number, then the fill's place in the report.
    struct Source {
        uint64_t session = 0;
        uint64_t epoch = 0;
        uint32_t seq = 0;
        uint32_t index = 0;

        [[nodiscard]] bool after(const Source& other) const noexcept {
            if (epoch != other.epoch) {
                return epoch > other.epoch;
            }
            return seq != other.seq ? seq > other.seq : index > other.index;
        }
    };

    // Opens the journal at `path`, creating it with room for `capacity` fills and its
    // numbering starting at `first_seq`, the wall-clock time in nanoseconds. An existing
    // journal keeps its own capacity. Throws std::runtime_error for a file that is not
    // this format's journal, or was written with a different layout.
    FillJournal(const std::string& path, const Format& format, size_t capacity, uint64_t first_seq);

    // Journals `fill` from `source` and gives it its journal number. False, with nothing
    // journaled, for a fill that already is: its report handed over again.
    bool record(OrderFillEvent& fill, const Source& source);
    // Whether a fill from `source`, or from a report after it, is journaled already.
    [[nodiscard]] bool journaled(const Source& source) const noexcept {
        const auto it = last_.find(source.session);
        return it != last_.end() && !source.after(it->second);
    }

    // The fills held, oldest first.
    [[nodiscard]] std::vector<OrderFillEvent> fills() const;

    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] uint64_t next_seq() const noexcept { return next_seq_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    struct FileHeader;
    struct Entry;

    [[nodiscard]] FileHeader& header() noexcept;
    [[nodiscard]] Entry& entry(size_t index) noexcept;
    [[nodiscard]] const Entry& entry(size_t index) const noexcept;

    std::string path_;
    MappedFile file_;
    size_t capacity_ = 0;
    size_t size_ = 0;
    uint64_t next_seq_ = 0;
    // The last fill journaled from each session.
    std::unordered_map<uint64_t, Source> last_;
};

} // namespace slick::trader
