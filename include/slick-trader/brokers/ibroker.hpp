#pragma once

#include <string>
#include <string_view>
#include <memory>
#include <slick/stream_buffer_multiplexer.hpp>
#include <slick-trader/events/event.hpp>
#include <slick-trader/utils/fee_structure.hpp>

namespace slick::trader {

// Forward declarations
class IMarketDataConnector;
class IOrderConnector;

// Base broker interface
class IBroker {
public:
    virtual ~IBroker() = default;

    // Broker identification
    virtual BrokerId get_broker_id() const = 0;
    virtual const char* get_broker_name() const = 0;

    virtual void register_broker() = 0;

    // Lifecycle
    virtual bool start() = 0;
    virtual void stop() = 0;

    // Connector access
    // virtual std::shared_ptr<IMarketDataConnector> get_market_data_connector() = 0;
    // virtual std::shared_ptr<IOrderConnector> get_order_connector() = 0;

    // Fee structure for this broker (symbol ignored for exchanges with uniform rates)
    // Returns zero-rate by default for brokers that report fees directly in fill events
    virtual FeeStructure get_fees(std::string_view symbol) const { return {}; }
};

} // namespace slick::trader
