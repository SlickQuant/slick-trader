#pragma once

#include <slick-trader/brokers/ibroker.hpp>
#include <slick/stream_buffer_multiplexer.hpp>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace slick::trader {

using json = nlohmann::json;

/// Maps a broker's config `name` (e.g. "coinbase") to the adapter that serves it,
/// so no broker name appears in TradingEngine or in any core target. Each broker
/// registers itself at static-init time with SLICK_TRADER_REGISTER_BROKER below,
/// which is also how a broker kept out of tree (SLICK_TRADER_EXTERNAL_BROKERS)
/// joins the build without editing it.
///
/// Populated once at startup and read once per configured broker, so nothing here
/// is on a hot path: a flat vector is the right size.
class BrokerRegistry {
public:
    /// A plain function pointer, not std::function - no allocation, no <functional>.
    using Factory = std::shared_ptr<IBroker> (*)(
        const json& config,
        slick::stream_buffer_multiplexer& event_mux,
        slick::queue<uint8_t>& control_queue,
        slick::queue<uint8_t>& md_request_queue,
        slick::queue<uint8_t>& order_request_queue);

    static BrokerRegistry& instance();

    /// `key` is matched case-insensitively. A second registration under the same
    /// key is refused and returns false; the first one stays. Returns a bool so a
    /// registration can initialise a namespace-scope constant.
    bool add(std::string_view key, Factory factory);

    /// nullptr when no adapter for this key was built into the binary.
    Factory find(std::string_view key) const noexcept;

    /// Registered keys, lowercased, in registration order - for the startup log
    /// and for the error on an unknown key.
    std::vector<std::string> keys() const;

private:
    BrokerRegistry() = default;

    struct Entry {
        std::string key;   // lowercased
        Factory factory;
    };
    std::vector<Entry> entries_;
};

} // namespace slick::trader

/// Defines a broker's factory and registers it. Place it once, at namespace scope,
/// in a translation unit of the broker's library:
///
///     SLICK_TRADER_REGISTER_BROKER("coinbase", CoinbaseBroker);
///
/// That TU must live in an OBJECT library - slick_trader_add_broker() makes one -
/// because nothing references the registration by name, and out of a static
/// library the linker would drop it and the broker would silently vanish.
#define SLICK_TRADER_REGISTER_BROKER(key, BrokerType)                               \
    namespace {                                                                     \
    ::std::shared_ptr<::slick::trader::IBroker> slick_trader_make_##BrokerType(     \
        const ::nlohmann::json& config,                                             \
        ::slick::stream_buffer_multiplexer& event_mux,                              \
        ::slick::queue<uint8_t>& control_queue,                                     \
        ::slick::queue<uint8_t>& md_request_queue,                                  \
        ::slick::queue<uint8_t>& order_request_queue) {                             \
        return ::std::make_shared<BrokerType>(                                      \
            config, event_mux, control_queue, md_request_queue, order_request_queue); \
    }                                                                               \
    [[maybe_unused]] const bool slick_trader_registered_##BrokerType =              \
        ::slick::trader::BrokerRegistry::instance().add(key, &slick_trader_make_##BrokerType); \
    }                                                                               \
    static_assert(true, "require a trailing semicolon")
