#pragma once

#include "event.hpp"
#include <limits>
#include <slick-trader/utils/fee_structure.hpp>

namespace slick::trader {

constexpr uint32_t INVALID_PRODUCER_ID = std::numeric_limits<uint32_t>::max();

enum class ComponentType : uint8_t {
    UNKNOWN,
    MARKET_DATA,
    ORDER_CONNECTOR,
};

#pragma pack(push, 1)
struct RegisterEvent : public Event {
    ComponentType component_type;
    uint32_t producer_id;
    char shm_name[31] = {'\0'};

    RegisterEvent(ComponentType ct = ComponentType::UNKNOWN)
        : Event(EventType::REGISTER)
        , component_type(ct)
        , producer_id(INVALID_PRODUCER_ID)
    {
    }
};

struct OrderConnectorRegisterEvent : public RegisterEvent {
    FeeStructure fee_structure;

    OrderConnectorRegisterEvent()
        : RegisterEvent(ComponentType::ORDER_CONNECTOR)
        , fee_structure{}
    {
    }
};

struct BrokerRegisterEndEvent : public Event {
    size_t num_producers;
    BrokerRegisterEndEvent()
        : Event(EventType::REGISTER_END)
    {
    }
};

#pragma pack(pop)

inline std::string_view to_string(ComponentType ct) {
    switch (ct) {
        case ComponentType::MARKET_DATA:
            return "MARKET_DATA";
        case ComponentType::ORDER_CONNECTOR:
            return "ORDER_CONNECTOR";
        default:
            return "UNKNOWN";
    }
}

}