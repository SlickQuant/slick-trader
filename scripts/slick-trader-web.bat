<# : The rest of this file is PowerShell. Run it on the PC with the browser.
@echo off
setlocal
set "ST_SELF=%~f0"
set "ST_ARGS=%*"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "iex ([IO.File]::ReadAllText($env:ST_SELF))"
set "ST_EXIT=%errorlevel%"
if "%~1"=="" pause
exit /b %ST_EXIT%
#>

# Sets this PC up for a SlickTrader dashboard that runs on another machine (the engine's own
# web_server_allow_popups and web_server_secure_origin do it on the engine's PC). For this user:
#  - lists http://<host>:<port> in the Edge and Chrome PopupsAllowedForUrls policies, so a
#    workspace opens all its windows at once, and OverrideSecurityRestrictionsOnInsecureOrigin
#    (the flag "Insecure origins treated as secure"), so it may place them on other screens;
#  - adds Desktop and Start menu shortcuts that open the dashboard as an app: no tabs or address bar.
#    The Desktop is wherever Windows keeps this user's, OneDrive included. They open it through
#    slick-trader-app.exe, built here into %LOCALAPPDATA%\SlickTrader, which keeps the dashboard's
#    windows together: one brought to the front, by its title bar or the taskbar too, brings the rest.
#
# Run it as the user who trades: the policies and shortcuts are the running user's. On the engine's
# own PC, run it with localhost for the app.
#
#   slick-trader-web.bat                              asks for the address and port
#   slick-trader-web.bat <host> [port] [edge|chrome]  port 3000 and Edge unless given
#   slick-trader-web.bat remove <host> [port]         undoes it
#
# <host> is what you type in the browser: an IP or a host name. SLICK_POLICY_ROOT,
# SLICK_SHORTCUT_DIR and SLICK_APP_DIR move where it writes, for testing.

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }   # paths in any language
$policies = if ($env:SLICK_POLICY_ROOT) { $env:SLICK_POLICY_ROOT } else { 'HKCU:\Software\Policies' }
$keys = foreach ($vendor in 'Microsoft\Edge', 'Google\Chrome') {
    foreach ($policy in 'PopupsAllowedForUrls', 'OverrideSecurityRestrictionsOnInsecureOrigin') {
        "$policies\$vendor\$policy"
    }
}
$folders = if ($env:SLICK_SHORTCUT_DIR) { @($env:SLICK_SHORTCUT_DIR) }
           else { @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('Programs')) }
$appDir = if ($env:SLICK_APP_DIR) { $env:SLICK_APP_DIR } else { Join-Path $env:LOCALAPPDATA 'SlickTrader' }
$appExe = Join-Path $appDir 'slick-trader-app.exe'

# "http://<host>:<port>", an IPv6 address in brackets. $null unless both are usable.
function Get-Origin([string]$hostName, [string]$port) {
    $p = 0
    if (-not [int]::TryParse($port, [ref]$p) -or $p -lt 1 -or $p -gt 65535) {
        return $null
    }
    if ($hostName.Contains(':')) {
        if ($hostName -match '^[0-9a-f:.]+$') { return "http://[${hostName}]:$p" }
        return $null
    }
    if ($hostName -match '^[a-z0-9.-]{1,253}$') { return "http://${hostName}:$p" }
    return $null
}

# The URL list at $key as browsers read it: entries 1, 2, ... up to the first number missing.
function Get-Entries([string]$key) {
    $list = @()
    if (Test-Path $key) {
        $values = Get-ItemProperty -Path $key
        for ($n = 1; $null -ne $values.PSObject.Properties["$n"]; ++$n) {
            $list += [string]$values."$n"
        }
    }
    return ,$list
}

# Adds $url after the entries read, unless one of them is $url. Other entries stay as they are.
function Add-Entry([string]$key, [string]$url) {
    $list = Get-Entries $key
    if ($list -contains $url) {
        return 'listed'
    }
    if (-not (Test-Path $key)) {
        New-Item -Path $key -Force | Out-Null
    }
    New-ItemProperty -Path $key -Name "$($list.Count + 1)" -Value $url -PropertyType String -Force | Out-Null
    return 'added'
}

# Drops $url and numbers the rest again: a gap would hide the entries after it. A key left empty
# goes, and so do its parents up to the policies root.
function Remove-Entry([string]$key, [string]$url) {
    $list = Get-Entries $key
    if ($list -notcontains $url) {
        return 'absent'
    }
    $kept = @($list | Where-Object { $_ -ne $url })
    for ($n = 1; $n -le $list.Count; ++$n) {
        Remove-ItemProperty -Path $key -Name "$n"
    }
    for ($n = 1; $n -le $kept.Count; ++$n) {
        New-ItemProperty -Path $key -Name "$n" -Value $kept[$n - 1] -PropertyType String | Out-Null
    }
    for ($k = $key; $k -ne $policies -and (Test-Path $k); $k = Split-Path $k) {
        $item = Get-Item $k
        if ($item.SubKeyCount -gt 0 -or $item.ValueCount -gt 0) {
            break
        }
        Remove-Item $k
    }
    return 'removed'
}

# Saves a shortcut through IShellLinkW. WScript.Shell turns the path into the ANSI code page and
# fails on a folder named outside it, as a OneDrive Desktop in another language can be.
function Save-Shortcut([string]$lnk, [string]$target, [string]$arguments, [string]$description, [string]$icon) {
    if (-not ('SlickTraderShortcut' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Text;

public static class SlickTraderShortcut {
    [ComImport, Guid("00021401-0000-0000-C000-000000000046")]
    private class ShellLink {}

    [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("000214F9-0000-0000-C000-000000000046")]
    private interface IShellLinkW {
        void GetPath([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder file, int max, IntPtr data, int flags);
        void GetIDList(out IntPtr idList);
        void SetIDList(IntPtr idList);
        void GetDescription([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder name, int max);
        void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string name);
        void GetWorkingDirectory([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder dir, int max);
        void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string dir);
        void GetArguments([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder args, int max);
        void SetArguments([MarshalAs(UnmanagedType.LPWStr)] string args);
        void GetHotkey(out short hotkey);
        void SetHotkey(short hotkey);
        void GetShowCmd(out int showCmd);
        void SetShowCmd(int showCmd);
        void GetIconLocation([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder path, int max, out int index);
        void SetIconLocation([MarshalAs(UnmanagedType.LPWStr)] string path, int index);
        void SetRelativePath([MarshalAs(UnmanagedType.LPWStr)] string path, int reserved);
        void Resolve(IntPtr hwnd, int flags);
        void SetPath([MarshalAs(UnmanagedType.LPWStr)] string file);
    }

    public static void Save(string lnk, string target, string arguments, string description, string icon) {
        var link = (IShellLinkW)new ShellLink();
        link.SetPath(target);
        link.SetArguments(arguments);
        link.SetWorkingDirectory(System.IO.Path.GetDirectoryName(target));
        link.SetDescription(description);
        link.SetIconLocation(icon, 0);
        ((IPersistFile)link).Save(lnk, true);
    }
}
'@
    }
    [SlickTraderShortcut]::Save($lnk, $target, $arguments, $description, $icon)
}

# slick-trader-app.exe. A page can't keep the windows together: a browser lets it raise a window
# only on a click in it, never on one on the title bar or the taskbar.
$appSource = @'
// Opens a SlickTrader dashboard as an app and keeps its windows together while any is open. When
// one comes to the front, by a click, its title bar, the taskbar or Alt+Tab, the rest go right
// under it, in their order, without taking focus. Each run is a dashboard of its own.
//   slick-trader-app.exe <browser> <origin> [browser args]
//   slick-trader-app.exe --group <token> <hwnd>   one pass under <hwnd>; exits with how many moved
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

public static class SlickTraderApp {
    const uint EVENT_SYSTEM_FOREGROUND = 3, WINEVENT_SKIPOWNPROCESS = 2;
    const uint SWP_NOSIZE = 0x1, SWP_NOMOVE = 0x2, SWP_NOACTIVATE = 0x10, SWP_NOOWNERZORDER = 0x200,
               SWP_ASYNCWINDOWPOS = 0x4000;
    const double FIRST_WINDOW_SECONDS = 60;   // how long a browser gets to show the dashboard

    delegate void WinEventProc(IntPtr hook, uint e, IntPtr hwnd, int obj, int child, uint thread, uint time);
    delegate bool EnumProc(IntPtr hwnd, IntPtr param);
    delegate void TimerProc(IntPtr hwnd, uint msg, UIntPtr id, uint time);

    [StructLayout(LayoutKind.Sequential)]
    struct Msg { public IntPtr hwnd; public uint message; public UIntPtr wParam; public IntPtr lParam; public uint time; public int x, y; }
    [StructLayout(LayoutKind.Sequential)]
    struct PropertyKey { public Guid fmtid; public uint pid; }
    [StructLayout(LayoutKind.Sequential)]
    struct PropVariant { public ushort vt, r1, r2, r3; public IntPtr value, more; }

    [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPropertyStore {
        void GetCount(out uint count);
        void GetAt(uint index, out PropertyKey key);
        void GetValue(ref PropertyKey key, out PropVariant value);
    }

    [DllImport("user32.dll")]
    static extern IntPtr SetWinEventHook(uint min, uint max, IntPtr module, WinEventProc proc, uint pid, uint thread, uint flags);
    [DllImport("user32.dll")]
    static extern bool EnumWindows(EnumProc proc, IntPtr param);
    [DllImport("user32.dll")]
    static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")]
    static extern bool IsIconic(IntPtr hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetClassName(IntPtr hwnd, StringBuilder name, int size);
    [DllImport("user32.dll")]
    static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")]
    static extern UIntPtr SetTimer(IntPtr hwnd, UIntPtr id, uint ms, TimerProc proc);
    [DllImport("user32.dll")]
    static extern int GetMessage(out Msg msg, IntPtr hwnd, uint min, uint max);
    [DllImport("user32.dll")]
    static extern IntPtr DispatchMessage(ref Msg msg);
    [DllImport("user32.dll")]
    static extern void PostQuitMessage(int code);
    [DllImport("shell32.dll")]
    static extern int SHGetPropertyStoreForWindow(IntPtr hwnd, ref Guid iid, [MarshalAs(UnmanagedType.Interface)] out IPropertyStore store);
    [DllImport("ole32.dll")]
    static extern int PropVariantClear(ref PropVariant value);

    // This dashboard's, the last part of the path it opens at. Edge and Chrome give an app's windows,
    // the popups it opens too, a taskbar id made of the host, not the port, and that path. Edge
    // keeps only its first and last 10 characters, so the token is 8, between slashes at the end.
    // The engine sends the path on to its dashboard.
    static string token;
    static readonly WinEventProc onForeground = OnForeground;   // held while Windows calls them
    static readonly TimerProc onTimer = OnTimer;
    static readonly DateTime started = DateTime.UtcNow;
    static bool seen;

    [STAThread]
    public static int Main(string[] args) {
        try {
            if (args.Length == 3 && args[0] == "--group") {
                token = args[1];
                return Group(new IntPtr(long.Parse(args[2])));
            }
            if (args.Length < 2) {
                return 2;
            }
            token = Guid.NewGuid().ToString("N").Substring(0, 8);
            var line = new StringBuilder("--app=" + args[1].TrimEnd('/') + "/app/" + token + "/");
            for (int i = 2; i < args.Length; ++i) {
                line.Append(" \"").Append(args[i]).Append('"');
            }
            Process.Start(args[0], line.ToString());
            SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, IntPtr.Zero, onForeground, 0, 0,
                            WINEVENT_SKIPOWNPROCESS);
            SetTimer(IntPtr.Zero, UIntPtr.Zero, 2000, onTimer);
            Msg msg;
            while (GetMessage(out msg, IntPtr.Zero, 0, 0) > 0) {
                DispatchMessage(ref msg);
            }
            return 0;
        } catch {
            return 1;
        }
    }

    static string AppId(IntPtr hwnd) {
        IPropertyStore store;
        Guid iid = typeof(IPropertyStore).GUID;
        if (SHGetPropertyStoreForWindow(hwnd, ref iid, out store) != 0 || store == null) {
            return "";
        }
        try {
            var key = new PropertyKey { fmtid = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"), pid = 5 };   // PKEY_AppUserModel_ID
            PropVariant value;
            store.GetValue(ref key, out value);
            string id = value.vt == 31 ? Marshal.PtrToStringUni(value.value) : "";   // VT_LPWSTR
            PropVariantClear(ref value);
            return id;
        } catch {
            return "";
        } finally {
            Marshal.ReleaseComObject(store);
        }
    }

    static bool Ours(IntPtr hwnd) {
        if (!IsWindowVisible(hwnd)) {
            return false;
        }
        var name = new StringBuilder(32);
        if (GetClassName(hwnd, name, name.Capacity) == 0 || name.ToString() != "Chrome_WidgetWin_1") {
            return false;
        }
        return AppId(hwnd).IndexOf("/" + token + "/", StringComparison.Ordinal) >= 0;
    }

    // The dashboard's other windows go right under `front`, minimized ones aside. None is activated.
    static int Group(IntPtr front) {
        if (!Ours(front)) {
            return 0;
        }
        var rest = new List<IntPtr>();
        EnumWindows(delegate(IntPtr hwnd, IntPtr unused) {
            if (hwnd != front && !IsIconic(hwnd) && Ours(hwnd)) {
                rest.Add(hwnd);
            }
            return true;
        }, IntPtr.Zero);
        IntPtr after = front;
        foreach (IntPtr hwnd in rest) {
            SetWindowPos(hwnd, after, 0, 0, 0, 0,
                         SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
            after = hwnd;
        }
        return rest.Count;
    }

    static void OnForeground(IntPtr hook, uint e, IntPtr hwnd, int obj, int child, uint thread, uint time) {
        try {
            Group(hwnd);
        } catch {
            // a window gone meanwhile
        }
    }

    // It goes with the dashboard's last window, or when the browser shows none.
    static void OnTimer(IntPtr hwnd, uint msg, UIntPtr id, uint time) {
        bool open = false;
        EnumWindows(delegate(IntPtr w, IntPtr unused) {
            open = Ours(w);
            return !open;
        }, IntPtr.Zero);
        if (open) {
            seen = true;
        } else if (seen || (DateTime.UtcNow - started).TotalSeconds > FIRST_WINDOW_SECONDS) {
            PostQuitMessage(0);
        }
    }
}
'@

# Builds slick-trader-app.exe. One running can't be replaced, but can be moved aside.
function Save-App {
    New-Item -ItemType Directory -Force -Path $appDir | Out-Null
    $built = Join-Path ([IO.Path]::GetTempPath()) "slick-trader-app-$PID.exe"
    Add-Type -TypeDefinition $appSource -OutputAssembly $built -OutputType WindowsApplication
    Get-ChildItem -LiteralPath $appDir -Filter 'slick-trader-app.*.old' | Remove-Item -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $appExe) {
        try {
            Remove-Item -LiteralPath $appExe
        } catch {
            Rename-Item -LiteralPath $appExe "slick-trader-app.$([DateTime]::Now.Ticks).old"
        }
    }
    Move-Item -LiteralPath $built $appExe
}

function Find-Browser([string]$name) {
    $exe = @{ edge = 'msedge.exe'; chrome = 'chrome.exe' }[$name]
    foreach ($root in 'HKCU:', 'HKLM:') {
        $appPath = "$root\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\$exe"
        if ($exe -and (Test-Path $appPath)) {
            $path = "$((Get-ItemProperty $appPath).'(default)')".Trim('"')
            if ($path -and (Test-Path $path)) {
                return $path
            }
        }
    }
    return $null
}

try {
    $argv = @(-split ("$env:ST_ARGS" -replace '"', '') | Where-Object { $_ })   # -split '' gives one ''
    $remove = $argv.Count -gt 0 -and $argv[0] -eq 'remove'
    if ($remove) {
        $argv = @($argv | Select-Object -Skip 1)
    }
    if ($argv.Count -gt 0) {
        $hostName = $argv[0]
        $port = if ($argv.Count -gt 1) { $argv[1] } else { '3000' }
    } else {
        $hostName = (Read-Host 'SlickTrader address, as typed in the browser (IP or host name)').Trim()
        $port = (Read-Host 'Port [3000]').Trim()
        if (-not $port) { $port = '3000' }
    }
    $browserName = if ($argv.Count -gt 2) { $argv[2].ToLower() } else { 'edge' }
    $origin = Get-Origin $hostName $port
    if (-not $origin) {
        Write-Host "Not an address and port: '$hostName' '$port'"
        exit 1
    }
    $name = 'SlickTrader ' + (($origin -replace '^http://', '') -replace '[\[\]:]+', '-').Trim('-')

    if ($remove) {
        foreach ($key in $keys) {
            Write-Host ('{0,-8} {1} in {2}' -f (Remove-Entry $key $origin), $origin, $key)
        }
        foreach ($folder in $folders) {
            $lnk = Join-Path $folder "$name.lnk"
            if (Test-Path -LiteralPath $lnk) {
                Remove-Item -LiteralPath $lnk
                Write-Host "removed  $lnk"
            }
        }
        # The helper goes with the last app, unless it is running: then it stays till next time.
        $apps = @($folders | Where-Object { Test-Path -LiteralPath $_ } |
                  ForEach-Object { Get-ChildItem -LiteralPath $_ -Filter 'SlickTrader *.lnk' })
        if ($apps.Count -eq 0 -and (Test-Path -LiteralPath $appDir)) {
            Get-ChildItem -LiteralPath $appDir -Filter 'slick-trader-app*' | Remove-Item -ErrorAction SilentlyContinue
            if (Test-Path -LiteralPath $appExe) {
                Write-Host "in use   $appExe"
            } else {
                Write-Host "removed  $appExe"
                if (-not (Get-ChildItem -LiteralPath $appDir)) {
                    Remove-Item -LiteralPath $appDir
                }
            }
        }
        Write-Host 'Restart Edge and Chrome, or Reload policies at edge://policy and chrome://policy.'
        exit 0
    }

    foreach ($key in $keys) {
        Write-Host ('{0,-8} {1} in {2}' -f (Add-Entry $key $origin), $origin, $key)
    }
    $browser = Find-Browser $browserName
    if (-not $browser -and $browserName -eq 'edge') {
        $browser = Find-Browser 'chrome'
    }
    $failed = $false
    if ($browser) {
        # Without the helper the app still opens, its windows just come to the front one by one.
        $target = $appExe
        $arguments = "`"$browser`" $origin"
        try {
            Save-App
            Write-Host "helper   $appExe"
        } catch {
            Write-Host "no helper $appExe : $($_.Exception.Message)"
            $target = $browser
            $arguments = "--app=$origin/"
            $failed = $true
        }
        foreach ($folder in $folders) {
            $lnk = Join-Path $folder "$name.lnk"
            try {
                Save-Shortcut $lnk $target $arguments "SlickTrader dashboard at $origin" $browser
                Write-Host "app      $lnk"
            } catch {
                Write-Host "no app   $lnk : $($_.Exception.Message)"
                $failed = $true
            }
        }
    } else {
        Write-Host "No $browserName here: no app shortcut. Open $origin in a browser."
    }
    Write-Host "Restart Edge and Chrome (or Reload policies at edge://policy), then open '$name'."
    exit [int]$failed
} catch {
    Write-Host "Failed: $($_.Exception.Message)"
    exit 1
}
