#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace slick::trader {

// The venue environment the config's top-level "environment" names. Venues don't say which one
// a session is on, so the config does, and the dashboard flags anything that isn't production.
inline constexpr std::string_view PRODUCTION_ENVIRONMENT = "Production";

// Empty, "production" or "prod", in any case. Anything else, a misspelling included, is not:
// the dashboard then shows its bar rather than hiding a sim or cert session.
inline bool is_production_environment(std::string_view env) noexcept {
    const auto equals = [env](std::string_view word) {
        return env.size() == word.size() &&
               std::equal(env.begin(), env.end(), word.begin(), [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) == b;
               });
    };
    return env.empty() || equals("production") || equals("prod");
}

// The config's "environment" into `env`, PRODUCTION_ENVIRONMENT when it has none. False when it is
// there but not a string: a mistake, and reading it as production would hide the bar.
inline bool read_environment(const nlohmann::json& config, std::string& env) {
    const auto it = config.find("environment");
    if (it == config.end()) {
        env = PRODUCTION_ENVIRONMENT;
        return true;
    }
    if (!it->is_string()) {
        return false;
    }
    env = it->get<std::string>();
    return true;
}

} // namespace slick::trader
