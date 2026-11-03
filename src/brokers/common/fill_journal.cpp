#include <slick-trader/brokers/common/fill_journal.hpp>
#include <slick/logger.hpp>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <stdexcept>

namespace slick::trader {

// Every field at its natural alignment with no padding between them, which the
// static_asserts pin: the layout is the file format.
struct FillJournal::FileHeader {
    char magic[8];
    uint32_t version;
    uint32_t entry_size;
    uint64_t capacity;
    uint64_t first_seq;
    // Past every number handed out. Written before an entry is overwritten: one cleared
    // by a process that died mid-write may have been the only entry left.
    uint64_t next_seq;
    uint8_t reserved[24];
};

struct FillJournal::Entry {
    // Written last, and cleared first when the entry is overwritten: an entry is never
    // read half written.
    uint64_t journal_seq;
    Source source;
    OrderFillEvent fill;
};

static_assert(sizeof(FillJournal::Source) == 24);

FillJournal::FillJournal(const std::string& path, const Format& format, size_t capacity, uint64_t first_seq)
    : path_(path)
    , file_(path, sizeof(FileHeader) + std::max<size_t>(capacity, 1) * sizeof(Entry))
{
    static_assert(sizeof(FileHeader) == 64 && sizeof(FileHeader) % alignof(Entry) == 0);
    static_assert(sizeof(Entry) % alignof(Entry) == 0);

    auto* header = reinterpret_cast<FileHeader*>(file_.data());
    const bool blank = std::all_of(file_.data(), file_.data() + sizeof(FileHeader), [](uint8_t b) { return b == 0; });
    if (blank) {
        FileHeader fresh{};
        fresh.version = format.version;
        fresh.entry_size = static_cast<uint32_t>(sizeof(Entry));
        fresh.capacity = std::max<size_t>(capacity, 1);
        fresh.first_seq = std::max<uint64_t>(first_seq, 1);
        std::memcpy(fresh.magic, format.magic, sizeof(fresh.magic));
        std::memcpy(header, &fresh, sizeof(FileHeader));
    } else if (std::memcmp(header->magic, format.magic, sizeof(header->magic)) != 0) {
        throw std::runtime_error(std::string(format.name) + ": " + path + " is not a fill journal");
    } else if (header->version != format.version || header->entry_size != sizeof(Entry) ||
               sizeof(FileHeader) + header->capacity * sizeof(Entry) > file_.size()) {
        throw std::runtime_error(std::string(format.name) + ": " + path +
                                 " was written with a different layout (version " +
                                 std::to_string(header->version) + ")");
    }
    // A ring keeps the capacity it was made with: its entries' places depend on it.
    capacity_ = static_cast<size_t>(header->capacity);
    if (capacity_ != capacity) {
        LOG_WARN("{}: {} keeps the capacity it was created with, {} fills, not {}", format.name, path_, capacity_,
                 capacity);
    }

    uint64_t last_seq = 0;
    std::unordered_map<uint64_t, uint64_t> newest;   // session -> journal number of its last fill
    for (size_t i = 0; i < capacity_; ++i) {
        const Entry& e = entry(i);
        if (e.journal_seq == 0) {
            continue;
        }
        ++size_;
        last_seq = std::max(last_seq, e.journal_seq);
        auto [it, added] = newest.try_emplace(e.source.session, e.journal_seq);
        if (added || e.journal_seq > it->second) {
            it->second = e.journal_seq;
            last_[e.source.session] = e.source;
        }
    }
    next_seq_ = std::max({last_seq + 1, header->first_seq, header->next_seq});
    LOG_INFO("{}: {} holds {} fill(s), room for {}", format.name, path_, size_, capacity_);
}

FillJournal::FileHeader& FillJournal::header() noexcept {
    return *reinterpret_cast<FileHeader*>(file_.data());
}

FillJournal::Entry& FillJournal::entry(size_t index) noexcept {
    return *reinterpret_cast<Entry*>(file_.data() + sizeof(FileHeader) + index * sizeof(Entry));
}

const FillJournal::Entry& FillJournal::entry(size_t index) const noexcept {
    return *reinterpret_cast<const Entry*>(file_.data() + sizeof(FileHeader) + index * sizeof(Entry));
}

bool FillJournal::record(OrderFillEvent& fill, const Source& source) {
    auto [last, first] = last_.try_emplace(source.session, source);
    if (!first) {
        if (!source.after(last->second)) {
            return false;   // its report was handed over again: this fill is journaled
        }
        last->second = source;
    }
    fill.journal_seq = next_seq_++;
    Entry& e = entry(fill.journal_seq % capacity_);
    if (e.journal_seq == 0) {
        ++size_;
    }
    // The number marked used, then the entry cleared, filled and numbered; the fences keep
    // the compiler from reordering the stores, which is all a process dying between two
    // of them could see.
    header().next_seq = next_seq_;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    e.journal_seq = 0;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    e.source = source;
    std::memcpy(static_cast<void*>(&e.fill), &fill, sizeof(OrderFillEvent));
    std::atomic_signal_fence(std::memory_order_seq_cst);
    e.journal_seq = fill.journal_seq;
    return true;
}

std::vector<OrderFillEvent> FillJournal::fills() const {
    std::vector<const Entry*> held;
    held.reserve(size_);
    for (size_t i = 0; i < capacity_; ++i) {
        if (entry(i).journal_seq != 0) {
            held.push_back(&entry(i));
        }
    }
    std::sort(held.begin(), held.end(), [](const Entry* a, const Entry* b) { return a->journal_seq < b->journal_seq; });
    std::vector<OrderFillEvent> out;
    out.reserve(held.size());
    for (const Entry* e : held) {
        out.push_back(e->fill);
    }
    return out;
}

} // namespace slick::trader
