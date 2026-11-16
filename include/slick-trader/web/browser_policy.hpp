#pragma once

#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace slick::trader {

// Chrome and Edge let a click open one pop-up, unless the site may open them: a workspace opens
// several at once. These URL list policies, under HKCU, name the sites that may. Each entry is a
// REG_SZ value named "1", "2", ..., read up to the first number missing.
inline constexpr std::string_view EDGE_POPUPS_KEY = R"(Software\Policies\Microsoft\Edge\PopupsAllowedForUrls)";
inline constexpr std::string_view CHROME_POPUPS_KEY = R"(Software\Policies\Google\Chrome\PopupsAllowedForUrls)";
inline constexpr std::array<std::string_view, 2> POPUP_POLICY_KEYS{EDGE_POPUPS_KEY, CHROME_POPUPS_KEY};

// Some APIs are for secure origins only: https, and http on localhost. Placing a workspace's windows
// on other screens is one. These list origins to treat as secure too, as the flag "Insecure origins
// treated as secure" does, in the same format.
inline constexpr std::string_view EDGE_SECURE_KEY =
    R"(Software\Policies\Microsoft\Edge\OverrideSecurityRestrictionsOnInsecureOrigin)";
inline constexpr std::string_view CHROME_SECURE_KEY =
    R"(Software\Policies\Google\Chrome\OverrideSecurityRestrictionsOnInsecureOrigin)";
inline constexpr std::array<std::string_view, 2> SECURE_POLICY_KEYS{EDGE_SECURE_KEY, CHROME_SECURE_KEY};

// "http://<ip>:<port>", an IPv6 address in brackets. Empty unless `ip` is an address or host name
// and `port` is 1..65535.
std::string dashboard_origin(std::string_view ip, int64_t port);

enum class PolicyUrl { ADDED, LISTED, FAILED };

// Lists `url` in the URL list policy at HKCU\<key>, under the lowest number no entry holds.
// FAILED, with `error`, when the key can't be written, and anywhere but Windows.
PolicyUrl add_policy_url(std::string_view key, std::string_view url, std::string& error);

// When `config` has `setting`, {"ip": ..., "port": ...}, lists that origin in each of `keys` and
// logs what it did. `ip` defaults to localhost, `port` to `default_port`.
void list_origin(const nlohmann::json& config, std::string_view setting, int64_t default_port,
                 std::span<const std::string_view> keys);

} // namespace slick::trader
