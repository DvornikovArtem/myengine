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
      - nothing is written to APPDATA.
    With -WithoutScripting it runs once more without python314.zip (the game must work without scripting).
    docs/game/packaging.md describes the checks.

.PARAMETER Dist
    The folder of the package (relative to the repository root).
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
$source = [System.IO.Path]::GetFullPath((Join-Path $root $Dist))
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

function Invoke-Run([string]$label, [string]$folder, [string]$expectedError = '') {
    $appData = Join-Path $work "appdata-$label"
    $localAppData = Join-Path $work "localappdata-$label"
    $workingDirectory = Join-Path $work "cwd-$label"
    New-Item -ItemType Directory -Force -Path $appData, $localAppData, $workingDirectory | Out-Null

    $before = Get-Snapshot $folder
    $savedAppData = $env:APPDATA
    $savedLocalAppData = $env:LOCALAPPDATA
    $env:APPDATA = $appData
    $env:LOCALAPPDATA = $localAppData
    try {
        Write-Step "[$label] running RocketArena.exe --exit-after-frames $Frames"
        $process = Start-Process -FilePath (Join-Path $folder 'RocketArena.exe') -ArgumentList @('--exit-after-frames', $Frames) `
            -WorkingDirectory $workingDirectory -PassThru
    }
    finally {
        $env:APPDATA = $savedAppData
        $env:LOCALAPPDATA = $savedLocalAppData
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
        foreach ($expected in @('Particle effects loaded', 'sound bank')) {
            if ($text.IndexOf($expected, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) { Fail "[$label] the log has no '$expected'" }
        }
        Write-Step "[$label] log: $log ($([math]::Round((Get-Item $log).Length / 1KB, 1)) KB)"
    }
    $written = @(Get-ChildItem -Path $appData -Recurse -File -ErrorAction SilentlyContinue)
    if ($written.Count -gt 0) { Fail "[$label] files were written to APPDATA: $($written[0].FullName)" }
    $userFiles = @(Get-ChildItem -Path $userData -Recurse -File -ErrorAction SilentlyContinue)
    Write-Step "[$label] files in %LOCALAPPDATA%\RocketArena: $($userFiles.Count) ($([math]::Round(($userFiles | Measure-Object Length -Sum).Sum / 1MB, 1)) MB)"
}

if (-not (Test-Path (Join-Path $source 'RocketArena.exe'))) { throw "$source\RocketArena.exe not found: run tools\package_game.ps1 first" }
if (-not $work.StartsWith($env:TEMP, [System.StringComparison]::OrdinalIgnoreCase)) { throw "Refusing to use $work" }
if (Test-Path $work) { [System.IO.Directory]::Delete($work, $true) }
New-Item -ItemType Directory -Force -Path $work | Out-Null

$clean = Join-Path $work 'RocketArena'
Write-Step "Copying the package to $clean"
Copy-Item -Recurse $source $clean

Invoke-Run 'full' $clean

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
