#pragma once

#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace slick::trader {

// The last `capacity` fills the dashboard was sent, for a Fills window opened later. Each is kept
// as the message it went out as, serialized once, with a `seq` that grows by one per fill: a page
// that read the history drops what it gets again over the WebSocket by it.
// Single-threaded: the WebServer's event loop owns it.
class FillHistory {
public:
    explicit FillHistory(size_t capacity) : ring_(capacity) {}

    // Stamps `fill` with the next seq, keeps it, and returns it as text to send. The oldest goes
    // once the ring is full.
    const std::string& push(nlohmann::json& fill) {
        fill["seq"] = ++seq_;
        std::string& slot = ring_[next_];
        slot = fill.dump();
        next_ = (next_ + 1) % ring_.size();
        size_ += size_ < ring_.size();
        return slot;
    }

    // The fills kept as a JSON array, oldest first, into `out`.
    void write_json(std::string& out) const {
        out.assign("[");
        const size_t first = (next_ + ring_.size() - size_) % ring_.size();
        for (size_t i = 0; i < size_; ++i) {
            if (i != 0) {
                out += ',';
            }
            out += ring_[(first + i) % ring_.size()];
        }
        out += ']';
    }

    size_t size() const noexcept { return size_; }
    uint64_t last_seq() const noexcept { return seq_; }

private:
    std::vector<std::string> ring_;
    size_t next_ = 0;
    size_t size_ = 0;
    uint64_t seq_ = 0;
};

} // namespace slick::trader
