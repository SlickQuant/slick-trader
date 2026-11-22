// scripts/slick-trader-web.bat, the setup for a browser on a PC other than the engine's, run as a
// user runs it. Its policy entries go under a throwaway HKCU key, its shortcuts into a temp folder
// named like a Chinese OneDrive Desktop, outside the ANSI code page, and the app's helper into
// another temp folder.

#include <gtest/gtest.h>

#ifdef _WIN32

#include <windows.h>
#include <propsys.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

using Entries = std::map<std::wstring, std::wstring>;

const auto STAMP = std::chrono::steady_clock::now().time_since_epoch().count();

class WebSetupScriptTest : public ::testing::Test {
protected:
    // The environment goes to the script through the OS, in UTF-16: _wputenv_s and _wsystem pass it
    // through a narrow copy, which handed the script the folder's name as mojibake.
    void SetUp() override {
        com_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        fs::create_directories(desktop_);
        SetEnvironmentVariableW(L"SLICK_POLICY_ROOT", (L"HKCU:\\" + root_).c_str());
        SetEnvironmentVariableW(L"SLICK_SHORTCUT_DIR", desktop_.c_str());
        SetEnvironmentVariableW(L"SLICK_APP_DIR", app_.c_str());
        // Never the real policies, Desktop or app folder: the script has to see where to write instead.
        ASSERT_EQ(shell(L"if defined SLICK_POLICY_ROOT (if exist \"%SLICK_SHORTCUT_DIR%\\\" (if defined SLICK_APP_DIR (exit 0) "
                        L"else (exit 3)) else (exit 3)) else (exit 3)"), 0);
    }

    void TearDown() override {
        RegDeleteTreeW(HKEY_CURRENT_USER, root_.c_str());
        std::error_code ignored;
        fs::remove_all(dir_, ignored);
        SetEnvironmentVariableW(L"SLICK_POLICY_ROOT", nullptr);
        SetEnvironmentVariableW(L"SLICK_SHORTCUT_DIR", nullptr);
        SetEnvironmentVariableW(L"SLICK_APP_DIR", nullptr);
        if (com_) {
            CoUninitialize();
        }
    }

    // Runs `command` through cmd.exe, in this process's environment. Its exit code.
    static int shell(const std::wstring& command) {
        return start(L"cmd.exe /d /s /c \"" + command + L"\"");
    }

    // Runs the command line `line` to its end. Its exit code.
    static int start(std::wstring line) {
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

    // A shortcut's target and arguments.
    static std::pair<std::wstring, std::wstring> shortcut(const fs::path& lnk) {
        std::pair<std::wstring, std::wstring> out;
        IShellLinkW* link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
            return out;
        }
        IPersistFile* file = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
            wchar_t text[1024];
            if (SUCCEEDED(file->Load(lnk.c_str(), STGM_READ))) {
                if (SUCCEEDED(link->GetPath(text, 1024, nullptr, SLGP_RAWPATH))) {
                    out.first = text;
                }
                if (SUCCEEDED(link->GetArguments(text, 1024))) {
                    out.second = text;
                }
            }
            file->Release();
        }
        link->Release();
        return out;
    }

    // A top-level window of Chromium's class, out of sight, with the taskbar id `app_id` unless null.
    static HWND window(const wchar_t* app_id) {
        static const bool registered = [] {
            WNDCLASSEXW cls{sizeof(cls)};
            cls.lpfnWndProc = DefWindowProcW;
            cls.hInstance = GetModuleHandleW(nullptr);
            cls.lpszClassName = L"Chrome_WidgetWin_1";
            return RegisterClassExW(&cls) != 0;
        }();
        if (!registered) {
            return nullptr;
        }
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"Chrome_WidgetWin_1", L"", WS_POPUP,
                                    -20000, -20000, 50, 50, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (hwnd && app_id) {
            IPropertyStore* store = nullptr;
            if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store)))) {
                // PKEY_AppUserModel_ID
                const PROPERTYKEY id{{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};
                PROPVARIANT value{};
                value.vt = VT_LPWSTR;
                const size_t bytes = (wcslen(app_id) + 1) * sizeof(wchar_t);
                value.pwszVal = static_cast<wchar_t*>(CoTaskMemAlloc(bytes));
                std::memcpy(value.pwszVal, app_id, bytes);
                store->SetValue(id, value);
                PropVariantClear(&value);
                store->Release();
            }
        }
        if (hwnd) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        return hwnd;
    }

    // Runs the app's helper for one pass that puts the dashboard opened at /app/<token>/ under
    // `front`. How many windows it moved. It asks this thread to move them: the asks are taken in
    // before it returns.
    int group(const std::wstring& token, HWND front) const {
        const int moved = start(L"\"" + app_exe_.wstring() + L"\" --group " + token + L" " +
                                std::to_wstring(reinterpret_cast<std::intptr_t>(front)));
        for (int i = 0; i < 20; ++i) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                DispatchMessageW(&msg);
            }
            Sleep(10);
        }
        return moved;
    }

    bool com_ = false;
    std::wstring root_ = L"Software\\SlickTraderTest-" + std::to_wstring(STAMP);
    fs::path dir_ = fs::temp_directory_path() / ("slick-trader-web-setup-" + std::to_string(STAMP));
    // "Desktop" in Chinese, from code points: MSVC reads this file in the ANSI code page.
    fs::path desktop_ = dir_ / std::wstring{wchar_t(0x684C), wchar_t(0x9762)};
    fs::path app_ = dir_ / "app";
    fs::path app_exe_ = app_ / "slick-trader-app.exe";
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
        // The app opens through the helper, which opens the browser on the dashboard.
        const auto [target, arguments] = shortcut(desktop_ / L"SlickTrader 10.0.0.9-3001.lnk");
        EXPECT_TRUE(fs::exists(app_exe_) && fs::equivalent(target, app_exe_)) << output();
        EXPECT_NE(arguments.find(L".exe\" http://10.0.0.9:3001"), std::wstring::npos) << output();
    }

    // Ours goes, the rest is numbered again so none hides behind a gap, and empty keys go too. So
    // does the helper, with no app left.
    ASSERT_EQ(run(L"remove 10.0.0.9 3001"), 0) << output();
    EXPECT_EQ(entries(edge_popups_), (Entries{{L"1", L"https://intranet"}, {L"2", L"http://other:80"}}));
    EXPECT_FALSE(exists(key(L"Microsoft\\Edge", L"OverrideSecurityRestrictionsOnInsecureOrigin")));
    EXPECT_FALSE(exists(root_ + L"\\Google"));
    EXPECT_TRUE(fs::is_empty(desktop_));
    EXPECT_FALSE(fs::exists(app_));
}

// A page can't raise the other windows when one comes to the front by its title bar or the
// taskbar: the helper does, from outside the browser. It finds the dashboard's windows by the
// taskbar id Chromium gives an --app window and its popups, made of the path it opened at.
// It took the host's windows at first, so two dashboards on one host, or two of its ports, came to
// the front together: each now opens at a path of its own. The ids are as Edge gives them, which
// keeps the first and last 10 characters of a long part.
TEST_F(WebSetupScriptTest, TheHelperKeepsTheDashboardWindowsTogether) {
    if (!has_browser()) {
        GTEST_SKIP() << "no Edge or Chrome: no app, no helper";
    }
    ASSERT_EQ(run(L"10.0.0.9 3001"), 0) << output();
    ASSERT_TRUE(fs::exists(app_exe_)) << output();

    HWND another_dashboard = window(L"MSEdge.10.0.0.9_//bbbbbbbb/");
    HWND first = window(L"MSEdge.10.0.0.9_//aaaaaaaa/");
    HWND untagged = window(nullptr);
    HWND second = window(L"MSEdge.10.0.0.9_//aaaaaaaa/.profileWbPZw.Default");   // off the default profile
    HWND minimized = window(L"MSEdge.10.0.0.9_//aaaaaaaa/");
    HWND front = window(L"Chrome.10.0.0.9_/app/aaaaaaaa/");   // Chrome cuts less
    const std::vector<HWND> all{another_dashboard, first, untagged, second, minimized, front};
    for (HWND hwnd : all) {
        ASSERT_NE(hwnd, nullptr);
    }
    ShowWindow(minimized, SW_SHOWMINNOACTIVE);
    for (size_t i = 1; i < all.size(); ++i) {
        SetWindowPos(all[i], all[i - 1], 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    EXPECT_EQ(group(L"aaaaaaaa", another_dashboard), 0) << "another dashboard's window moves none";
    EXPECT_EQ(group(L"aaaaaaaa", front), 2);
    EXPECT_EQ(GetWindow(front, GW_HWNDNEXT), first);
    EXPECT_EQ(GetWindow(first, GW_HWNDNEXT), second) << "in the order they were";
    EXPECT_TRUE(IsIconic(minimized));
    for (HWND hwnd : all) {
        DestroyWindow(hwnd);
    }
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
