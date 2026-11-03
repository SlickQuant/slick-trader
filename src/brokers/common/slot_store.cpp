#include <slick-trader/brokers/common/slot_store.hpp>
#include <slick/logger.hpp>
#include <algorithm>
#include <stdexcept>

namespace slick::trader::detail {

namespace {

// Every field at its natural alignment with no padding between them: the layout is the
// file format.
struct FileHeader {
    char magic[8];
    uint32_t version;
    uint32_t slot_size;
    char identity[SlotFile::IDENTITY_SIZE];
    uint8_t state[SlotFile::STATE_SIZE];
};
static_assert(sizeof(FileHeader) == SlotFile::HEADER_SIZE);
static_assert(offsetof(FileHeader, state) == SlotFile::STATE_OFFSET);

}   // namespace

SlotFile::SlotFile(const std::string& path, const SlotStoreFormat& format,
                   const std::array<char, IDENTITY_SIZE>& identity, std::string_view identity_text, size_t slot_size,
                   size_t max_slots, const void* fresh_state, size_t state_size)
    : path_(path)
    , name_(format.name)
    , file_(path, HEADER_SIZE + std::max<size_t>(max_slots, 1) * slot_size)
    , slot_size_(slot_size)
{
    auto* header = reinterpret_cast<FileHeader*>(file_.data());
    // A file nothing has been written to is all zeros, which a fresh state need not be.
    const bool blank = std::all_of(file_.data(), file_.data() + HEADER_SIZE, [](uint8_t b) { return b == 0; });
    if (blank) {
        created_ = true;
        FileHeader fresh{};
        fresh.version = format.version;
        fresh.slot_size = static_cast<uint32_t>(slot_size);
        std::memcpy(fresh.identity, identity.data(), IDENTITY_SIZE);
        std::memcpy(fresh.state, fresh_state, state_size);
        std::memcpy(fresh.magic, format.magic, sizeof(fresh.magic));
        std::memcpy(header, &fresh, HEADER_SIZE);
    } else if (std::memcmp(header->magic, format.magic, sizeof(header->magic)) != 0) {
        throw std::runtime_error(std::string(format.name) + ": " + path + " is not a session store");
    } else if (header->version != format.version || header->slot_size != slot_size) {
        throw std::runtime_error(std::string(format.name) + ": " + path +
                                 " was written with a different layout (version " +
                                 std::to_string(header->version) + ")");
    } else if (std::memcmp(header->identity, identity.data(), IDENTITY_SIZE) != 0) {
        throw std::runtime_error(std::string(format.name) + ": " + path + " belongs to another session, not " +
                                 std::string(identity_text));
    }
    capacity_ = static_cast<uint32_t>((file_.size() - HEADER_SIZE) / slot_size_);
    LOG_INFO("{}: {} has room for {} record(s) of {}", name_, path_, capacity_, identity_text);
}

void report_slot_store_full(const char* name, const std::string& path, size_t capacity) {
    LOG_ERROR("{}: {} is full ({} records) - further orders are NOT kept across a restart; raise "
              "max_persisted_orders",
              name, path, capacity);
}

} // namespace slick::trader::detail
