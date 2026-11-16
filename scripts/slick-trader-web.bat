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
#    The Desktop is wherever Windows keeps this user's, OneDrive included.
#
# Run it as the user who trades: the policies and shortcuts are the running user's.
#
#   slick-trader-web.bat                              asks for the address and port
#   slick-trader-web.bat <host> [port] [edge|chrome]  port 3000 and Edge unless given
#   slick-trader-web.bat remove <host> [port]         undoes it
#
# <host> is what you type in the browser: an IP or a host name. SLICK_POLICY_ROOT and
# SLICK_SHORTCUT_DIR move where it writes, for testing.

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
function Save-Shortcut([string]$lnk, [string]$target, [string]$arguments, [string]$description) {
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

    public static void Save(string lnk, string target, string arguments, string description) {
        var link = (IShellLinkW)new ShellLink();
        link.SetPath(target);
        link.SetArguments(arguments);
        link.SetWorkingDirectory(System.IO.Path.GetDirectoryName(target));
        link.SetDescription(description);
        ((IPersistFile)link).Save(lnk, true);
    }
}
'@
    }
    [SlickTraderShortcut]::Save($lnk, $target, $arguments, $description)
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
        foreach ($folder in $folders) {
            $lnk = Join-Path $folder "$name.lnk"
            try {
                Save-Shortcut $lnk $browser "--app=$origin/" "SlickTrader dashboard at $origin"
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
