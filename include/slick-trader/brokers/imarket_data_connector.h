#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <slick-trader/events/event.h>

// Forward declare SlickQueue
namespace slick {
    template<typename T>
    class SlickQueue;
}

namespace slick::trader {

enum class SubscriptionType : uint8_t {
    TRADE,
    QUOTE,
    ORDER_BOOK
};

// Market data connector interface
// Each connector runs on its own thread and publishes to SlickQueue<uint8_t>
class IMarketDataConnector {
public:
    virtual ~IMarketDataConnector() = default;

    // Connection lifecycle
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;

    // Subscriptions
    virtual bool subscribe(SubscriptionType type, const std::vector<std::string>& instruments, int depth = 10) = 0;
    virtual bool unsubscribe(SubscriptionType type, const std::vector<std::string>& instruments) = 0;

    // Thread lifecycle
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool is_running() const = 0;

    // Queue access for publishing market data events
    virtual void set_md_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) = 0;
};

} // namespace slick::trader
