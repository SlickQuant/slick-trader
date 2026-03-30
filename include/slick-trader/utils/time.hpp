#pragma once

#include <chrono>

namespace slick::trader {

inline uint64_t get_timestamp_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

}

} // namespace slick::trader