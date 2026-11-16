// The browser policies the engine sets for the dashboard's origin: pop-ups without a click each,
// so workspaces open all their windows, and an http origin treated as secure. The origin a setting
// names, and the Edge and Chrome URL lists it goes in.

#include <gtest/gtest.h>

#include <slick-trader/web/browser_policy.hpp>

#include <array>
#include <chrono>
#include <map>
#include <string>

#ifdef _WIN32
    #include <windows.h>
#endif

using namespace slick::trader;
using nlohmann::json;

TEST(BrowserPolicy, AnOriginIsAnAddressAndAPort) {
    EXPECT_EQ(dashboard_origin("192.168.1.20", 3000), "http://192.168.1.20:3000");
    EXPECT_EQ(dashboard_origin("localhost", 3000), "http://localhost:3000");
    EXPECT_EQ(dashboard_origin("trader-pc.lan", 65535), "http://trader-pc.lan:65535");
    EXPECT_EQ(dashboard_origin("::1", 1), "http://[::1]:1");
}

TEST(BrowserPolicy, WhatIsNoAddressOrPortMakesNoOrigin) {
    for (const char* ip : {"", "a b", "host/path", "localhost:3000", "[::1]", "x@y", "http://h"}) {
        EXPECT_EQ(dashboard_origin(ip, 3000), "") << ip;
    }
    EXPECT_EQ(dashboard_origin(std::string(254, 'a'), 3000), "");
    EXPECT_EQ(dashboard_origin("localhost", 0), "");
    EXPECT_EQ(dashboard_origin("localhost", 65536), "");
}

#ifdef _WIN32

namespace {

using Entries = std::map<std::wstring, std::wstring>;

// Each test writes under a key of its own in HKCU, never the browsers' policies.
class BrowserPolicyKeyTest : public ::testing::Test {
protected:
    void TearDown() override { RegDeleteTreeW(HKEY_CURRENT_USER, wide(root_).c_str()); }

    static std::wstring wide(const std::string& s) { return {s.begin(), s.end()}; }

    static Entries entries(const std::string& key) {
        Entries out;
        HKEY hkey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, wide(key).c_str(), 0, KEY_READ, &hkey) != ERROR_SUCCESS) {
            return out;
        }
        wchar_t name[256];
        wchar_t data[1024];
        for (DWORD i = 0;; ++i) {
            DWORD name_len = 256;
            DWORD size = sizeof(data);
            if (RegEnumValueW(hkey, i, name, &name_len, nullptr, nullptr, reinterpret_cast<BYTE*>(data), &size) !=
                ERROR_SUCCESS) {
                break;
            }
            out[std::wstring(name, name_len)] = std::wstring(data);
        }
        RegCloseKey(hkey);
        return out;
    }

    static void set(const std::string& key, const std::string& name, const std::string& value) {
        const std::wstring v = wide(value);
        RegSetKeyValueW(HKEY_CURRENT_USER, wide(key).c_str(), wide(name).c_str(), REG_SZ, v.c_str(),
                        static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    }

    std::string root_ = "Software\\SlickTraderTest-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::string edge_ = root_ + "\\Edge\\PopupsAllowedForUrls";
    std::string chrome_ = root_ + "\\Chrome\\PopupsAllowedForUrls";
    std::array<std::string_view, 2> keys_{edge_, chrome_};
    std::string error_;
};

} // namespace

TEST_F(BrowserPolicyKeyTest, AUrlGoesUnderOneAndOnlyOnce) {
    EXPECT_EQ(add_policy_url(edge_, "http://localhost:3000", error_), PolicyUrl::ADDED);
    EXPECT_EQ(add_policy_url(edge_, "http://localhost:3000", error_), PolicyUrl::LISTED);
    EXPECT_EQ(entries(edge_), (Entries{{L"1", L"http://localhost:3000"}}));
}

TEST_F(BrowserPolicyKeyTest, OtherEntriesStayAndANewOneFillsTheFirstGap) {
    set(edge_, "1", "https://intranet");
    set(edge_, "3", "http://other:80");
    set(edge_, "note", "kept");
    EXPECT_EQ(add_policy_url(edge_, "http://localhost:3000", error_), PolicyUrl::ADDED);
    EXPECT_EQ(entries(edge_), (Entries{{L"1", L"https://intranet"}, {L"2", L"http://localhost:3000"},
                                       {L"3", L"http://other:80"}, {L"note", L"kept"}}));
}

// Browsers read 1, 2, ... up to the first number missing: past it, "01" or "url" they never see.
TEST_F(BrowserPolicyKeyTest, AUrlBrowsersDontReadIsListedAgain) {
    set(edge_, "1", "https://intranet");
    set(edge_, "3", "http://localhost:3000");
    set(edge_, "01", "http://localhost:3000");
    set(edge_, "url", "http://localhost:3000");
    EXPECT_EQ(add_policy_url(edge_, "http://localhost:3000", error_), PolicyUrl::ADDED);
    EXPECT_EQ(entries(edge_)[L"2"], L"http://localhost:3000");
}

TEST_F(BrowserPolicyKeyTest, AKeyThatCantBeWrittenFails) {
    // A key name is at most 255 characters.
    EXPECT_EQ(add_policy_url(root_ + "\\" + std::string(300, 'k'), "http://localhost:3000", error_), PolicyUrl::FAILED);
    EXPECT_FALSE(error_.empty());
}

TEST_F(BrowserPolicyKeyTest, ASettingListsItsOriginInEachOfItsKeys) {
    const auto with = [](json at) { return json{{"web_server_port", 3000}, {"origin", std::move(at)}}; };
    list_origin(with({{"ip", "192.168.1.20"}, {"port", 3001}}), "origin", 3000, keys_);
    list_origin(with({{"ip", "192.168.1.20"}}), "origin", 3000, keys_);   // the server's port
    list_origin(with(json::object()), "origin", 3000, keys_);            // localhost
    list_origin(with({{"ip", "192.168.1.20"}, {"port", 3001}}), "origin", 3000, keys_);
    const Entries want{{L"1", L"http://192.168.1.20:3001"}, {L"2", L"http://192.168.1.20:3000"},
                       {L"3", L"http://localhost:3000"}};
    EXPECT_EQ(entries(edge_), want);
    EXPECT_EQ(entries(chrome_), want);
}

// Each setting is off unless there, and writes its own keys alone.
TEST_F(BrowserPolicyKeyTest, NoSettingWritesNothing) {
    const json config{{"web_server_port", 3000}, {"popups", {{"ip", "10.0.0.5"}}}};
    list_origin(config, "secure", 3000, keys_);
    EXPECT_TRUE(entries(edge_).empty());
    list_origin(config, "popups", 3000, std::array<std::string_view, 1>{chrome_});
    EXPECT_TRUE(entries(edge_).empty());
    EXPECT_EQ(entries(chrome_), (Entries{{L"1", L"http://10.0.0.5:3000"}}));
}

TEST_F(BrowserPolicyKeyTest, ASettingThatNamesNoOriginWritesNothing) {
    for (const json& bad : {json("localhost"), json::array(), json{{"ip", "a b"}}, json{{"ip", 1}},
                            json{{"port", "3000"}}, json{{"port", 0}}}) {
        list_origin(json{{"origin", bad}}, "origin", 3000, keys_);
    }
    EXPECT_TRUE(entries(edge_).empty());
    EXPECT_TRUE(entries(chrome_).empty());
}

#else

TEST(BrowserPolicy, OnlyWindowsKeepsThePolicyInTheRegistry) {
    std::string error;
    EXPECT_EQ(add_policy_url(EDGE_POPUPS_KEY, "http://localhost:3000", error), PolicyUrl::FAILED);
    EXPECT_FALSE(error.empty());
}

#endif
