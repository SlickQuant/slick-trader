#pragma once

#include "event.hpp"

namespace slick::trader {

enum class Status : uint8_t {
    STARTING,
    RUNNIGN,
    STOPING,
    STOPED,
};

#pragma pack(push, 1)
struct StatusEvent : public Event {
    Status status = Status::STOPED;

    StatusEvent()
        : Event(EventType::STATUS)
    {}
};
#pragma pack(pop)
}