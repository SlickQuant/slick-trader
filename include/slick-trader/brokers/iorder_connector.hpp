#pragma once

#include <memory>

#include <slick/queue.hpp>
#include <slick-trader/events/event.hpp>

namespace slick::trader {

// Whether an order request is addressed to `broker`. Every order connector reads every
// request on the shared order queue, so each must act only on its own: without this
// check an order for one broker is also sent to every other broker that is enabled.
inline bool is_request_for(const Event& request, BrokerId broker) noexcept {
    return request.broker_id == broker;
}

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
