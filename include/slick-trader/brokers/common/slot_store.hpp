#pragma once

#include <slick-trader/utils/mapped_file.hpp>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace slick::trader {

// What a broker's state file is: its magic, layout version, and the name its log lines
// and errors go by.
struct SlotStoreFormat {
    char magic[8];
    uint32_t version;
    const char* name;
};

namespace detail {

// The untyped half of SlotStore: opens, checks and lays out the file.
//
// The header is 128 bytes: magic@0, version@8, slot size@12, identity@16 (16 bytes),
// then the broker's session state@32 (96 bytes). The slots follow it.
class SlotFile {
public:
    static constexpr size_t HEADER_SIZE = 128;
    static constexpr size_t IDENTITY_SIZE = 16;
    static constexpr size_t STATE_OFFSET = 32;
    static constexpr size_t STATE_SIZE = HEADER_SIZE - STATE_OFFSET;

    // `fresh_state` is copied into a file that is new. Throws std::runtime_error for a
    // file of another format, layout or identity.
    SlotFile(const std::string& path, const SlotStoreFormat& format, const std::array<char, IDENTITY_SIZE>& identity,
             std::string_view identity_text, size_t slot_size, size_t max_slots, const void* fresh_state,
             size_t state_size);

    [[nodiscard]] uint8_t* state() noexcept { return file_.data() + STATE_OFFSET; }
    [[nodiscard]] uint8_t* slot(uint32_t index) noexcept {
        return file_.data() + HEADER_SIZE + static_cast<size_t>(index) * slot_size_;
    }
    [[nodiscard]] const uint8_t* slot(uint32_t index) const noexcept {
        return file_.data() + HEADER_SIZE + static_cast<size_t>(index) * slot_size_;
    }
    [[nodiscard]] uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool created() const noexcept { return created_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] const char* name() const noexcept { return name_; }

private:
    std::string path_;
    const char* name_;
    MappedFile file_;
    size_t slot_size_;
    uint32_t capacity_ = 0;
    bool created_ = false;
};

} // namespace detail

// A session's state kept across restarts: the broker's own `State` (its sequence numbers,
// say) and a fixed number of `Record` slots (its working orders).
//
// The file is memory-mapped: saving a record or a sequence number is a store to memory,
// never a wait for the disk, and what was stored is in the file even if the process is
// killed the next instant. A slot holds two copies of its record and a byte saying which
// is current: an update writes the other copy whole, then flips the byte, so a process
// that dies mid-update leaves the record as it was.
//
// `identity` (16 bytes) names whose state this is: opening another session's file
// throws, since its sequence numbers and orders would be worse than none.
//
// Single-threaded: owned by the order worker.
template <typename State, typename Record>
class SlotStore {
    static_assert(std::is_trivially_copyable_v<State> && std::is_trivially_copyable_v<Record>);
    static_assert(sizeof(State) <= detail::SlotFile::STATE_SIZE, "State must fit the header's 96 bytes");
    static_assert(alignof(State) <= 8 && alignof(Record) <= 8);

public:
    static constexpr uint32_t NO_SLOT = UINT32_MAX;
    static constexpr size_t DEFAULT_MAX_RECORDS = 4096;
    using Identity = std::array<char, detail::SlotFile::IDENTITY_SIZE>;

    SlotStore(const std::string& path, const SlotStoreFormat& format, const Identity& identity,
              std::string_view identity_text, size_t max_records = DEFAULT_MAX_RECORDS)
        : file_(path, format, identity, identity_text, sizeof(Slot), max_records, &fresh_state_, sizeof(State))
    {
        free_.reserve(file_.capacity());
        for (uint32_t index = file_.capacity(); index-- > 0;) {
            Slot& s = slot(index);
            if (s.state != SLOT_RECORD_0 && s.state != SLOT_RECORD_1) {
                s.state = SLOT_FREE;
                free_.push_back(index);
            }
        }
    }

    // The session's state, in the file.
    [[nodiscard]] State& state() noexcept { return *reinterpret_cast<State*>(file_.state()); }

    // Calls `f(slot, const Record&)` for each slot in use.
    template <typename F>
    void for_each(F&& f) const {
        for (uint32_t index = 0; index < file_.capacity(); ++index) {
            const Slot& s = slot(index);
            if (s.state == SLOT_RECORD_0 || s.state == SLOT_RECORD_1) {
                f(index, s.records[s.state - SLOT_RECORD_0]);
            }
        }
    }

    // Records `record` in `slot`, or with NO_SLOT in a free one. Returns the slot, or
    // NO_SLOT when the store is full: the record is then not kept across a restart.
    uint32_t save(uint32_t index, const Record& record) {
        if (index == NO_SLOT) {
            if (free_.empty()) {
                report_full();
                return NO_SLOT;
            }
            index = free_.back();
            free_.pop_back();
        } else if (index >= file_.capacity()) {
            return NO_SLOT;
        }
        // The copy that is not current is written whole, then made current by one byte:
        // a process that dies in between leaves the record as it was, or a free slot free.
        Slot& s = slot(index);
        const uint8_t next = s.state == SLOT_RECORD_0 ? SLOT_RECORD_1 : SLOT_RECORD_0;
        std::memcpy(static_cast<void*>(&s.records[next - SLOT_RECORD_0]), &record, sizeof(Record));
        std::atomic_signal_fence(std::memory_order_seq_cst);   // the compiler keeps that order too
        s.state = next;
        return index;
    }

    void release(uint32_t index) noexcept {
        if (index < file_.capacity() && slot(index).state != SLOT_FREE) {
            slot(index).state = SLOT_FREE;
            free_.push_back(index);
        }
    }

    [[nodiscard]] size_t capacity() const noexcept { return file_.capacity(); }
    [[nodiscard]] size_t size() const noexcept { return file_.capacity() - free_.size(); }
    [[nodiscard]] const std::string& path() const noexcept { return file_.path(); }
    // True when the file was created by this run: there was nothing to resume.
    [[nodiscard]] bool created() const noexcept { return file_.created(); }

private:
    static constexpr uint8_t SLOT_FREE = 0;
    static constexpr uint8_t SLOT_RECORD_0 = 1;
    static constexpr uint8_t SLOT_RECORD_1 = 2;

    struct Slot {
        uint8_t state;
        uint8_t reserved[7];
        Record records[2];
    };
    static_assert(offsetof(Slot, records) == 8);

    [[nodiscard]] Slot& slot(uint32_t index) noexcept { return *reinterpret_cast<Slot*>(file_.slot(index)); }
    [[nodiscard]] const Slot& slot(uint32_t index) const noexcept {
        return *reinterpret_cast<const Slot*>(file_.slot(index));
    }

    void report_full();

    inline static const State fresh_state_{};
    detail::SlotFile file_;
    std::vector<uint32_t> free_;
    bool full_logged_ = false;
};

namespace detail {
void report_slot_store_full(const char* name, const std::string& path, size_t capacity);
}

template <typename State, typename Record>
void SlotStore<State, Record>::report_full() {
    if (!full_logged_) {
        full_logged_ = true;
        detail::report_slot_store_full(file_.name(), file_.path(), file_.capacity());
    }
}

} // namespace slick::trader
