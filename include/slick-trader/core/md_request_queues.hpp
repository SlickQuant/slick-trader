#pragma once

#include <slick-trader/core/broker_status.hpp>
#include <slick-trader/events/event.hpp>
#include <slick/queue.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace slick::trader {

// A market data request queue per broker. StrategyManager is the one writer of each and the
// broker's market data connector its one reader, so a connector that stops reading holds up its
// own requests alone, and the writer never laps one it waits on. Made at startup, before
// StrategyManager, and not changed after.
class MdRequestQueues {
public:
    static constexpr size_t BROKER_COUNT = static_cast<size_t>(BrokerId::_BROKER_ID_COUNT_);

    // Each queue `size` bytes. With `shm_name`, each in shared memory named "<shm_name>_<BROKER>".
    explicit MdRequestQueues(uint32_t size, std::string shm_name = {})
        : size_(size), shm_name_(std::move(shm_name)) {}

    // `broker`'s queue, made on the first call.
    slick::queue<uint8_t>& get(BrokerId broker) {
        const auto i = static_cast<size_t>(broker);
        if (broker == BrokerId::UNKNOWN || i >= BROKER_COUNT) {
            throw std::invalid_argument("no market data request queue for broker " + std::to_string(i));
        }
        auto& queue = queues_[i];
        if (!queue) {
            const std::string name = shm_name_.empty() ? std::string() : shm_name_ + "_" + to_string(broker);
            queue = std::make_unique<slick::queue<uint8_t>>(size_, name.empty() ? nullptr : name.c_str());
            // Nothing is read off a new queue yet. A cursor a connector of another queue reported
            // would let StrategyManager write over requests this one's has not read.
            BrokerStatusRegistry::instance().set_md_request_cursor(broker, BrokerStatus::NO_MD_REQUEST_CURSOR);
        }
        return *queue;
    }

    // `broker`'s queue, or nullptr when none was made, as for a broker this engine did not build.
    slick::queue<uint8_t>* find(BrokerId broker) const noexcept {
        const auto i = static_cast<size_t>(broker);
        return i < BROKER_COUNT ? queues_[i].get() : nullptr;
    }

private:
    uint32_t size_;
    std::string shm_name_;
    std::array<std::unique_ptr<slick::queue<uint8_t>>, BROKER_COUNT> queues_;
};

} // namespace slick::trader
