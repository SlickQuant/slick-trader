#include <slick-trader/core/broker_registry.hpp>
#include <algorithm>
#include <cctype>
#include <exception>

namespace slick::trader {

namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

BrokerRegistry& BrokerRegistry::instance() {
    static BrokerRegistry registry;
    return registry;
}

bool BrokerRegistry::add(std::string_view key, Factory factory) {
    // Runs during static initialisation, before the logger exists, so a duplicate is
    // reported only through the return value.
    auto lowered = to_lower(key);
    if (factory == nullptr || find(lowered) != nullptr) {
        return false;
    }
    entries_.push_back(Entry{std::move(lowered), factory});
    return true;
}

BrokerRegistry::Factory BrokerRegistry::find(std::string_view key) const noexcept {
    for (const auto& entry : entries_) {
        if (entry.key.size() == key.size() &&
            std::equal(entry.key.begin(), entry.key.end(), key.begin(), [](char a, unsigned char b) {
                return a == static_cast<char>(std::tolower(b));
            })) {
            return entry.factory;
        }
    }
    return nullptr;
}

std::shared_ptr<IBroker> BrokerRegistry::create(std::string_view key, const json& config,
                                                slick::stream_buffer_multiplexer& event_mux,
                                                slick::queue<uint8_t>& control_queue,
                                                slick::queue<uint8_t>& md_request_queue,
                                                slick::queue<uint8_t>& order_request_queue,
                                                std::string& error) const {
    const Factory factory = find(key);
    if (factory == nullptr) {
        error = "no adapter for '" + std::string(key) + "' is built into this binary";
        return nullptr;
    }
    try {
        auto broker = factory(config, event_mux, control_queue, md_request_queue, order_request_queue);
        if (!broker) {
            error = "its factory returned nothing";
        }
        return broker;
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "its constructor threw something that is not a std::exception";
    }
    return nullptr;
}

std::vector<std::string> BrokerRegistry::keys() const {
    std::vector<std::string> result;
    result.reserve(entries_.size());
    for (const auto& entry : entries_) {
        result.push_back(entry.key);
    }
    return result;
}

} // namespace slick::trader
