#pragma once

#include <slick-trader/core/broker_status.hpp>
#include <slick-trader/utils/time.hpp>
#include <slick/logger.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace slick::trader {

// An environment variable's value, or `fallback` when it is unset or empty. Credentials
// come from here, never from a config file on disk.
inline std::string env_or(const char* name, std::string fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::string(value) : std::move(fallback);
}

// The wall clock minus the framework's monotonic clock (get_timestamp_ns()), taken once:
// timers run on the monotonic clock, exchanges want wall-clock times on the wire.
inline int64_t wall_clock_offset_ns() {
    const auto wall_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    return static_cast<int64_t>(wall_ns) - static_cast<int64_t>(get_timestamp_ns());
}

inline uint64_t wall_clock_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// A connector's up/down state as BrokerStatusRegistry shows it, and its `connected`
// flag: set, and logged, only when it changes.
class StatusLatch {
public:
    enum class Component : uint8_t { MARKET_DATA, ORDER };

    StatusLatch(BrokerId broker, Component component) noexcept : broker_(broker), component_(component) {}

    // Returns true when `up` changed the state.
    bool update(bool up, std::string_view who, std::string_view up_text, std::string_view down_text) {
        if (up == up_) {
            return false;
        }
        up_ = up;
        connected_.store(up, std::memory_order_release);
        publish(up);
        LOG_INFO("{}: {}", who, up ? up_text : down_text);
        return true;
    }

    // Down, without a log line: the connector is stopping.
    void reset() {
        up_ = false;
        connected_.store(false, std::memory_order_release);
        publish(false);
    }

    [[nodiscard]] bool up() const noexcept { return connected_.load(std::memory_order_acquire); }

private:
    void publish(bool up) {
        auto& registry = BrokerStatusRegistry::instance();
        if (component_ == Component::MARKET_DATA) {
            registry.set_md_connected(broker_, up);
        } else {
            registry.set_order_connected(broker_, up);
        }
    }

    BrokerId broker_;
    Component component_;
    bool up_ = false;
    std::atomic_bool connected_{false};
};

} // namespace slick::trader
