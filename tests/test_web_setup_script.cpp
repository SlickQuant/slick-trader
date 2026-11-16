// scripts/slick-trader-web.bat, the setup for a browser on a PC other than the engine's, run as a
// user runs it. Its policy entries go under a throwaway HKCU key, and its shortcuts into a temp
// folder named like a Chinese OneDrive Desktop, outside the ANSI code page.

#include <gtest/gtest.h>

#ifdef _WIN32

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

using Entries = std::map<std::wstring, std::wstring>;

const auto STAMP = std::chrono::steady_clock::now().time_since_epoch().count();

class WebSetupScriptTest : public ::testing::Test {
protected:
    // The environment goes to the script through the OS, in UTF-16: _wputenv_s and _wsystem pass it
    // through a narrow copy, which handed the script the folder's name as mojibake.
    void SetUp() override {
        fs::create_directories(desktop_);
        SetEnvironmentVariableW(L"SLICK_POLICY_ROOT", (L"HKCU:\\" + root_).c_str());
        SetEnvironmentVariableW(L"SLICK_SHORTCUT_DIR", desktop_.c_str());
        // Never the real policies or Desktop: the script has to see where to write instead, by name.
        ASSERT_EQ(shell(L"if defined SLICK_POLICY_ROOT (if exist \"%SLICK_SHORTCUT_DIR%\\\" (exit 0) else (exit 3)) else (exit 3)"), 0);
    }

    void TearDown() override {
        RegDeleteTreeW(HKEY_CURRENT_USER, root_.c_str());
        std::error_code ignored;
        fs::remove_all(dir_, ignored);
        SetEnvironmentVariableW(L"SLICK_POLICY_ROOT", nullptr);
        SetEnvironmentVariableW(L"SLICK_SHORTCUT_DIR", nullptr);
    }

    // Runs `command` through cmd.exe, in this process's environment. Its exit code.
    static int shell(const std::wstring& command) {
        std::wstring line = L"cmd.exe /d /s /c \"" + command + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
            return -1;
        }
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return static_cast<int>(code);
    }

    // Runs the script with `args`, typing `answers` when it asks. Its exit code.
    int run(const std::wstring& args, const std::string& answers = {}) const {
        std::wstring command = L"\"" + script_ + L"\" " + args;
        if (!answers.empty()) {
            const fs::path in = dir_ / "answers.txt";
            std::ofstream(in, std::ios::binary) << answers;
            command += L" < \"" + in.wstring() + L"\"";
        }
        return shell(command + L" > \"" + (dir_ / "output.txt").wstring() + L"\" 2>&1");
    }

    // What the last run printed.
    std::string output() const {
        std::ifstream in(dir_ / "output.txt", std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    std::wstring key(const wchar_t* vendor, const wchar_t* policy) const {
        return root_ + L"\\" + vendor + L"\\" + policy;
    }

    static bool exists(const std::wstring& key) {
        HKEY hkey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &hkey) != ERROR_SUCCESS) {
            return false;
        }
        RegCloseKey(hkey);
        return true;
    }

    static Entries entries(const std::wstring& key) {
        Entries out;
        HKEY hkey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &hkey) != ERROR_SUCCESS) {
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

    static void set(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
        RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(),
                        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }

    // Edge or Chrome here: the script adds no app without one.
    static bool has_browser() {
        for (const auto root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
            for (const wchar_t* exe : {L"msedge.exe", L"chrome.exe"}) {
                const std::wstring app = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + exe;
                if (RegGetValueW(root, app.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                    return true;
                }
            }
        }
        return false;
    }

    std::wstring root_ = L"Software\\SlickTraderTest-" + std::to_wstring(STAMP);
    fs::path dir_ = fs::temp_directory_path() / ("slick-trader-web-setup-" + std::to_string(STAMP));
    // "Desktop" in Chinese, from code points: MSVC reads this file in the ANSI code page.
    fs::path desktop_ = dir_ / std::wstring{wchar_t(0x684C), wchar_t(0x9762)};
    std::wstring script_ = fs::path(SLICK_TRADER_WEB_SETUP_SCRIPT).make_preferred().wstring();
    std::wstring edge_popups_ = key(L"Microsoft\\Edge", L"PopupsAllowedForUrls");
};

} // namespace

// WScript.Shell, which the script first saved shortcuts with, failed on a folder named like this one.
TEST_F(WebSetupScriptTest, ListsTheOriginInEachPolicyAndAddsTheApp) {
    set(edge_popups_, L"1", L"https://intranet");
    set(edge_popups_, L"3", L"http://other:80");
    ASSERT_EQ(run(L"10.0.0.9 3001"), 0) << output();
    EXPECT_EQ(entries(edge_popups_), (Entries{{L"1", L"https://intranet"}, {L"2", L"http://10.0.0.9:3001"},
                                              {L"3", L"http://other:80"}}));
    for (const auto& k : {key(L"Microsoft\\Edge", L"OverrideSecurityRestrictionsOnInsecureOrigin"),
                          key(L"Google\\Chrome", L"PopupsAllowedForUrls"),
                          key(L"Google\\Chrome", L"OverrideSecurityRestrictionsOnInsecureOrigin")}) {
        EXPECT_EQ(entries(k), (Entries{{L"1", L"http://10.0.0.9:3001"}}));
    }
    if (has_browser()) {
        EXPECT_TRUE(fs::exists(desktop_ / L"SlickTrader 10.0.0.9-3001.lnk")) << output();
    }

    // Ours goes, the rest is numbered again so none hides behind a gap, and empty keys go too.
    ASSERT_EQ(run(L"remove 10.0.0.9 3001"), 0) << output();
    EXPECT_EQ(entries(edge_popups_), (Entries{{L"1", L"https://intranet"}, {L"2", L"http://other:80"}}));
    EXPECT_FALSE(exists(key(L"Microsoft\\Edge", L"OverrideSecurityRestrictionsOnInsecureOrigin")));
    EXPECT_FALSE(exists(root_ + L"\\Google"));
    EXPECT_TRUE(fs::is_empty(desktop_));
}

// A double-click passes no arguments: it asks. It once took an empty address instead.
TEST_F(WebSetupScriptTest, WithNoArgumentsItAsks) {
    ASSERT_EQ(run(L"", "10.0.0.7\r\n\r\n\r\n"), 0) << output();
    EXPECT_EQ(entries(key(L"Google\\Chrome", L"PopupsAllowedForUrls")), (Entries{{L"1", L"http://10.0.0.7:3000"}}));
}

TEST_F(WebSetupScriptTest, WhatIsNoAddressAndPortWritesNothing) {
    EXPECT_EQ(run(L"host/x 3000"), 1) << output();
    EXPECT_EQ(run(L"10.0.0.1 70000"), 1) << output();
    EXPECT_FALSE(exists(root_));
    EXPECT_TRUE(fs::is_empty(desktop_));
}

#endif
