#pragma once

#include <slick-trader/utils/time.hpp>
#include <slick/logger.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <thread>

namespace slick::trader {

// Working orders a kill switch is still waiting on.
struct KillSwitchCount {
    size_t waiting = 0;       // on sessions that can still be told
    size_t unreachable = 0;   // on sessions that are down
};

// The last thing an order worker does: pulls every working order before its sessions
// end, waiting up to `timeout_ns` for the exchange to confirm. No new requests are read
// from here on, only the exchange's answers.
//
// `service(now) -> bool` reads the sockets (true when anything was read); `step(now) ->
// KillSwitchCount` asks each session to cancel what it still has and counts what is
// left. `fallback` says what takes the orders left over (an exchange's cancel on
// disconnect, say).
template <typename Service, typename Step>
KillSwitchCount run_kill_switch(std::string_view who, size_t working, uint64_t timeout_ns, Service&& service,
                                Step&& step, std::string_view fallback) {
    KillSwitchCount left;
    if (working == 0) {
        return left;
    }
    if (timeout_ns == 0) {
        LOG_WARN("{}: stopping with {} order(s) working (cancel_on_stop_ms is 0)", who, working);
        left.waiting = working;
        return left;
    }
    LOG_INFO("{}: stopping, cancelling {} working order(s)", who, working);
    const uint64_t deadline_ns = get_timestamp_ns() + timeout_ns;
    for (;;) {
        const uint64_t now_ns = get_timestamp_ns();
        const bool worked = service(now_ns);
        left = step(now_ns);
        if (left.waiting == 0 || now_ns >= deadline_ns) {
            break;
        }
        if (!worked) {
            std::this_thread::yield();
        }
    }
    if (left.waiting + left.unreachable == 0) {
        LOG_INFO("{}: every working order is cancelled or filled", who);
    } else {
        LOG_ERROR("{}: {} order(s) may still be working ({} unconfirmed, {} on a session that is down) - {}", who,
                  left.waiting + left.unreachable, left.waiting, left.unreachable, fallback);
    }
    return left;
}

} // namespace slick::trader
