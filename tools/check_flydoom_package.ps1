<#
.SYNOPSIS
    Runs dist/FlyDoom from a clean folder outside the repository and checks that the package of FlyDoom stands alone and carries what a GPL game must.

.DESCRIPTION
    The check of the package of tools/package_game.ps1 -Game flydoom (the manifest games/flydoom/game.json; the Check package button of the Build window of the editor).
    Copies the package to %TEMP%\flydoom_clean, starts FlyDoom.exe from another working directory with its own LOCALAPPDATA / APPDATA (so the real ones stay
    untouched) and checks:
      - the files: README.txt, COPYING (GPL), SOURCE.txt (names a commit), THIRD_PARTY.txt, licenses\ with the texts of the data and of the libraries, version.txt;
        a WAD of the full game (DOOM.WAD, DOOM2.WAD, plutonia, tnt) is never in the package; doom1.wad, when it is there, is the shareware 1.9 (md5) with its license
        next to it; the fly brain, when it is there, is a .fbrain file (the magic, the version 1, the notice ATTRIB) and THIRD_PARTY.txt has its CC BY 4.0
        attribution; there is no PDB and no file of an SDK;
      - the exe has its icon and the version info (the version of the project, "FlyDoom");
      - the run: the game starts in a clean folder with no WAD of the player and ends on its own with the exit code 0, the log is in <LOCALAPPDATA>\FlyDoom\logs,
        says which WAD it took (DOOM: the game data is ...), names no path of the repository and has no error line; the screenshot of the frame is a picture, not
        one colour;
      - every WAD of the package is taken by --iwad and the game plays from it to the first level (-warp 1 1);
      - nothing is created or changed next to the exe, nothing is written to APPDATA;
      - a deliberate crash (MYENGINE_CRASH_TEST=1) leaves a minidump, a report and the marker in <LOCALAPPDATA>\FlyDoom\crashes, and the next start tells about it once.
    This package check validates the fly brain file; separate fly-mode tests exercise its simulation. When the log mentions the brain it must not report an error.
    docs/game/packaging.md describes the checks.

.PARAMETER Dist
    The folder of the package (relative to the repository root, or a full path: the zip unpacked somewhere).
.PARAMETER Frames
    --exit-after-frames for the runs.
.PARAMETER Keep
    Keep %TEMP%\flydoom_clean after the checks.
#>
[CmdletBinding()]
param(
    [string]$Dist = 'dist\FlyDoom',
    [int]$Frames = 400,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$source = if ([System.IO.Path]::IsPathRooted($Dist)) { [System.IO.Path]::GetFullPath($Dist) } else { [System.IO.Path]::GetFullPath((Join-Path $root $Dist)) }
$tempRoot = (Get-Item -LiteralPath $env:TEMP).FullName   # the long name: Push-Location and the relative paths do not take the short (8.3) one
$work = Join-Path $tempRoot 'flydoom_clean'
$failures = New-Object 'System.Collections.Generic.List[string]'
$sharewareMd5 = 'f0cefca49926d00903cf57551d901abe'

function Write-Step([string]$text) { Write-Host "[CHECK] $text" }
# a warning as a line of text that does not depend on the language of Windows (Write-Warning prints a translated word on a Russian Windows): the Build window finds "WARNING:"
function Write-Warning([string]$Message) { Write-Host "WARNING: $Message" }
function Fail([string]$text) { $failures.Add($text); Write-Host "[CHECK] FAILED: $text" -ForegroundColor Red }

# the path of a file inside a folder, from the folder (TEMP is given by its short name, a file is named by the long one: no cutting by length)
function Get-RelativePath([string]$folder, [string]$file) {
    Push-Location -LiteralPath $folder
    try { return (Resolve-Path -LiteralPath $file -Relative).Substring(2) }
    finally { Pop-Location }
}

function Get-Snapshot([string]$folder) {
    $prefix = $folder.TrimEnd('\') + '\'
    $result = @{}
    Get-ChildItem -Path $folder -Recurse -File -Force | ForEach-Object {
        $result[$_.FullName.Substring($prefix.Length)] = '{0}|{1}' -f $_.Length, $_.LastWriteTimeUtc.Ticks
    }
    return $result
}

function Read-Text([string]$path) { return [System.IO.File]::ReadAllText($path) }

# what the package of a GPL game must have, and what it must not have
function Test-PackageFiles([string]$folder) {
    foreach ($name in @('FlyDoom.exe', 'README.txt', 'COPYING', 'SOURCE.txt', 'THIRD_PARTY.txt', 'version.txt', 'config\app.json', 'myengine.myproject', 'python314.dll')) {
        if (-not (Test-Path (Join-Path $folder $name))) { Fail "the package has no $name" }
    }
    foreach ($name in @('Freedoom-COPYING.txt', 'Freedoom-CREDITS.txt', 'COPYING-nuked-opl3.txt', 'OFL-1.1.txt', 'FreeType-FTL.txt')) {
        if (-not (Test-Path (Join-Path $folder "licenses\$name"))) { Fail "no licenses\$name" }
    }
    $copying = Join-Path $folder 'COPYING'
    if ((Test-Path $copying) -and (Read-Text $copying) -notmatch 'GNU GENERAL PUBLIC LICENSE\s+Version 2') { Fail 'COPYING is not the GPL version 2' }
    $sourceFile = Join-Path $folder 'SOURCE.txt'
    if (Test-Path $sourceFile) {
        $sourceText = Read-Text $sourceFile
        if ($sourceText -notmatch 'Commit\s*:\s*[0-9a-f]{40}') { Fail 'SOURCE.txt names no commit (40 hex digits)' }
        if ($sourceText -match '\{[A-Z_]+\}') { Fail "SOURCE.txt has a placeholder that was not filled: $($Matches[0])" }
        if ($sourceText -notmatch 'github\.com/DvornikovArtem/myengine') { Fail 'SOURCE.txt names no repository' }
    }
    $thirdParty = Join-Path $folder 'THIRD_PARTY.txt'
    $thirdText = if (Test-Path $thirdParty) { Read-Text $thirdParty } else { '' }
    foreach ($needle in @('doomgeneric', 'Chocolate Doom', 'Nuked OPL3', 'Freedoom', 'doom1.wad', 'MaleCNS', 'CC BY 4.0', 'Berg S.', '10.1016/j.cell.2026.08.015')) {
        if ($thirdText.IndexOf($needle, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) { Fail "THIRD_PARTY.txt does not mention '$needle'" }
    }

    # the original game is the player's own, never ours
    $original = @(Get-ChildItem -Path $folder -Recurse -File | Where-Object { $_.Name -in @('DOOM.WAD', 'DOOM2.WAD', 'PLUTONIA.WAD', 'TNT.WAD', 'DOOM64.WAD') })
    if ($original.Count -gt 0) { Fail "a WAD of the full game is in the package: $((Get-RelativePath $folder $original[0].FullName))" }
    $forbidden = @(Get-ChildItem -Path $folder -Recurse -File | Where-Object { $_.Extension -in @('.pdb', '.lib', '.h', '.hpp', '.obj') -or $_.Name -like 'Ak*.dll' -or $_.Name -like 'steam_api*' })
    if ($forbidden.Count -gt 0) { Fail "a file that must not be in the package: $((Get-RelativePath $folder $forbidden[0].FullName))" }

    $shareware = Join-Path $folder 'wads\doom1.wad'
    if (Test-Path $shareware) {
        if ((Get-FileHash -Algorithm MD5 -LiteralPath $shareware).Hash -ne $sharewareMd5.ToUpper()) { Fail "wads\doom1.wad is not the shareware DOOM 1.9 (md5 $sharewareMd5)" }
        if (-not (Test-Path (Join-Path $folder 'wads\doom1.wad.license.txt'))) { Fail 'wads\doom1.wad has no license text next to it (wads\doom1.wad.license.txt)' }
        elseif ((Read-Text (Join-Path $folder 'wads\doom1.wad.license.txt')) -notmatch 'freely distributable') { Fail 'wads\doom1.wad.license.txt is not the license of the shareware' }
    }
    elseif (Test-Path (Join-Path $folder 'wads\doom1.wad.license.txt')) { Fail 'the license of doom1.wad is in the package without doom1.wad' }
    else { Write-Warning 'the package has no doom1.wad (the shareware DOOM): it plays with Freedoom only' }
    if (-not (Test-Path (Join-Path $folder 'assets\doom\freedoom1.wad')) -and -not (Test-Path $shareware)) { Fail 'the package has no WAD at all' }

    $brain = Join-Path $folder 'assets\flybrain\malecns_v1.fbrain'
    if (Test-Path $brain) {
        $stream = [System.IO.File]::OpenRead($brain)
        try {
            $header = New-Object byte[] 40
            [void]$stream.Read($header, 0, 40)
        }
        finally { $stream.Dispose() }
        $magic = [System.Text.Encoding]::ASCII.GetString($header, 0, 8)
        $formatVersion = [System.BitConverter]::ToUInt32($header, 8)
        $neurons = [System.BitConverter]::ToUInt32($header, 16)
        if ($magic -ne 'FLYBRAIN' -or $formatVersion -ne 1) { Fail "malecns_v1.fbrain is not a .fbrain file of the version 1 (magic '$magic', version $formatVersion)" }
        elseif ($neurons -lt 100000) { Fail "malecns_v1.fbrain has $neurons neurons: not the connectome of the fly" }
        else { Write-Step ("the fly brain: {0:N1} MB, {1:N0} neurons" -f ((Get-Item $brain).Length / 1MB), $neurons) }
        # the attribution is in the file itself (the section ATTRIB) and the license text says the same
        $head = [System.IO.File]::ReadAllBytes($brain)[0..4095]
        $headText = [System.Text.Encoding]::ASCII.GetString($head)
        if ((Get-Item $brain).Length -gt 4096 -and $headText.IndexOf('ATTRIB') -lt 0) { Fail 'the table of the sections of malecns_v1.fbrain has no ATTRIB' }
    }
    else { Write-Warning 'the package has no fly brain (assets\flybrain\malecns_v1.fbrain)' }

    # the faces of real people do not go into a game (the decision of the owner): no face database in the package
    if (Test-Path (Join-Path $folder 'faceid\allowed_faces.fdb')) { Fail 'the package has faceid\allowed_faces.fdb: the games carry no face database' }

    $versionText = Read-Text (Join-Path $folder 'version.txt')
    foreach ($line in @(Get-ChildItem -Path $folder -Recurse -File | Where-Object { $_.FullName -match '\\wads\\' -or $_.FullName -match '\\assets\\(doom|flybrain)\\' })) {
        $relative = (Get-RelativePath $folder $line.FullName).Replace('\', '/')
        # the controls of the fly (controls_*.json) and the trained fly (fly_trained.bin, N4) travel as assets of the manifest, not as data files: version.txt does not list them
        if ($relative -notlike '*.license.txt' -and $relative -notlike 'assets/flybrain/controls_*.json' -and $relative -ne 'assets/flybrain/fly_trained.bin' -and $versionText.IndexOf("file $relative") -lt 0) { Fail "version.txt does not list the data file $relative" }
    }
    # N4: the trained fly (the memory of our layer WallSense) is in the package and is that file
    $trained = Join-Path $folder 'assets\flybrain\fly_trained.bin'
    if (Test-Path $trained) {
        $magic = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($trained), 0, 8)
        if ($magic -ne 'FLYWALL3') { Fail "fly_trained.bin is not a memory of WallSense (magic '$magic')" }
    }
    else { Fail 'the package has no trained fly (assets\flybrain\fly_trained.bin)' }
    $total = (Get-ChildItem -Path $folder -Recurse -File | Measure-Object Length -Sum).Sum
    Write-Step ("the package: {0:N1} MB" -f ($total / 1MB))
}

# a TGA that is a picture: the pixels differ (a black or one-colour frame is a window that drew nothing)
function Test-Picture([string]$path, [string]$label) {
    if (-not (Test-Path $path)) { Fail "[$label] no screenshot $path"; return }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    if ($bytes.Length -lt 1000) { Fail "[$label] the screenshot is too small ($($bytes.Length) bytes)"; return }
    $seen = New-Object 'System.Collections.Generic.HashSet[int]'
    for ($i = 18; $i + 2 -lt $bytes.Length; $i += 997) { [void]$seen.Add([int]$bytes[$i] * 65536 + [int]$bytes[$i + 1] * 256 + [int]$bytes[$i + 2]) }
    if ($seen.Count -lt 24) { Fail "[$label] the screenshot has $($seen.Count) colours in the sample: the frame is not a picture" }
    else { Write-Step "[$label] screenshot $([System.IO.Path]::GetFileName($path)): $($seen.Count) colours in the sample" }
}

# one run of the game: its own LOCALAPPDATA / APPDATA, another working directory. extra: the arguments after --exit-after-frames.
function Invoke-Run([string]$label, [string]$folder, [string[]]$extra = @(), [string[]]$expectedLog = @(), [switch]$Screenshot, [switch]$Menu) {
    $appData = Join-Path $work "appdata-$label"
    $localAppData = Join-Path $work "localappdata-$label"
    $workingDirectory = Join-Path $work "cwd-$label"
    New-Item -ItemType Directory -Force -Path $appData, $localAppData, $workingDirectory | Out-Null
    $picture = Join-Path $work "screenshot-$label.tga"

    $before = Get-Snapshot $folder
    $savedAppData = $env:APPDATA
    $savedLocalAppData = $env:LOCALAPPDATA
    $env:APPDATA = $appData
    $env:LOCALAPPDATA = $localAppData
    try {
        $arguments = @('--exit-after-frames', "$Frames")
        if ($Screenshot) { $arguments += @('--screenshot', $picture, '--screenshot-frame', [string]($Frames - 100)) }
        $arguments += $extra
        Write-Step "[$label] running FlyDoom.exe $($arguments -join ' ')"
        $quoted = @($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } })
        $process = Start-Process -FilePath (Join-Path $folder 'FlyDoom.exe') -ArgumentList $quoted -WorkingDirectory $workingDirectory -PassThru
    }
    finally {
        $env:APPDATA = $savedAppData
        $env:LOCALAPPDATA = $savedLocalAppData
    }
    if (-not $process.WaitForExit(180000)) {
        $process.Kill()
        Fail "[$label] the game did not finish within 180 s"
    }
    elseif ($process.ExitCode -ne 0) { Fail "[$label] exit code $($process.ExitCode)" }
    else { Write-Step "[$label] exit code 0" }

    $after = Get-Snapshot $folder
    foreach ($name in $after.Keys) {
        if (-not $before.ContainsKey($name)) { Fail "[$label] a file appeared next to the exe: $name" }
        elseif ($before[$name] -ne $after[$name]) { Fail "[$label] a file next to the exe changed: $name" }
    }
    foreach ($name in $before.Keys) {
        if (-not $after.ContainsKey($name)) { Fail "[$label] a file next to the exe disappeared: $name" }
    }

    $userData = Join-Path $localAppData 'FlyDoom'
    $log = Join-Path $userData 'logs\myengine.log'
    if (-not (Test-Path $log)) { Fail "[$label] no log in $userData\logs" }
    else {
        $text = Read-Text $log
        foreach ($marker in @($root, 'Documents\GitHub', '\.team\', 'build-shipping', 'flydoom-build')) {
            if ($text.IndexOf($marker, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) { Fail "[$label] the log names the repository ('$marker')" }
        }
        $errors = @($text -split "`n" | Where-Object { $_ -match '\[(ERROR|FATAL)\]' })
        if ($errors.Count -gt 0) { Fail "[$label] the log has $($errors.Count) error line(s), the first: $($errors[0].Trim())" }
        # without arguments FlyDoom stops at its menu: the WAD is chosen when a mode starts, so there is no line about the game data yet (-Menu)
        foreach ($expected in (@($(if ($Menu) { } else { 'DOOM: the game data is' })) + $expectedLog)) {
            if ($text.IndexOf($expected, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) { Fail "[$label] the log has no '$expected'" }
        }
        $chosen = @($text -split "`n" | Where-Object { $_ -match 'DOOM: the game data is' })
        if ($chosen.Count -gt 0) { Write-Step "[$label] $($chosen[0].Trim() -replace '^.*(DOOM: the game data is)', '$1')" }
        Write-Step "[$label] log: $([math]::Round((Get-Item $log).Length / 1KB, 1)) KB"
    }
    $written = @(Get-ChildItem -Path $appData -Recurse -File -ErrorAction SilentlyContinue)
    if ($written.Count -gt 0) { Fail "[$label] files were written to APPDATA: $($written[0].FullName)" }
    if ($Screenshot) { Test-Picture $picture $label }
    return $localAppData
}

# the icon and the version info of the exe
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class FlyDoomResources
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
    $exePath = Join-Path $folder 'FlyDoom.exe'
    $info = (Get-Item $exePath).VersionInfo
    $expectedVersion = ((Get-Content -Encoding UTF8 -TotalCount 1 (Join-Path $folder 'version.txt')) -split ' ')[-1]
    if ($info.ProductName -ne 'FlyDoom') { Fail "the exe has ProductName '$($info.ProductName)', expected 'FlyDoom'" }
    if ($info.OriginalFilename -ne 'FlyDoom.exe') { Fail "the exe has OriginalFilename '$($info.OriginalFilename)'" }
    if (-not $info.FileDescription) { Fail 'the exe has no FileDescription' }
    if ($info.FileVersion -ne $expectedVersion) { Fail "the exe says version $($info.FileVersion), version.txt says $expectedVersion" }
    if (-not [FlyDoomResources]::HasIcon($exePath)) { Fail 'the exe has no icon (RT_GROUP_ICON 1)' }
    Write-Step "exe: $($info.ProductName) $($info.ProductVersion), $($info.FileDescription), icon $([FlyDoomResources]::HasIcon($exePath))"
}

# A deliberate crash at the very start leaves the report and the marker; the next start tells the player once (the log line "the previous run crashed") and removes the marker
function Test-Crash([string]$folder) {
    $appData = Join-Path $work 'appdata-crash'
    $localAppData = Join-Path $work 'localappdata-crash'
    $workingDirectory = Join-Path $work 'cwd-crash'
    New-Item -ItemType Directory -Force -Path $appData, $localAppData, $workingDirectory | Out-Null
    $saved = @{ APPDATA = $env:APPDATA; LOCALAPPDATA = $env:LOCALAPPDATA }
    $env:APPDATA = $appData; $env:LOCALAPPDATA = $localAppData
    $env:MYENGINE_CRASH_TEST = '1'; $env:MYENGINE_CRASH_NO_DIALOG = '1'
    try {
        Write-Step '[crash] running FlyDoom.exe with MYENGINE_CRASH_TEST=1'
        $process = Start-Process -FilePath (Join-Path $folder 'FlyDoom.exe') -WorkingDirectory $workingDirectory -PassThru
    }
    finally {
        $env:APPDATA = $saved.APPDATA; $env:LOCALAPPDATA = $saved.LOCALAPPDATA
        Remove-Item Env:MYENGINE_CRASH_TEST, Env:MYENGINE_CRASH_NO_DIALOG -ErrorAction SilentlyContinue
    }
    if (-not $process.WaitForExit(90000)) { $process.Kill(); Fail '[crash] the crashed game did not end'; return }
    if ($process.ExitCode -ne -1073741819) { Fail "[crash] exit code $($process.ExitCode), expected 0xC0000005 (-1073741819)" }
    $crashes = Join-Path $localAppData 'FlyDoom\crashes'
    $dump = @(Get-ChildItem -Path $crashes -Filter 'crash-*.dmp' -ErrorAction SilentlyContinue)
    $report = @(Get-ChildItem -Path $crashes -Filter 'crash-*.txt' -ErrorAction SilentlyContinue)
    if ($dump.Count -ne 1 -or $dump[0].Length -lt 10000) { Fail "[crash] the minidump is missing or too small in $crashes" }
    elseif ($report.Count -ne 1) { Fail "[crash] the report .txt is missing in $crashes" }
    else {
        $reportText = Read-Text $report[0].FullName
        $expectedVersion = ((Get-Content -Encoding UTF8 -TotalCount 1 (Join-Path $folder 'version.txt')) -split ' ')[-1]
        if ($reportText -notmatch 'access violation') { Fail '[crash] the report does not name the exception' }
        if ($reportText.IndexOf($expectedVersion) -lt 0) { Fail '[crash] the report has no version' }
        if ($reportText.IndexOf('FlyDoom.exe+0x') -lt 0) { Fail '[crash] the report has no place of the crash' }
        Write-Step "[crash] $($dump[0].Name) ($([math]::Round($dump[0].Length / 1KB)) KB) and $($report[0].Name)"
    }
    if (-not (Test-Path (Join-Path $crashes 'pending.txt'))) { Fail '[crash] no marker of the crash for the next start'; return }

    # the next start (the same LOCALAPPDATA) says so once: the log line, and the marker is gone
    $env:APPDATA = $appData; $env:LOCALAPPDATA = $localAppData
    try {
        $next = Start-Process -FilePath (Join-Path $folder 'FlyDoom.exe') -ArgumentList @('--exit-after-frames', '120') -WorkingDirectory $workingDirectory -PassThru
    }
    finally { $env:APPDATA = $saved.APPDATA; $env:LOCALAPPDATA = $saved.LOCALAPPDATA }
    if (-not $next.WaitForExit(90000)) { $next.Kill(); Fail '[crash] the start after the crash did not end'; return }
    if ($next.ExitCode -ne 0) { Fail "[crash] the start after the crash: exit code $($next.ExitCode)" }
    $log = Join-Path $localAppData 'FlyDoom\logs\myengine.log'
    if (-not (Test-Path $log)) { Fail '[crash] the start after the crash left no log' }
    else {
        $logText = Read-Text $log
        if ($logText.IndexOf('the previous run crashed', [System.StringComparison]::OrdinalIgnoreCase) -lt 0) { Fail "[crash] the start after the crash does not tell about it (no 'the previous run crashed' in the log)" }
        elseif ($logText.IndexOf($report[0].Name) -lt 0) { Fail "[crash] the line about the crash does not name the report $($report[0].Name)" }
        else { Write-Step '[crash] the next start told about the crash' }
    }
    if (Test-Path (Join-Path $crashes 'pending.txt')) { Fail '[crash] the marker is still there after the game told about the crash' }
}

if (-not (Test-Path (Join-Path $source 'FlyDoom.exe'))) { throw "$source\FlyDoom.exe not found: run tools\package_game.ps1 -Game flydoom first" }
if (-not $work.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) { throw "Refusing to use $work" }
if (Test-Path $work) { [System.IO.Directory]::Delete($work, $true) }
New-Item -ItemType Directory -Force -Path $work | Out-Null

$clean = Join-Path $work 'FlyDoom'
Write-Step "Copying the package to $clean"
Copy-Item -Recurse $source $clean

Test-PackageFiles $clean
Test-ExeResources $clean

# the run as a player starts it: no argument. The Steam copy of the original DOOM of this machine may be chosen instead of the WAD of the package
# (the registry is not ours to change): the log says which, and the runs below name the WADs of the package by --iwad
[void](Invoke-Run 'plain' $clean @() @() -Screenshot -Menu)
foreach ($wad in @(@{ Name = 'freedoom'; Path = 'assets\doom\freedoom1.wad' }, @{ Name = 'shareware'; Path = 'wads\doom1.wad' })) {
    $wadPath = Join-Path $clean $wad.Path
    if (Test-Path $wadPath) {
        [void](Invoke-Run $wad.Name $clean @('--iwad', $wadPath, '--doom-args', '-warp 1 1 -skill 3') @($wad.Path.Replace('\', '/').Split('/')[-1]) -Screenshot)
    }
}
Test-Crash $clean

if (-not $Keep) { [System.IO.Directory]::Delete($work, $true) }
if ($failures.Count -gt 0) {
    Write-Host "[CHECK] $($failures.Count) problem(s)" -ForegroundColor Red
    exit 1
}
Write-Host '[CHECK] The package runs from a clean folder' -ForegroundColor Green
