#pragma once

#include <string>
#include <memory>
#include <slick-trader/events/event.h>

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

    // Lifecycle
    virtual bool initialize(const std::string& config_json) = 0;
    virtual void shutdown() = 0;

    // Connector access
    virtual std::shared_ptr<IMarketDataConnector> get_market_data_connector() = 0;
    virtual std::shared_ptr<IOrderConnector> get_order_connector() = 0;
};

} // namespace slick::trader
