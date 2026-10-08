#include <slick-trader/core/broker_registry.hpp>
#include <algorithm>
#include <cctype>

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

std::vector<std::string> BrokerRegistry::keys() const {
    std::vector<std::string> result;
    result.reserve(entries_.size());
    for (const auto& entry : entries_) {
        result.push_back(entry.key);
    }
    return result;
}

} // namespace slick::trader
