#pragma once

#include <slick-trader/events/event.hpp>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

namespace slick::trader {

// Plugins index the host's registry with their own build of this struct: a change to its
// layout is a change to STRATEGY_API_VERSION.
struct BrokerStatus {
    static constexpr uint64_t NO_MD_REQUEST_CURSOR = std::numeric_limits<uint64_t>::max();

    std::atomic<bool> md_enabled{false};
    std::atomic<bool> md_connected{false};
    std::atomic<bool> order_enabled{false};
    std::atomic<bool> order_connected{false};
    char name[32]{};
};

class BrokerStatusRegistry {
public:
    // Process-wide singleton — but strategy plugins are shared libraries that link
    // slick-trader-core statically, so on Windows each module gets its own copy of
    // the function-local static below. A plugin would then read an all-false registry
    // that nothing ever writes, and every order it submits would be skipped as
    // "broker order connector disconnected". StrategyLoader injects the host's
    // instance into each plugin via set_instance(), the same way it forwards the
    // logger. See SLICK_TRADER_EXPORT_STRATEGY / set_broker_status_registry().
    static BrokerStatusRegistry& instance() {
        static BrokerStatusRegistry module_local;
        auto* shared = shared_.load(std::memory_order_acquire);
        return shared ? *shared : module_local;
    }

    // Point this module's registry at the host's instance. Called once per plugin at
    // load time, before create_strategy(); never called in the host, which keeps
    // using its own module_local.
    static void set_instance(BrokerStatusRegistry* registry) {
        shared_.store(registry, std::memory_order_release);
    }

    void set_name(BrokerId id, const char* name) {
        auto& s = slot(id);
        std::strncpy(s.name, name, sizeof(s.name) - 1);
        s.name[sizeof(s.name) - 1] = '\0';
    }

    void set_md_enabled(BrokerId id, bool v);
    void set_md_connected(BrokerId id, bool v);
    void set_order_enabled(BrokerId id, bool v);
    void set_order_connected(BrokerId id, bool v);

    bool is_md_enabled(BrokerId id)    const { return slot(id).md_enabled.load(std::memory_order_acquire); }
    bool is_md_connected(BrokerId id)  const { return slot(id).md_connected.load(std::memory_order_acquire); }
    bool is_order_enabled(BrokerId id) const { return slot(id).order_enabled.load(std::memory_order_acquire); }
    bool is_order_connected(BrokerId id) const { return slot(id).order_connected.load(std::memory_order_acquire); }

    // A broker this process built: its connectors name it when they are made.
    bool is_registered(BrokerId id) const { return slot(id).name[0] != '\0'; }

    // Why `id` cannot take an order now, or nullptr when it can.
    const char* order_unavailable_reason(BrokerId id) const {
        if (!is_order_enabled(id)) {
            return "Broker order connector disabled";
        }
        if (!is_order_connected(id)) {
            return "Broker order connector disconnected";
        }
        return nullptr;
    }

    // How far the market data connector has read its request queue, NO_MD_REQUEST_CURSOR until
    // it first reads. The queue is lossy, so StrategyManager writes no further ahead of it than
    // the queue holds. Release: the connector is done with every request before `cursor` once it
    // says so, and the next write may land on them. process_md_request() calls it.
    void set_md_request_cursor(BrokerId id, uint64_t cursor) noexcept {
        request_cursors_[static_cast<size_t>(id)].value.store(cursor, std::memory_order_release);
    }
    uint64_t md_request_cursor(BrokerId id) const noexcept {
        return request_cursors_[static_cast<size_t>(id)].value.load(std::memory_order_acquire);
    }

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

    // Module-local redirect slot: null in the host, set to the host's registry in
    // every plugin. Deliberately not shared across modules — each module needs its own.
    static inline std::atomic<BrokerStatusRegistry*> shared_{nullptr};

    std::array<BrokerStatus, static_cast<size_t>(BrokerId::_BROKER_ID_COUNT_)> statuses_;
    // A line each: every connector writes its own, and the status flags beside are read often.
    struct alignas(64) RequestCursor {
        std::atomic<uint64_t> value{BrokerStatus::NO_MD_REQUEST_CURSOR};
    };
    std::array<RequestCursor, static_cast<size_t>(BrokerId::_BROKER_ID_COUNT_)> request_cursors_;
};

} // namespace slick::trader
