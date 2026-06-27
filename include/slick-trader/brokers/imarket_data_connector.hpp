#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <slick-trader/events/md_subscription.hpp>

// Forward declare SlickQueue
namespace slick {
    template<typename T>
    class SlickQueue;
}

namespace slick::trader {

// Market data connector interface
// Each connector runs on its own thread and publishes to SlickQueue<uint8_t>
class IMarketDataConnector {
public:
    virtual ~IMarketDataConnector() = default;

    virtual void register_md_connector() = 0;

    // Connection lifecycle
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;

    // Subscriptions
    virtual bool subscribe(const MDSubscription& request) = 0;
    virtual bool unsubscribe(const MDSubscription& request) = 0;

    // Thread lifecycle
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool is_running() const = 0;
};

} // namespace slick::trader
