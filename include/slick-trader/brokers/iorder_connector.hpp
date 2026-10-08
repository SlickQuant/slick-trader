#pragma once

#include <memory>

#include <slick/queue.hpp>

namespace slick::trader {

// Order connector interface
// Each connector runs on its own thread
// - Consumes OrderRequest/CancelRequest from order submission queue
// - Publishes order events (ack, fill, reject, cancel) to order event queue
class IOrderConnector {
public:
    virtual ~IOrderConnector() = default;

    virtual void register_order_connector() = 0;

    // Connection lifecycle
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;

    // Thread lifecycle
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool is_running() const = 0;
};

} // namespace slick::trader
