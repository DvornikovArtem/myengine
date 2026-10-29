<#
.SYNOPSIS
    Runs dist/RocketArena from a clean folder outside the repository and checks that the package stands alone.

.DESCRIPTION
    Copies the package to %TEMP%\rocket_clean, starts RocketArena.exe from another working directory with its own
    LOCALAPPDATA / APPDATA (so the real ones stay untouched and can be inspected), lets it run for -Frames frames and checks:
      - the exit code is 0 and the process ends on its own;
      - nothing is created or changed next to the exe;
      - the log is in <LOCALAPPDATA>\RocketArena\logs and names no path of the repository;
      - the log has no error line;
      - the sound: with Wwise (version.txt says "wwise yes") the log has "Audio: Wwise", "Wwise banks from ...: N events" (N is the number of the events of
        audio_bank.json in the package), no line about a bank, and the package has the banks, no WAV file, no file of the SDK and the licenses of the codecs;
        without Wwise the log has the sound bank of XAudio2;
      - nothing is written to APPDATA;
      - the exe has its icon and its version info (the version of the project, "Rocket Arena");
      - a deliberate crash (MYENGINE_CRASH_TEST=1) leaves a minidump and a report in <LOCALAPPDATA>\RocketArena\crashes, and the next
        start of the game tells about it (the log line "the previous run crashed");
      - a package with Steam has steam_api64.dll and steam_appid.txt, and a start where Steam cannot start (no steam_appid.txt) still
        plays: the game runs and the log says why Steam is not there.
    With -WithoutScripting it runs once more without python314.zip (the game must work without scripting).
    docs/game/packaging.md describes the checks.

.PARAMETER Dist
    The folder of the package (relative to the repository root, or a full path: the zip unpacked somewhere).
.PARAMETER Frames
    --exit-after-frames for the run.
.PARAMETER WithoutScripting
    Also run without python314.zip.
.PARAMETER Keep
    Keep %TEMP%\rocket_clean after the checks.
#>
[CmdletBinding()]
param(
    [string]$Dist = 'dist\RocketArena',
    [int]$Frames = 600,
    [switch]$WithoutScripting,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$source = if ([System.IO.Path]::IsPathRooted($Dist)) { [System.IO.Path]::GetFullPath($Dist) } else { [System.IO.Path]::GetFullPath((Join-Path $root $Dist)) }
$work = Join-Path $env:TEMP 'rocket_clean'
$failures = New-Object 'System.Collections.Generic.List[string]'

function Write-Step([string]$text) { Write-Host "[CHECK] $text" }
function Fail([string]$text) { $failures.Add($text); Write-Host "[CHECK] FAILED: $text" -ForegroundColor Red }

function Get-Snapshot([string]$folder) {
    $prefix = $folder.TrimEnd('\') + '\'
    $result = @{}
    Get-ChildItem -Path $folder -Recurse -File -Force | ForEach-Object {
        $result[$_.FullName.Substring($prefix.Length)] = '{0}|{1}' -f $_.Length, $_.LastWriteTimeUtc.Ticks
    }
    return $result
}

# the sound of a package: version.txt says whether it is Wwise
function Test-WwisePackage([string]$folder) {
    $versionFile = Join-Path $folder 'version.txt'
    return (Test-Path $versionFile) -and ([System.IO.File]::ReadAllText($versionFile) -match '(?m)^wwise yes\s*$')
}

function Get-EventCount([string]$folder) {
    $bank = Get-Content -Raw -Encoding UTF8 (Join-Path $folder 'assets\data\audio_bank.json') | ConvertFrom-Json
    return @($bank.events.PSObject.Properties).Count
}

function Test-WwiseFiles([string]$folder) {
    foreach ($bank in @('Init.bnk', 'RocketArena.bnk')) {
        if (-not (Test-Path (Join-Path $folder "assets\audio\wwise\$bank"))) { Fail "no bank assets\audio\wwise\$bank" }
    }
    $wavs = @(Get-ChildItem -Path $folder -Recurse -File -Filter '*.wav')
    if ($wavs.Count -gt 0) { Fail "a Wwise package has WAV files, the first: $($wavs[0].Name)" }
    $sdk = @(Get-ChildItem -Path $folder -Recurse -File | Where-Object { $_.Extension -in @('.lib', '.h', '.hpp') -or $_.Name -like 'Ak*.dll' -or $_.Name -like 'Wwise*.dll' })
    if ($sdk.Count -gt 0) { Fail "a file of the Wwise SDK is in the package: $($sdk[0].Name)" }
    foreach ($name in @('Ogg-Vorbis-Xiph-LICENSE.txt', 'Opus-LICENSE.txt', 'OpusFile-LICENSE.txt', 'THIRD_PARTY_NOTICES.txt')) {
        if (-not (Test-Path (Join-Path $folder "licenses\$name"))) { Fail "no licenses\$name" }
    }
    $notices = Join-Path $folder 'licenses\THIRD_PARTY_NOTICES.txt'
    if ((Test-Path $notices) -and ([System.IO.File]::ReadAllText($notices).IndexOf('Audiokinetic') -lt 0)) { Fail 'licenses\THIRD_PARTY_NOTICES.txt does not name Audiokinetic' }
    $size = @(Get-ChildItem -Path (Join-Path $folder 'assets\audio\wwise') -File | Measure-Object Length -Sum)[0].Sum
    Write-Step ("Wwise: the banks {0:N1} MB, {1} events in audio_bank.json" -f ($size / 1MB), (Get-EventCount $folder))
}

function Invoke-Run([string]$label, [string]$folder, [string]$expectedError = '', [string[]]$expectedLog = @(), [hashtable]$environment = @{}) {
    $appData = Join-Path $work "appdata-$label"
    $localAppData = Join-Path $work "localappdata-$label"
    $workingDirectory = Join-Path $work "cwd-$label"
    New-Item -ItemType Directory -Force -Path $appData, $localAppData, $workingDirectory | Out-Null

    $before = Get-Snapshot $folder
    $savedAppData = $env:APPDATA
    $savedLocalAppData = $env:LOCALAPPDATA
    $env:APPDATA = $appData
    $env:LOCALAPPDATA = $localAppData
    foreach ($name in $environment.Keys) { Set-Item -Path "Env:$name" -Value $environment[$name] }
    try {
        # a run with a UI script ends by the quit of the script (the frames would end it before the script reaches its screen)
        $arguments = if ($environment.ContainsKey('MYENGINE_UI_SCRIPT')) { @('--game') } else { @('--exit-after-frames', "$Frames") }
        Write-Step "[$label] running RocketArena.exe $($arguments -join ' ')"
        $process = Start-Process -FilePath (Join-Path $folder 'RocketArena.exe') -ArgumentList $arguments `
            -WorkingDirectory $workingDirectory -PassThru
    }
    finally {
        $env:APPDATA = $savedAppData
        $env:LOCALAPPDATA = $savedLocalAppData
        foreach ($name in $environment.Keys) { Remove-Item -Path "Env:$name" -ErrorAction SilentlyContinue }
    }

    if (-not $process.WaitForExit(180000)) {
        $process.Kill()
        Fail "[$label] the game did not finish within 180 s"
    }
    elseif ($process.ExitCode -ne 0) {
        Fail "[$label] exit code $($process.ExitCode)"
    }
    else {
        Write-Step "[$label] exit code 0"
    }

    # nothing next to the exe
    $after = Get-Snapshot $folder
    foreach ($name in $after.Keys) {
        if (-not $before.ContainsKey($name)) { Fail "[$label] a file appeared next to the exe: $name" }
        elseif ($before[$name] -ne $after[$name]) { Fail "[$label] a file next to the exe changed: $name" }
    }
    foreach ($name in $before.Keys) {
        if (-not $after.ContainsKey($name)) { Fail "[$label] a file next to the exe disappeared: $name" }
    }

    # the files of the player are in LOCALAPPDATA\RocketArena, nothing in APPDATA
    $userData = Join-Path $localAppData 'RocketArena'
    $log = Join-Path $userData 'logs\myengine.log'
    if (-not (Test-Path $log)) {
        Fail "[$label] no log in $userData\logs"
    }
    else {
        $text = [System.IO.File]::ReadAllText($log)
        $repositoryMarkers = @($root, 'Documents\GitHub', '\.team\', 'build-shipping')
        foreach ($marker in $repositoryMarkers) {
            if ($text.IndexOf($marker, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) { Fail "[$label] the log names the repository ('$marker')" }
        }
        $errors = @($text -split "`n" | Where-Object { $_ -match '\[(ERROR|FATAL)\]' -and ($expectedError -eq '' -or $_ -notmatch $expectedError) })
        if ($errors.Count -gt 0) { Fail "[$label] the log has $($errors.Count) error line(s), the first: $($errors[0].Trim())" }
        $wwise = Test-WwisePackage $folder
        $soundExpected = if ($wwise) { @('Audio: Wwise', 'Wwise banks from', ": $(Get-EventCount $folder) events") } else { @('sound bank') }
        if ($wwise) {
            $bankLines = @($text -split "`n" | Where-Object { $_ -match '(bank|Wwise)' -and $_ -match '\[(WARN|WARNING|ERROR|FATAL)\]' })
            if ($bankLines.Count -gt 0) { Fail "[$label] the log has a warning about the sound: $($bankLines[0].Trim())" }
        }
        foreach ($expected in (@('Particle effects loaded') + $soundExpected + $expectedLog)) {
            if ($text.IndexOf($expected, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) { Fail "[$label] the log has no '$expected'" }
        }
        Write-Step "[$label] log: $log ($([math]::Round((Get-Item $log).Length / 1KB, 1)) KB)"
    }
    $written = @(Get-ChildItem -Path $appData -Recurse -File -ErrorAction SilentlyContinue)
    if ($written.Count -gt 0) { Fail "[$label] files were written to APPDATA: $($written[0].FullName)" }
    $userFiles = @(Get-ChildItem -Path $userData -Recurse -File -ErrorAction SilentlyContinue)
    Write-Step "[$label] files in %LOCALAPPDATA%\RocketArena: $($userFiles.Count) ($([math]::Round(($userFiles | Measure-Object Length -Sum).Sum / 1MB, 1)) MB)"
}

# the icon and the version info of the exe
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class RocketArenaResources
{
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)] static extern IntPtr LoadLibraryExW(string file, IntPtr handle, uint flags);
    [DllImport("kernel32", SetLastError = true)] static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32")] static extern bool FreeLibrary(IntPtr module);
    // RT_GROUP_ICON (14), the first icon (id 1) of the exe
    public static bool HasIcon(string path)
    {
        IntPtr module = LoadLibraryExW(path, IntPtr.Zero, 0x2);
        if (module == IntPtr.Zero) return false;
        bool found = FindResourceW(module, (IntPtr)1, (IntPtr)14) != IntPtr.Zero;
        FreeLibrary(module);
        return found;
    }
}
'@

function Test-ExeResources([string]$folder) {
    $exePath = Join-Path $folder 'RocketArena.exe'
    $info = (Get-Item $exePath).VersionInfo
    $expectedVersion = ((Get-Content -Encoding UTF8 -TotalCount 1 (Join-Path $folder 'version.txt')) -split ' ')[-1]
    if ($info.ProductName -ne 'Rocket Arena') { Fail "the exe has ProductName '$($info.ProductName)', expected 'Rocket Arena'" }
    if ($info.OriginalFilename -ne 'RocketArena.exe') { Fail "the exe has OriginalFilename '$($info.OriginalFilename)'" }
    if (-not $info.FileDescription) { Fail 'the exe has no FileDescription' }
    if ($info.FileVersion -ne $expectedVersion) { Fail "the exe says version $($info.FileVersion), version.txt says $expectedVersion" }
    if (-not [RocketArenaResources]::HasIcon($exePath)) { Fail 'the exe has no icon (RT_GROUP_ICON 1)' }
    Write-Step "exe: $($info.ProductName) $($info.ProductVersion), $($info.FileDescription), icon $([RocketArenaResources]::HasIcon($exePath))"
}

# A deliberate crash leaves the report. `mode` is MYENGINE_CRASH_TEST: 1 crashes at the very start, 2 crashes 10 s after the first screen of the game
# (with the UI script: the Steam screen has been opened, the Steam library has started and may have taken the handlers over)
function Invoke-CrashRun([string]$label, [string]$folder, [string]$mode, [hashtable]$environment = @{}) {
    $appData = Join-Path $work "appdata-$label"
    $localAppData = Join-Path $work "localappdata-$label"
    $workingDirectory = Join-Path $work "cwd-$label"
    New-Item -ItemType Directory -Force -Path $appData, $localAppData, $workingDirectory | Out-Null
    $saved = @{ APPDATA = $env:APPDATA; LOCALAPPDATA = $env:LOCALAPPDATA }
    $env:APPDATA = $appData; $env:LOCALAPPDATA = $localAppData
    $env:MYENGINE_CRASH_TEST = $mode; $env:MYENGINE_CRASH_NO_DIALOG = '1'
    foreach ($name in $environment.Keys) { Set-Item -Path "Env:$name" -Value $environment[$name] }
    try {
        Write-Step "[$label] running RocketArena.exe with MYENGINE_CRASH_TEST=$mode"
        $process = Start-Process -FilePath (Join-Path $folder 'RocketArena.exe') -WorkingDirectory $workingDirectory -PassThru
    }
    finally {
        $env:APPDATA = $saved.APPDATA; $env:LOCALAPPDATA = $saved.LOCALAPPDATA
        Remove-Item Env:MYENGINE_CRASH_TEST, Env:MYENGINE_CRASH_NO_DIALOG -ErrorAction SilentlyContinue
        foreach ($name in $environment.Keys) { Remove-Item -Path "Env:$name" -ErrorAction SilentlyContinue }
    }
    if (-not $process.WaitForExit(90000)) { $process.Kill(); Fail "[$label] the crashed game did not end"; return $null }
    if ($process.ExitCode -ne -1073741819) { Fail "[$label] exit code $($process.ExitCode), expected 0xC0000005 (-1073741819)" }
    $crashes = Join-Path $localAppData 'RocketArena\crashes'
    $dump = @(Get-ChildItem -Path $crashes -Filter 'crash-*.dmp' -ErrorAction SilentlyContinue)
    $text = @(Get-ChildItem -Path $crashes -Filter 'crash-*.txt' -ErrorAction SilentlyContinue)
    if ($dump.Count -ne 1 -or $dump[0].Length -lt 10000) { Fail "[$label] the minidump is missing or too small in $crashes" }
    elseif ($text.Count -ne 1) { Fail "[$label] the report .txt is missing in $crashes" }
    else {
        $report = [System.IO.File]::ReadAllText($text[0].FullName)
        $expectedVersion = ((Get-Content -Encoding UTF8 -TotalCount 1 (Join-Path $folder 'version.txt')) -split ' ')[-1]
        if ($report -notmatch 'access violation') { Fail "[$label] the report does not name the exception" }
        if ($report.IndexOf($expectedVersion) -lt 0) { Fail "[$label] the report has no version" }
        if ($report.IndexOf('RocketArena.exe+0x') -lt 0) { Fail "[$label] the report has no place of the crash" }
        # a crash at the very start has no log yet
        if ($mode -eq '2' -and ($report.IndexOf('The end of the log') -lt 0 -or $report.IndexOf('[INFO]') -lt 0)) { Fail "[$label] the report has no end of the log" }
        Write-Step "[$label] $($dump[0].Name) ($([math]::Round($dump[0].Length / 1KB)) KB) and $($text[0].Name)"
    }
    if (-not (Test-Path (Join-Path $crashes 'pending.txt'))) { Fail "[$label] no marker of the crash for the next start" }
    return $crashes
}

function Test-Crash([string]$folder) {
    $crashes = Invoke-CrashRun 'aftercrash' $folder '1'
    if ($null -eq $crashes) { return }
    # the next start says so once
    Invoke-Run 'aftercrash' $folder '' @('the previous run crashed')
    if (Test-Path (Join-Path $crashes 'pending.txt')) { Fail '[aftercrash] the marker is still there after the game told about the crash' }
}

# Steam: the files are there; a start where Steam cannot start (no steam_appid.txt next to the exe) plays all the same and says why;
# a crash after Steam has started (the Steam library installs its own crash handler) still leaves our report
function Test-Steam([string]$folder) {
    if (-not (Test-Path (Join-Path $folder 'steam_api64.dll'))) { Write-Step 'package without Steam: nothing to check'; return }
    if (-not (Test-Path (Join-Path $folder 'steam_appid.txt'))) { Fail 'steam_api64.dll without steam_appid.txt'; return }
    # the script is copied: the log names the path of the script, and the path of the repository must not be in it
    $scriptCopy = Join-Path $work 'check_steam_screen.txt'
    Copy-Item (Join-Path $PSScriptRoot 'check_steam_screen.txt') $scriptCopy
    $script = @{ MYENGINE_UI_SCRIPT = $scriptCopy }

    $without = Join-Path $work 'RocketArena-nosteamclient'
    Copy-Item -Recurse $folder $without
    [System.IO.File]::Delete((Join-Path $without 'steam_appid.txt'))
    Invoke-Run 'nosteamclient' $without '' @('Steam is not available') $script

    $steamRunning = $null -ne (Get-Process -Name 'steam' -ErrorAction SilentlyContinue)
    if (-not $steamRunning) { Write-Warning '[steamcrash] the Steam client is not running on this machine: the crash after the start of Steam is not checked'; return }
    $crashes = Invoke-CrashRun 'steamcrash' $folder '2' $script
    $log = Join-Path $work 'localappdata-steamcrash\RocketArena\logs\myengine.log'
    if ((Test-Path $log) -and ([System.IO.File]::ReadAllText($log).IndexOf('Steam is available') -lt 0)) { Fail '[steamcrash] Steam did not start in this run: the check proves nothing' }
}

if (-not (Test-Path (Join-Path $source 'RocketArena.exe'))) { throw "$source\RocketArena.exe not found: run tools\package_game.ps1 first" }
if (-not $work.StartsWith($env:TEMP, [System.StringComparison]::OrdinalIgnoreCase)) { throw "Refusing to use $work" }
if (Test-Path $work) { [System.IO.Directory]::Delete($work, $true) }
New-Item -ItemType Directory -Force -Path $work | Out-Null

$clean = Join-Path $work 'RocketArena'
Write-Step "Copying the package to $clean"
Copy-Item -Recurse $source $clean

Test-ExeResources $clean
if (Test-WwisePackage $clean) { Test-WwiseFiles $clean }
Invoke-Run 'full' $clean
Test-Crash $clean
Test-Steam $clean

if ($WithoutScripting) {
    $noScripting = Join-Path $work 'RocketArena-noscripting'
    Copy-Item -Recurse $source $noScripting
    [System.IO.File]::Delete((Join-Path $noScripting 'python314.zip'))
    Invoke-Run 'noscripting' $noScripting 'ScriptRuntime: python314.zip not found'
}

if (-not $Keep) { [System.IO.Directory]::Delete($work, $true) }
if ($failures.Count -gt 0) {
    Write-Host "[CHECK] $($failures.Count) problem(s)" -ForegroundColor Red
    exit 1
}
Write-Host '[CHECK] The package runs from a clean folder' -ForegroundColor Green
