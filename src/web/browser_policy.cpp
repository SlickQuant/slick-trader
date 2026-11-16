#include <slick-trader/web/browser_policy.hpp>
#include <slick/logger.hpp>
#include <algorithm>
#include <cctype>
#include <system_error>
#include <vector>

#ifdef _WIN32
    #include <windows.h>
#endif

namespace slick::trader {

namespace {

constexpr size_t MAX_HOST = 253;

bool host_char(unsigned char c) {
    return std::isalnum(c) || c == '.' || c == '-';
}

bool ipv6_char(unsigned char c) {
    return std::isxdigit(c) || c == ':' || c == '.';
}

#ifdef _WIN32
std::wstring widen(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    std::wstring out(MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), static_cast<int>(out.size()));
    return out;
}

// The entry number a value name holds: "1", "2", ...; 0 for any other name, "01" too.
unsigned long entry_number(const wchar_t* name) {
    if (*name == L'0') {
        return 0;
    }
    unsigned long n = 0;
    for (const wchar_t* c = name; *c; ++c) {
        if (*c < L'0' || *c > L'9' || n > 1'000'000) {
            return 0;
        }
        n = n * 10 + static_cast<unsigned long>(*c - L'0');
    }
    return n;
}
#endif

} // namespace

std::string dashboard_origin(std::string_view ip, int64_t port) {
    if (ip.empty() || ip.size() > MAX_HOST || port < 1 || port > 65535) {
        return {};
    }
    const bool v6 = ip.find(':') != std::string_view::npos;
    if (!std::all_of(ip.begin(), ip.end(), v6 ? ipv6_char : host_char)) {
        return {};
    }
    std::string origin = "http://";
    if (v6) {
        origin.append("[").append(ip).append("]");
    } else {
        origin.append(ip);
    }
    return origin.append(":").append(std::to_string(port));
}

PolicyUrl add_policy_url(std::string_view key, std::string_view url, std::string& error) {
#ifdef _WIN32
    HKEY hkey = nullptr;
    LSTATUS rc = RegCreateKeyExW(HKEY_CURRENT_USER, widen(key).c_str(), 0, nullptr, 0,
                                 KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &hkey, nullptr);
    if (rc != ERROR_SUCCESS) {
        error = std::system_category().message(static_cast<int>(rc));
        return PolicyUrl::FAILED;
    }
    const std::wstring wanted = widen(url);
    std::vector<unsigned long> taken;     // entry numbers
    std::vector<unsigned long> holding;   // those of them that hold `url`
    wchar_t name[256];
    wchar_t data[1024];
    for (DWORD i = 0;; ++i) {
        DWORD name_len = static_cast<DWORD>(std::size(name));
        rc = RegEnumValueW(hkey, i, name, &name_len, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS) {
            break;
        }
        const auto n = rc == ERROR_SUCCESS ? entry_number(name) : 0;   // a longer name is no entry's
        if (n == 0) {
            continue;
        }
        taken.push_back(n);
        DWORD size = sizeof(data);   // a longer value is no URL of ours
        if (RegGetValueW(hkey, nullptr, name, RRF_RT_REG_SZ, nullptr, data, &size) == ERROR_SUCCESS &&
            std::wstring_view(data) == wanted) {
            holding.push_back(n);
        }
    }
    // Browsers read 1, 2, ... up to the first number missing: an entry past it doesn't count, and
    // a new one goes there.
    std::sort(taken.begin(), taken.end());
    unsigned long next = 1;
    for (const auto t : taken) {
        next += t == next;
    }
    PolicyUrl result = PolicyUrl::LISTED;
    if (std::none_of(holding.begin(), holding.end(), [next](unsigned long h) { return h < next; })) {
        const std::wstring value = std::to_wstring(next);
        rc = RegSetValueExW(hkey, value.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(wanted.c_str()),
                            static_cast<DWORD>((wanted.size() + 1) * sizeof(wchar_t)));
        result = rc == ERROR_SUCCESS ? PolicyUrl::ADDED : PolicyUrl::FAILED;
        if (rc != ERROR_SUCCESS) {
            error = std::system_category().message(static_cast<int>(rc));
        }
    }
    RegCloseKey(hkey);
    return result;
#else
    (void)key;
    (void)url;
    error = "browser policies are set in the Windows registry only";
    return PolicyUrl::FAILED;
#endif
}

void list_origin(const nlohmann::json& config, std::string_view setting, int64_t default_port,
                 std::span<const std::string_view> keys) {
    const auto found = config.is_object() ? config.find(std::string(setting)) : config.end();
    if (found == config.end()) {
        return;
    }
    const auto& at = *found;
    const auto ip = at.is_object() ? at.find("ip") : at.end();
    const auto port = at.is_object() ? at.find("port") : at.end();
    std::string origin;
    if (at.is_object() && (ip == at.end() || ip->is_string()) && (port == at.end() || port->is_number_integer())) {
        origin = dashboard_origin(ip == at.end() ? "localhost" : ip->get_ref<const std::string&>(),
                                  port == at.end() ? default_port : port->get<int64_t>());
    }
    if (origin.empty()) {
        LOG_WARN("WebServer: {} wants {{\"ip\": <address>, \"port\": <1..65535>}}, not {}", setting, at.dump());
        return;
    }
    for (const auto key : keys) {
        std::string error;
        switch (add_policy_url(key, origin, error)) {
        case PolicyUrl::ADDED:
            LOG_INFO("WebServer: {}: added {} to HKCU\\{}", setting, origin, key);
            break;
        case PolicyUrl::LISTED:
            LOG_INFO("WebServer: {}: HKCU\\{} lists {}", setting, key, origin);
            break;
        case PolicyUrl::FAILED:
            LOG_WARN("WebServer: could not list {} in HKCU\\{}: {}", origin, key, error);
            break;
        }
    }
}

} // namespace slick::trader
