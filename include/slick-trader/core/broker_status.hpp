#pragma once

#include <slick-trader/events/event.hpp>
#include <atomic>
#include <array>
#include <cstring>

namespace slick::trader {

struct BrokerStatus {
    std::atomic<bool> md_enabled{false};
    std::atomic<bool> md_connected{false};
    std::atomic<bool> order_enabled{false};
    std::atomic<bool> order_connected{false};
    char name[32]{};
};

class BrokerStatusRegistry {
public:
    static BrokerStatusRegistry& instance() {
        static BrokerStatusRegistry inst;
        return inst;
    }

    void set_name(BrokerId id, const char* name) {
        auto& s = slot(id);
        std::strncpy(s.name, name, sizeof(s.name) - 1);
        s.name[sizeof(s.name) - 1] = '\0';
    }

    void set_md_enabled(BrokerId id, bool v)    { slot(id).md_enabled.store(v, std::memory_order_release); }
    void set_md_connected(BrokerId id, bool v)  { slot(id).md_connected.store(v, std::memory_order_release); }
    void set_order_enabled(BrokerId id, bool v) { slot(id).order_enabled.store(v, std::memory_order_release); }
    void set_order_connected(BrokerId id, bool v){ slot(id).order_connected.store(v, std::memory_order_release); }

    bool is_md_enabled(BrokerId id)    const { return slot(id).md_enabled.load(std::memory_order_acquire); }
    bool is_md_connected(BrokerId id)  const { return slot(id).md_connected.load(std::memory_order_acquire); }
    bool is_order_enabled(BrokerId id) const { return slot(id).order_enabled.load(std::memory_order_acquire); }
    bool is_order_connected(BrokerId id) const { return slot(id).order_connected.load(std::memory_order_acquire); }

    const BrokerStatus& get(BrokerId id) const { return slot(id); }

    const std::array<BrokerStatus, static_cast<size_t>(BrokerId::_BROKER_ID_COUNT_)>& all() const {
        return statuses_;
    }

private:
    BrokerStatusRegistry() = default;

    BrokerStatus& slot(BrokerId id) {
        return statuses_[static_cast<size_t>(id)];
    }
    const BrokerStatus& slot(BrokerId id) const {
        return statuses_[static_cast<size_t>(id)];
    }

    std::array<BrokerStatus, static_cast<size_t>(BrokerId::_BROKER_ID_COUNT_)> statuses_;
};

} // namespace slick::trader
