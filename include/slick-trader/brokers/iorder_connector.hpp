#pragma once

#include <memory>

// Forward declare SlickQueue
namespace slick {
    template<typename T>
    class SlickQueue;
}

namespace slick::trader {

// Order connector interface
// Each connector runs on its own thread
// - Consumes OrderRequest/CancelRequest from order submission queue
// - Publishes order events (ack, fill, reject, cancel) to order event queue
class IOrderConnector {
public:
    virtual ~IOrderConnector() = default;

    // Connection lifecycle
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;

    // Thread lifecycle
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool is_running() const = 0;

    // Queue access
    // Consume order requests from this queue
    virtual void set_order_submission_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) = 0;

    // Publish order events to this queue
    virtual void set_order_event_queue(std::shared_ptr<slick::SlickQueue<uint8_t>> queue) = 0;
};

} // namespace slick::trader
