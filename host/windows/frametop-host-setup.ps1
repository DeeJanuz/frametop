# Frametop host setup for Windows: makes this PC a host for Frametop's remote displays, its
# monitors shown as screens on a Steam Frame (docs/remote-displays.md). Run
# "Setup Frametop host.cmd"; it asks Windows for admin.
#
#  1. Vibepollo 2.0.0 (github.com/Nonary/Vibepollo), installed with its own installer if it
#     isn't here (checked against its SHA-256 first).
#  2. Frametop's build of Vibepollo's sunshine.exe (github.com/DeeJanuz/frametop-vibepollo):
#     it can stream any of your monitors (not only the main one), keeps your monitor layout
#     when a virtual display goes away, and doesn't stall the Web UI. Checked against its
#     SHA-256; the original is kept as sunshine.exe.2.0.0-original.
#  3. Settings Frametop needs (the rest of Vibepollo's settings stay as they are): remote
#     displays carry the PC's sound, and a virtual display goes away when Frametop
#     disconnects it, but stays through a dropped stream.
#  4. The Web UI login you sign in with once from the Steam Frame: keep the one there is, or
#     set one. The password is typed here and never shown or saved.
#  5. Checks: Vibepollo's firewall rule covers every network type (a Steam Link dongle's
#     network counts as Public), whether a Steam Link dongle is plugged in, and that the Web
#     UI answers.
#
# Options:
#   -FrametopBuild PATH|URL   where Frametop's sunshine.exe comes from (default: BuildUrl
#                             below, else sunshine-frametop.exe next to this script)
#   -SkipLogin                leave the Web UI login alone
#   -Check                    only say what would change
#   -Undo                     put back the original sunshine.exe and the settings from before
# A log goes to %ProgramData%\Frametop\host-setup.log, backups to %ProgramData%\Frametop\backup-*.
param([string]$FrametopBuild = "", [switch]$SkipLogin, [switch]$Check, [switch]$Undo)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$VibepolloVersion = "2.0.0"
$SetupUrl = "https://github.com/Nonary/Vibepollo/releases/download/2.0.0/VibepolloSetup-v2.0.0.exe"
$SetupSha = "7B3500EC0C774644CE5A435A48F61C046C48494D0F18B67AFA0B3561931794B7"
$OriginalSha = "2CC018FD92DDB4D3748D91D8DA25316909ED45DB3710D1FF278BD73D51EB00C1"  # its sunshine.exe
# Frametop's build: frametop-vibepollo branch frametop/2.0.0-remote-monitor-fix at 2f032252.
$BuildSha = "B5B7D2E7353454AEA6D895D0B68DE235E4CC581684C9DD44F8EFAF2E872F517F"
$BuildUrl = ""  # not published yet
$Settings = [ordered]@{
    "remote_monitor_mute_audio"                      = "disabled"
    "remote_monitor_disconnect_on_client_disconnect" = "enabled"
    "remote_monitor_disconnect_on_stream_end"        = "disabled"
}
$Service = "ApolloService"  # Vibepollo keeps Apollo's service name
$Data = Join-Path $env:ProgramData "Frametop"
$Here = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }

$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $pass = @()
    foreach ($k in $PSBoundParameters.Keys) {
        $v = $PSBoundParameters[$k]
        if ($v -is [switch]) { if ($v) { $pass += "-$k" } } else { $pass += "-$k `"$v`"" }
    }
    Start-Process powershell -Verb RunAs -ArgumentList ("-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" " + ($pass -join " "))
    exit
}
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
New-Item -ItemType Directory -Force $Data | Out-Null
$Log = Join-Path $Data "host-setup.log"

function Say([string]$text) {
    Write-Host $text
    Add-Content -Path $Log -Value ((Get-Date -Format "yyyy-MM-dd HH:mm:ss") + "  " + $text)
}

function Sha([string]$path) { (Get-FileHash $path -Algorithm SHA256).Hash }

function Plain($secure) {
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) } finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
}

# One argument for a Windows program, quoted so it reads back exactly this string
# (Windows PowerShell 5 mangles arguments that contain double quotes).
function Quote([string]$s) {
    $r = '"'
    $slashes = 0
    foreach ($c in $s.ToCharArray()) {
        if ($c -eq '\') { $slashes++; continue }
        if ($c -eq '"') { $r += ('\' * (2 * $slashes + 1)) + '"' } else { $r += ('\' * $slashes) + $c }
        $slashes = 0
    }
    $r + ('\' * (2 * $slashes)) + '"'
}

function Find-Vibepollo {
    $keys = "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*", "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*"
    $entry = Get-ItemProperty $keys -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -eq "Vibepollo" -and $_.InstallLocation } | Select-Object -First 1
    $dir = if ($entry) { $entry.InstallLocation.TrimEnd('\') } else { "C:\Program Files\Apollo" }
    if ((Test-Path "$dir\sunshine.exe") -and (Get-Service $Service -ErrorAction SilentlyContinue)) { return $dir }
    return $null
}

function Stop-Vibepollo {
    Stop-Service $Service
    for ($i = 0; $i -lt 30 -and (Get-Process sunshine -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 500 }
}

# sunshine.conf: "key = value" lines. Returns the lines with $Settings applied.
function Set-Settings([string[]]$lines) {
    $out = New-Object System.Collections.Generic.List[string]
    $done = @{}
    foreach ($line in $lines) {
        $key = ($line -split "=", 2)[0].Trim()
        if ($Settings.Contains($key)) {
            if (-not $done[$key]) { $out.Add("$key = $($Settings[$key])"); $done[$key] = $true }
        } else {
            $out.Add($line)
        }
    }
    foreach ($key in $Settings.Keys) { if (-not $done[$key]) { $out.Add("$key = $($Settings[$key])") } }
    return , $out.ToArray()
}

function Get-Build {
    $from = $FrametopBuild
    if (-not $from) { $from = if ($BuildUrl) { $BuildUrl } else { Join-Path $Here "sunshine-frametop.exe" } }
    if ($from -match "^https?://") {
        $file = Join-Path $env:TEMP "sunshine-frametop.exe"
        Say "Downloading Frametop's build of Vibepollo..."
        Invoke-WebRequest -UseBasicParsing -Uri $from -OutFile $file
    } else {
        $file = $from
    }
    if (-not (Test-Path $file)) { return $null }
    if ((Sha $file) -ne $BuildSha) { throw "$file isn't Frametop's build of Vibepollo $VibepolloVersion (its SHA-256 doesn't match)." }
    return $file
}

function Wait-WebUi {
    Add-Type @"
using System.Net;
using System.Security.Cryptography.X509Certificates;
public class FrametopTrustLocal : ICertificatePolicy {
    public bool CheckValidationResult(ServicePoint s, X509Certificate c, WebRequest r, int p) { return true; }
}
"@ -ErrorAction SilentlyContinue
    $old = [Net.ServicePointManager]::CertificatePolicy
    [Net.ServicePointManager]::CertificatePolicy = New-Object FrametopTrustLocal  # 127.0.0.1 only
    try {
        for ($i = 0; $i -lt 40; $i++) {
            try { Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 "https://127.0.0.1:47990/" | Out-Null; return $true } catch { Start-Sleep 1 }
        }
        return $false
    } finally {
        [Net.ServicePointManager]::CertificatePolicy = $old
    }
}

function Main {
    Say "== Frametop host setup$(if ($Check) { ' (check only)' })$(if ($Undo) { ' (undo)' })"
    $dir = Find-Vibepollo
    $exe = if ($dir) { "$dir\sunshine.exe" } else { $null }
    $orig = if ($dir) { "$dir\sunshine.exe.$VibepolloVersion-original" } else { $null }

    if ($Undo) {
        if (-not $dir) { throw "Vibepollo isn't installed here." }
        $backup = Get-ChildItem $Data -Directory -Filter "backup-*" -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -First 1
        Stop-Vibepollo
        if (Test-Path $orig) { Copy-Item $orig $exe -Force; Say "Put back the original sunshine.exe." }
        if ($backup -and (Test-Path "$($backup.FullName)\sunshine.conf")) {
            Copy-Item "$($backup.FullName)\sunshine.conf" "$dir\config\sunshine.conf" -Force
            Say "Put back the settings from $($backup.FullName)."
        }
        Start-Service $Service
        Say "Done. (The Web UI login stays as it is.)"
        return
    }

    # 1. Vibepollo
    if (-not $dir) {
        if ($Check) { Say "Would install Vibepollo $VibepolloVersion."; return }
        $setup = Join-Path $env:TEMP "VibepolloSetup-v$VibepolloVersion.exe"
        Say "Downloading Vibepollo $VibepolloVersion..."
        Invoke-WebRequest -UseBasicParsing -Uri $SetupUrl -OutFile $setup
        if ((Sha $setup) -ne $SetupSha) { throw "The Vibepollo installer didn't match its SHA-256; not running it." }
        Say "Running Vibepollo's installer: follow its steps, then come back here."
        Start-Process $setup -Wait
        $dir = Find-Vibepollo
        if (-not $dir) { throw "Vibepollo doesn't seem to be installed. Run this again once it is." }
        $exe = "$dir\sunshine.exe"
        $orig = "$dir\sunshine.exe.$VibepolloVersion-original"
    }
    Say "Vibepollo: $dir"

    # 2. Frametop's build of sunshine.exe
    $now = Sha $exe
    $swap = $null
    if ($now -eq $BuildSha) {
        Say "Frametop's build of Vibepollo is already installed."
    } elseif ($now -eq $OriginalSha) {
        $swap = Get-Build
        if (-not $swap) {
            Say "Frametop's build of Vibepollo isn't available here, so this PC can't stream its own monitors yet (virtual displays work)."
        }
    } else {
        $v = (Get-Item $exe).VersionInfo.ProductVersion
        throw "This is Vibepollo $v, and Frametop's build is for $VibepolloVersion. Install Vibepollo $VibepolloVersion ($SetupUrl), then run this again."
    }

    # 3. Settings
    $conf = "$dir\config\sunshine.conf"
    $lines = if (Test-Path $conf) { [IO.File]::ReadAllLines($conf) } else { @() }
    $new = Set-Settings $lines
    $changed = ($new -join "`n") -ne ($lines -join "`n")

    # 4. Login
    $state = "$dir\config\sunshine_state.json"
    $user = $null
    if (Test-Path $state) { try { $user = (Get-Content $state -Raw | ConvertFrom-Json).username } catch { } }
    $password = $null
    if (-not $SkipLogin -and -not $Check) {
        if ($user) {
            $answer = Read-Host "The Web UI login is '$user'. Keep it? You'll sign in with it once from the Steam Frame. (Y/n)"
            $setLogin = $answer -match "^[nN]"
        } else {
            Write-Host "Vibepollo has no Web UI login yet. Make one: you'll sign in with it once from the Steam Frame."
            $setLogin = $true
        }
        if ($setLogin) {
            $typed = Read-Host "User name$(if ($user) { " (Enter keeps '$user')" })"
            if ($typed) { $user = $typed }
            while (-not $user) { $user = Read-Host "User name" }
            while ($true) {
                $a = Plain (Read-Host "Password" -AsSecureString)
                $b = Plain (Read-Host "Same password again" -AsSecureString)
                if ($a.Length -ge 4 -and $a -ceq $b) { $password = $a; break }
                Write-Host "They don't match, or it's shorter than 4 characters. Try again."
            }
            Remove-Variable a, b
        }
    }

    if ($Check) {
        Say "Would $(if ($swap) { 'install' } else { 'not change' }) sunshine.exe, $(if ($changed) { 'change' } else { 'not change' }) the settings."
    } elseif ($swap -or $changed -or $password) {
        $backup = Join-Path $Data ("backup-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
        New-Item -ItemType Directory $backup | Out-Null
        foreach ($f in "$dir\config\sunshine.conf", "$dir\config\apps.json") { if (Test-Path $f) { Copy-Item $f $backup } }
        Say "Backed up the settings to $backup."
        Say "Stopping Vibepollo..."
        Stop-Vibepollo
        try {
            if ($swap) {
                if (-not (Test-Path $orig)) { Copy-Item $exe $orig }
                Copy-Item $swap $exe -Force
                Say "Installed Frametop's build of Vibepollo (the original is $orig)."
            }
            if ($changed) {
                [IO.File]::WriteAllLines($conf, [string[]]$new)
                Say "Set: $(($Settings.Keys | ForEach-Object { "$_ = $($Settings[$_])" }) -join ', ')."
            }
            if ($password) {
                $info = New-Object Diagnostics.ProcessStartInfo
                $info.FileName = $exe
                $info.WorkingDirectory = $dir
                $info.Arguments = "--creds " + (Quote $user) + " " + (Quote $password)
                $info.UseShellExecute = $false
                $info.RedirectStandardOutput = $true
                $info.RedirectStandardError = $true
                $proc = [Diagnostics.Process]::Start($info)
                $info.Arguments = ""
                $proc.StandardOutput.ReadToEnd() | Out-Null
                $proc.StandardError.ReadToEnd() | Out-Null
                $proc.WaitForExit()
                Remove-Variable password
                if ($proc.ExitCode -ne 0) { throw "Setting the Web UI login failed (sunshine.exe --creds: exit code $($proc.ExitCode))." }
                Say "Web UI login set: '$user'."
            }
        } finally {
            Start-Service $Service
            Say "Vibepollo is running again."
        }
    }

    # 5. Checks
    $rules = @(Get-NetFirewallApplicationFilter -ErrorAction SilentlyContinue |
        Where-Object { [Environment]::ExpandEnvironmentVariables($_.Program) -ieq $exe } |
        Get-NetFirewallRule | Where-Object { $_.Enabled -eq "True" -and $_.Direction -eq "Inbound" -and $_.Action -eq "Allow" })
    $everywhere = $rules | Where-Object { $_.Profile -eq "Any" -or ($_.Profile -match "Private" -and $_.Profile -match "Public") }
    if ($everywhere) {
        Say "Firewall: Vibepollo is allowed on every network type."
    } elseif ($Check) {
        Say "Would add a firewall rule letting Vibepollo in on every network type."
    } else {
        New-NetFirewallRule -DisplayName "Vibepollo (Frametop)" -Program $exe -Direction Inbound -Action Allow -Profile Any | Out-Null
        Say "Firewall: added a rule letting Vibepollo in on every network type (a Steam Link dongle's network is Public)."
    }
    $dongle = Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.InterfaceDescription -match "For Valve" } | Select-Object -First 1
    if ($dongle -and $dongle.Status -eq "Up") {
        $ip = (Get-NetIPAddress -InterfaceIndex $dongle.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Select-Object -First 1).IPAddress
        Say "Steam Link dongle: connected$(if ($ip) { " ($ip)" }). Frametop can stream over it, past your router."
    } elseif ($dongle) {
        Say "Steam Link dongle: plugged in, not connected to the Steam Frame. Frametop uses your network until it is."
    } else {
        Say "Steam Link dongle: none. Frametop streams over your network."
    }
    if (-not $Check) {
        if (Wait-WebUi) { Say "The Web UI answers (https://localhost:47990)." } else { Say "The Web UI didn't answer yet; check that Vibepollo is running." }
    }

    $addresses = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object {
        $_.IPAddress -notmatch "^(127\.|169\.254\.)" -and $_.InterfaceAlias -notmatch "Tailscale|vEthernet|Loopback" -and
        (-not $dongle -or $_.InterfaceIndex -ne $dongle.ifIndex) } | ForEach-Object { $_.IPAddress }
    Write-Host ""
    Say "Done. On the Steam Frame: open Frametop Remote Displays, Add computer, pick $([Net.Dns]::GetHostName()) ($($addresses -join ', ')), and sign in as '$user'."
}

try {
    Main
} catch {
    Say "Failed: $_"
    if (-not $Check -and (Get-Service $Service -ErrorAction SilentlyContinue) -and (Get-Service $Service).Status -ne "Running") {
        Start-Service $Service -ErrorAction SilentlyContinue
    }
}
if (-not $env:FRAMETOP_NO_PAUSE) { Read-Host "Press Enter to close" | Out-Null }
