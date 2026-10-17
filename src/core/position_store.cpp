#include <slick-trader/core/position_store.hpp>
#include <slick/logger.hpp>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <stdexcept>

namespace slick::trader {

namespace {

constexpr char MAGIC[8] = {'S', 'L', 'K', 'P', 'O', 'S', 'N', '1'};
constexpr uint32_t VERSION = 1;

// Slot::state. The two record states name the record that is current.
constexpr uint8_t SLOT_EMPTY = 0;
constexpr uint8_t SLOT_RECORD_0 = 1;
constexpr uint8_t SLOT_RECORD_1 = 2;

// Every field below sits at its natural alignment with no padding between them, which the
// static_asserts pin: the layout is the file format.
struct Record {
    qty_t quantity;
    price_t avg_price;
    price_t realized_pnl;
    price_t realized_fees;
    price_t open_fees;
    double multiplier;
    uint64_t last_update_time;
    uint32_t lot_count;
    uint8_t journal_broker;   // a BrokerId: whose journal journal_seq is in
    uint8_t reserved[3];
    uint64_t journal_seq;     // the fill the position was last booked from; 0 for none
    PositionLot lots[PositionStore::MAX_LOTS];
};

static_assert(sizeof(PositionLot) == 24);
static_assert(sizeof(Record) == 72 + PositionStore::MAX_LOTS * sizeof(PositionLot));

bool store_key(char* field, std::string_view key) noexcept {
    if (key.size() > PositionStore::MAX_KEY_LENGTH) {
        return false;
    }
    std::memset(field, 0, PositionStore::MAX_KEY_LENGTH + 1);
    std::memcpy(field, key.data(), key.size());
    return true;
}

std::string_view load_key(const char* field) noexcept {
    return {field, strnlen(field, PositionStore::MAX_KEY_LENGTH)};
}

}   // namespace

struct PositionStore::FileHeader {
    char magic[8];
    uint32_t version;
    uint32_t slot_size;
    uint32_t max_lots;
    uint8_t reserved[44];
    uint64_t journaled[MAX_BROKERS];   // by BrokerId: see mark_journaled()
};

struct PositionStore::Slot {
    uint8_t state;
    uint8_t reserved[7];
    char strategy_id[MAX_KEY_LENGTH + 1];
    char instrument[MAX_KEY_LENGTH + 1];
    Record records[2];
};

PositionStore::FileHeader& PositionStore::header() noexcept {
    return *reinterpret_cast<FileHeader*>(file_.data());
}

PositionStore::PositionStore(const std::string& path, std::size_t capacity)
    : path_(path)
    , file_(path, sizeof(FileHeader) + std::max<std::size_t>(capacity, 1) * sizeof(Slot))
{
    static_assert(sizeof(FileHeader) == 64 + 8 * MAX_BROKERS);
    static_assert(sizeof(Slot) == 8 + 2 * (MAX_KEY_LENGTH + 1) + 2 * sizeof(Record));
    static_assert(sizeof(Slot) % alignof(Slot) == 0 && sizeof(FileHeader) % alignof(Slot) == 0);

    auto* header = reinterpret_cast<FileHeader*>(file_.data());
    const FileHeader blank{};
    if (std::memcmp(header, &blank, sizeof(FileHeader)) == 0) {
        // New, or never written: this is the first run with it.
        FileHeader fresh{};
        std::memcpy(fresh.magic, MAGIC, sizeof(MAGIC));
        fresh.version = VERSION;
        fresh.slot_size = static_cast<uint32_t>(sizeof(Slot));
        fresh.max_lots = static_cast<uint32_t>(MAX_LOTS);
        std::memcpy(header, &fresh, sizeof(FileHeader));
    } else if (std::memcmp(header->magic, MAGIC, sizeof(MAGIC)) != 0) {
        throw std::runtime_error("PositionStore: " + path + " is not a position store");
    } else if (header->version != VERSION || header->slot_size != sizeof(Slot) || header->max_lots != MAX_LOTS) {
        throw std::runtime_error("PositionStore: " + path + " was written with a different layout (version " +
                                 std::to_string(header->version) + ")");
    }

    capacity_ = (file_.size() - sizeof(FileHeader)) / sizeof(Slot);
    next_free_ = capacity_;
    std::copy_n(header->journaled, MAX_BROKERS, journaled_);
    for (std::size_t i = 0; i < capacity_; ++i) {
        const Slot& slot = slot_at(i);
        if (slot.state == SLOT_RECORD_0 || slot.state == SLOT_RECORD_1) {
            ++size_;
            // A fill booked after the strategies' place in the journal was last marked:
            // the process died in between, and the position says how far they had got.
            const Record& record = slot.records[slot.state - SLOT_RECORD_0];
            if (record.journal_broker < MAX_BROKERS) {
                journaled_[record.journal_broker] = std::max(journaled_[record.journal_broker], record.journal_seq);
            }
        } else if (slot.state != SLOT_EMPTY) {
            ++size_;
        } else if (next_free_ == capacity_) {
            next_free_ = i;
        }
    }
    // What the positions said is the mark from now on: it stays when they go.
    std::copy_n(journaled_, MAX_BROKERS, header->journaled);
    LOG_INFO("PositionStore: {} holds {} position(s), room for {}", path_, size_, capacity_);
}

uint64_t PositionStore::journaled(BrokerId broker) const noexcept {
    const auto index = static_cast<std::size_t>(broker);
    return index < MAX_BROKERS ? journaled_[index] : 0;
}

void PositionStore::mark_journaled(BrokerId broker, uint64_t journal_seq) noexcept {
    const auto index = static_cast<std::size_t>(broker);
    if (index < MAX_BROKERS && journal_seq > journaled_[index]) {
        journaled_[index] = journal_seq;
        header().journaled[index] = journal_seq;
    }
}

PositionStore::Slot& PositionStore::slot_at(std::size_t index) noexcept {
    return *reinterpret_cast<Slot*>(file_.data() + sizeof(FileHeader) + index * sizeof(Slot));
}

const PositionStore::Slot& PositionStore::slot_at(std::size_t index) const noexcept {
    return *reinterpret_cast<const Slot*>(file_.data() + sizeof(FileHeader) + index * sizeof(Slot));
}

void PositionStore::for_each(const Visitor& visit) const {
    for (std::size_t i = 0; i < capacity_; ++i) {
        const Slot& slot = slot_at(i);
        if (slot.state != SLOT_RECORD_0 && slot.state != SLOT_RECORD_1) {
            continue;
        }
        // Copied out, so a caller is free to save() from the visit.
        const Record record = slot.records[slot.state - SLOT_RECORD_0];
        Position position(load_key(slot.instrument));
        position.quantity = record.quantity;
        position.avg_price = record.avg_price;
        position.realized_pnl = record.realized_pnl;
        position.realized_fees = record.realized_fees;
        position.open_fees = record.open_fees;
        position.multiplier = record.multiplier;
        position.last_update_time = record.last_update_time;
        const std::size_t lot_count = std::min<std::size_t>(record.lot_count, MAX_LOTS);
        visit(static_cast<uint32_t>(i), load_key(slot.strategy_id), position, {record.lots, lot_count});
    }
}

uint32_t PositionStore::save(uint32_t slot_index, std::string_view strategy_id, const Position& position,
                             std::span<const PositionLot> lots, BrokerId broker, uint64_t journal_seq) {
    if (slot_index == NO_SLOT) {
        while (next_free_ < capacity_ && slot_at(next_free_).state != SLOT_EMPTY) {
            ++next_free_;
        }
        if (next_free_ >= capacity_) {
            if (!full_logged_) {
                full_logged_ = true;
                LOG_ERROR("PositionStore: {} is full ({} positions) - {} {} and any further new position are NOT "
                          "kept across a restart; raise position_store.capacity",
                          path_, capacity_, strategy_id, position.instrument);
            }
            return NO_SLOT;
        }
        Slot& fresh = slot_at(next_free_);
        if (!store_key(fresh.strategy_id, strategy_id) || !store_key(fresh.instrument, position.instrument)) {
            LOG_ERROR("PositionStore: {} {} cannot be kept across a restart - a name is longer than {} characters",
                      strategy_id, position.instrument, MAX_KEY_LENGTH);
            return NO_SLOT;
        }
        slot_index = static_cast<uint32_t>(next_free_++);
        ++size_;
    }
    if (slot_index >= capacity_) {
        return NO_SLOT;
    }

    Slot& slot = slot_at(slot_index);
    // The record that is not current is written whole, then made current by one byte.
    const uint8_t next_state = slot.state == SLOT_RECORD_0 ? SLOT_RECORD_1 : SLOT_RECORD_0;
    Record record{};
    record.quantity = position.quantity;
    record.avg_price = position.avg_price;
    record.realized_pnl = position.realized_pnl;
    record.realized_fees = position.realized_fees;
    record.open_fees = position.open_fees;
    record.multiplier = position.multiplier;
    record.last_update_time = position.last_update_time;
    if (journal_seq != 0) {
        record.journal_broker = static_cast<uint8_t>(broker);
        record.journal_seq = journal_seq;
    } else if (slot.state == SLOT_RECORD_0 || slot.state == SLOT_RECORD_1) {
        const Record& current = slot.records[slot.state - SLOT_RECORD_0];
        record.journal_broker = current.journal_broker;
        record.journal_seq = current.journal_seq;
    }

    const std::size_t kept = std::min(lots.size(), MAX_LOTS);
    std::copy_n(lots.begin(), kept, record.lots);
    if (lots.size() > MAX_LOTS) {
        // More lots than a slot holds: the newest are kept as one, at their average price.
        // FIFO closes the oldest first, so this only matters once those are gone.
        double quantity = 0.0;
        double cost = 0.0;
        for (std::size_t i = MAX_LOTS - 1; i < lots.size(); ++i) {
            quantity += from_qty_t(lots[i].quantity);
            cost += from_qty_t(lots[i].quantity) * from_price_t(lots[i].price);
        }
        PositionLot& merged = record.lots[MAX_LOTS - 1];
        merged.quantity = to_qty_t(quantity);
        merged.price = quantity > 0.0 ? to_price_t(cost / quantity) : lots[MAX_LOTS - 1].price;
        merged.timestamp = lots[MAX_LOTS - 1].timestamp;
        if (!lots_logged_) {
            lots_logged_ = true;
            LOG_WARN("PositionStore: {} {} has {} open lots; the newest {} are kept as one at their average price",
                     strategy_id, position.instrument, lots.size(), lots.size() - (MAX_LOTS - 1));
        }
    }
    record.lot_count = static_cast<uint32_t>(kept);

    std::memcpy(&slot.records[next_state - SLOT_RECORD_0], &record, sizeof(Record));
    std::atomic_signal_fence(std::memory_order_seq_cst);   // the compiler keeps that order too
    slot.state = next_state;
    return slot_index;
}

void PositionStore::erase_strategy(std::string_view strategy_id) {
    for (std::size_t i = 0; i < capacity_; ++i) {
        Slot& slot = slot_at(i);
        if (slot.state != SLOT_EMPTY && load_key(slot.strategy_id) == strategy_id) {
            slot.state = SLOT_EMPTY;
            --size_;
            next_free_ = std::min(next_free_, i);
        }
    }
}

void PositionStore::clear() {
    for (std::size_t i = 0; i < capacity_; ++i) {
        slot_at(i).state = SLOT_EMPTY;
    }
    size_ = 0;
    next_free_ = 0;
}

} // namespace slick::trader
