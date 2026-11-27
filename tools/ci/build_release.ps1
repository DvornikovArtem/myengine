<#
.SYNOPSIS
    LR5 (docs/crash/README.md): builds the release of Rocket Arena and packs the two archives of a GitHub Release from ONE build.

.DESCRIPTION
    1. Builds and assembles the game the usual way of the project: tools/package_game.ps1 -Config RelWithDebInfo (the shipping exe with the debug
       information, /O2 and a PDB), without Steam and Wwise (their SDKs are not in the repository), with the build id passed to CMake.
    2. Writes build-info.txt (buildId, commit, tag, the Actions run) into the game folder and into the symbols folder.
    3. Checks the release: every file the game needs to start a scene from a clean folder is there, RocketArena.pdb is the PDB of this very
       RocketArena.exe (the GUID of the exe is in the PDB), the exe says this build id. A missing file stops the script with exit code 1.
    4. Packs <Out>\RocketArena-<version>-build<id>-win64.zip (the game) and <Out>\RocketArena-<version>-build<id>-symbols.zip (RocketArena.pdb), and
       writes <Out>\release-notes.md, the text of the GitHub Release.
    In GitHub Actions (.github/workflows/release.yml) the paths go to GITHUB_OUTPUT: build_id, game_zip, symbols_zip, notes.

    The build id is GITHUB_RUN_ID.GITHUB_RUN_ATTEMPT in Actions (the run that built the release; a re-run of it gets a new attempt, so a new id),
    "local-<date>-<time>" on a machine.

.PARAMETER Out
    The folder of the archives (created; its RocketArena and RocketArena-symbols folders are replaced). Default: release (in the repository).
.PARAMETER BuildDir
    The build folder (package_game.ps1 -BuildDir). Default: build-release (in the repository).
.PARAMETER BuildId
    Overrides the build id (letters, digits, '.', '_', '-').
#>
[CmdletBinding()]
param(
    [string]$Out = 'release',
    [string]$BuildDir = 'build-release',
    [string]$BuildId = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
function Get-FullFolder([string]$path) {
    if ([System.IO.Path]::IsPathRooted($path)) { return [System.IO.Path]::GetFullPath($path) }
    return [System.IO.Path]::GetFullPath((Join-Path $root $path))
}
function Write-Step([string]$text) { Write-Host "[RELEASE] $text" }
function Write-Utf8([string]$path, [string]$text) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    [System.IO.File]::WriteAllText($path, ($text -replace "`r?`n", "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
}

$outDir = Get-FullFolder $Out
$buildPath = Get-FullFolder $BuildDir
$inActions = [bool]$env:GITHUB_ACTIONS

# ---------------------------------------------------------------- the identity of the build
if (-not $BuildId) {
    $BuildId = if ($env:GITHUB_RUN_ID) { "$($env:GITHUB_RUN_ID).$($env:GITHUB_RUN_ATTEMPT)" } else { 'local-' + (Get-Date -Format 'yyyyMMdd-HHmmss') }
}
if ($BuildId -notmatch '^[A-Za-z0-9._-]+$') { throw "Bad build id '$BuildId'" }
$commit = if ($env:GITHUB_SHA) { $env:GITHUB_SHA } else { (& git -C $root rev-parse HEAD).Trim() }
$ref = if ($env:GITHUB_REF_NAME) { $env:GITHUB_REF_NAME } else { (& git -C $root rev-parse --abbrev-ref HEAD).Trim() }
$isTag = $env:GITHUB_REF_TYPE -eq 'tag'
$runUrl = if ($env:GITHUB_RUN_ID) { "$($env:GITHUB_SERVER_URL)/$($env:GITHUB_REPOSITORY)/actions/runs/$($env:GITHUB_RUN_ID)/attempts/$($env:GITHUB_RUN_ATTEMPT)" } else { '(a local build)' }
$repoUrl = if ($env:GITHUB_REPOSITORY) { "$($env:GITHUB_SERVER_URL)/$($env:GITHUB_REPOSITORY)" } else { 'https://github.com/DvornikovArtem/myengine' }
$versionMatch = [regex]::Match([System.IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt')), 'project\(\s*myengine\s+VERSION\s+(\d+\.\d+\.\d+)')
if (-not $versionMatch.Success) { throw 'project(myengine VERSION x.y.z) not found in CMakeLists.txt' }
$version = $versionMatch.Groups[1].Value
Write-Step "Rocket Arena $version, build $BuildId, commit $commit ($ref)"

# ---------------------------------------------------------------- 1. build and assemble (the usual packing of the project)
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$package = Join-Path $root 'tools\package_game.ps1'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $package -Game rocketarena -Config RelWithDebInfo -NoSteam -NoWwise -NoZip `
    -Out $outDir -BuildDir $buildPath -BuildId $BuildId
if ($LASTEXITCODE -ne 0) { throw "package_game.ps1 failed with exit code $LASTEXITCODE" }

$gameName = 'RocketArena'
$gameDir = Join-Path $outDir $gameName
$symbolsDir = Join-Path $outDir "$gameName-symbols"
$exe = Join-Path $gameDir "$gameName.exe"
$pdbSource = Join-Path $buildPath "app\RelWithDebInfo\$gameName.pdb"

# ---------------------------------------------------------------- 2. build-info.txt and the symbols folder
$gameZipName = "$gameName-$version-build$BuildId-win64.zip"
$symbolsZipName = "$gameName-$version-build$BuildId-symbols.zip"
$builtAt = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
$buildInfo = @"
product: Rocket Arena
version: $version
buildId: $BuildId
commit: $commit
ref: $ref
run: $runUrl
built: $builtAt
game: $gameZipName
symbols: $symbolsZipName
"@
Write-Utf8 (Join-Path $gameDir 'build-info.txt') $buildInfo

if (Test-Path $symbolsDir) { Remove-Item -Recurse -Force $symbolsDir }
New-Item -ItemType Directory -Force -Path $symbolsDir | Out-Null
if (-not (Test-Path $pdbSource)) { throw "The PDB of the exe is not in the build folder: $pdbSource" }
# the name stays RocketArena.pdb: the exe names its PDB so (/PDBALTPATH in app/CMakeLists.txt), and a debugger looks for that name
Copy-Item -LiteralPath $pdbSource (Join-Path $symbolsDir "$gameName.pdb")
Write-Utf8 (Join-Path $symbolsDir 'build-info.txt') $buildInfo
Write-Utf8 (Join-Path $symbolsDir 'README.txt') @"
The debug symbols of Rocket Arena $version, build $BuildId (commit $commit).
RocketArena.pdb belongs to RocketArena.exe of $gameZipName from the same GitHub Release, and only to it.
How a crash dump is read with them: docs/crash/dump-analysis.md of the repository at this commit:
$repoUrl/blob/$commit/docs/crash/dump-analysis.md
"@

# ---------------------------------------------------------------- 3. the checks of the release
$missing = New-Object 'System.Collections.Generic.List[string]'
$required = @(
    "$gameName.exe", 'python314.dll', 'python314.zip', 'vcruntime140.dll', 'msvcp140.dll',
    'config\app.json', 'myengine.myproject', 'version.txt', 'build-info.txt', 'README.txt',
    'assets\scenes\rocket_league.json', 'assets\shaders\textured_lit.hlsl', 'assets\fonts'
)
foreach ($name in $required) { if (-not (Test-Path (Join-Path $gameDir $name))) { $missing.Add("$gameName\$name") } }
foreach ($name in @("$gameName.pdb", 'build-info.txt')) { if (-not (Test-Path (Join-Path $symbolsDir $name))) { $missing.Add("$gameName-symbols\$name") } }
# the scene of the config is in the package
if (Test-Path (Join-Path $gameDir 'config\app.json')) {
    $startScene = (Get-Content -Raw -Encoding UTF8 (Join-Path $gameDir 'config\app.json') | ConvertFrom-Json).startScene
    if (-not $startScene -or -not (Test-Path (Join-Path $gameDir $startScene.Replace('/', '\')))) { $missing.Add("$gameName\$startScene (the start scene of config\app.json)") }
}
if ($missing.Count -gt 0) { throw "The release is not complete, missing: $($missing -join ', ')" }

# a search of bytes in a file of hundreds of MB (a loop of PowerShell would take minutes)
Add-Type -TypeDefinition @'
public static class ReleaseBytes {
    public static int IndexOf(byte[] data, byte[] pattern, int from) {
        for (int i = from; i <= data.Length - pattern.Length; i++) {
            int j = 0;
            while (j < pattern.Length && data[i + j] == pattern[j]) j++;
            if (j == pattern.Length) return i;
        }
        return -1;
    }
}
'@
# the exe and the PDB are of one link: the RSDS record of the exe (the GUID of its PDB, then the age, then the name) has its GUID in the PDB
$exeBytes = [System.IO.File]::ReadAllBytes($exe)
$rsdsTag = [System.Text.Encoding]::ASCII.GetBytes('RSDS')
$rsds = -1
for ($at = [ReleaseBytes]::IndexOf($exeBytes, $rsdsTag, 0); $at -ge 0; $at = [ReleaseBytes]::IndexOf($exeBytes, $rsdsTag, $at + 1)) {
    $nameStart = $at + 24
    $nameEnd = [Array]::IndexOf($exeBytes, [byte]0, $nameStart)
    if ($nameEnd -gt $nameStart -and [System.Text.Encoding]::ASCII.GetString($exeBytes, $nameStart, $nameEnd - $nameStart) -eq "$gameName.pdb") { $rsds = $at; break }
}
if ($rsds -lt 0) { throw "$gameName.exe has no debug record that names $gameName.pdb" }
$guid = New-Object byte[] 16
[Array]::Copy($exeBytes, $rsds + 4, $guid, 0, 16)
$guidText = ([guid]::new($guid)).ToString('N').ToUpper()
$age = [BitConverter]::ToUInt32($exeBytes, $rsds + 20)
$pdbBytes = [System.IO.File]::ReadAllBytes((Join-Path $symbolsDir "$gameName.pdb"))
if ([ReleaseBytes]::IndexOf($pdbBytes, $guid, 0) -lt 0) { throw "$gameName.pdb is not the PDB of this $gameName.exe (the GUID $guidText of the exe is not in it)" }
$pdbBytes = $null
Write-Step "$gameName.pdb matches $gameName.exe (GUID $guidText, age $age)"
# the exe says this build id (Version.h, MYENGINE_BUILD_ID): the crash reports will name it
if ([ReleaseBytes]::IndexOf($exeBytes, [System.Text.Encoding]::ASCII.GetBytes($BuildId), 0) -lt 0) {
    throw "$gameName.exe does not have the build id $BuildId in it: the build folder was configured for another id"
}
Write-Step "$gameName.exe has the build id $BuildId"

# ---------------------------------------------------------------- 4. the archives and the text of the release
Add-Type -AssemblyName System.IO.Compression.FileSystem
$gameZip = Join-Path $outDir $gameZipName
$symbolsZip = Join-Path $outDir $symbolsZipName
foreach ($pair in @(@($gameDir, $gameZip), @($symbolsDir, $symbolsZip))) {
    if (Test-Path $pair[1]) { Remove-Item -Force $pair[1] }
    [System.IO.Compression.ZipFile]::CreateFromDirectory($pair[0], $pair[1], [System.IO.Compression.CompressionLevel]::Optimal, $true)
    Write-Step ('{0}: {1:N1} MB' -f $pair[1], ((Get-Item $pair[1]).Length / 1MB))
}

$tagLine = if ($isTag) { "Tag ``$ref``, " } else { "Branch ``$ref``, " }
$notes = @"
## Rocket Arena $version, build ``$BuildId``

| | |
| --- | --- |
| **buildId** | ``$BuildId`` |
| **commit** | [``$commit``]($repoUrl/commit/$commit) |
| **built by** | $runUrl |
| **built** | $builtAt |

$tagLine Windows x64, RelWithDebInfo (the shipping game with /O2 and a PDB), without Steam and Wwise.

### Assets
- ``$gameZipName``: the game. Unpack it into an empty folder and start ``RocketArena\RocketArena.exe``.
- ``$symbolsZipName``: ``RocketArena.pdb``, the debug symbols of this very exe (for the crash dumps of this build only).

Both archives have ``build-info.txt`` with the same buildId and commit; every crash report of this exe names this buildId.

### For the tester
1. System: Windows 10 or 11 x64, a GPU with DirectX 12; nothing has to be installed (the Visual C++ runtime and Python are in the folder).
2. Start ``RocketArena.exe``. The main menu has the **CRASH TEST** panel (top right, with the build id): each button crashes the game on purpose.
3. After a crash the report is in ``%LOCALAPPDATA%\RocketArena\crashes``: three files ``crash-<date>-<time>-<reportId>.dmp / .txt / .log``. Send all three.

The full instruction: [docs/crash/tester-guide.md]($repoUrl/blob/$commit/docs/crash/tester-guide.md). How the developer reads a report: [docs/crash/dump-analysis.md]($repoUrl/blob/$commit/docs/crash/dump-analysis.md).
"@
$notesFile = Join-Path $outDir 'release-notes.md'
Write-Utf8 $notesFile $notes

if ($inActions -and $env:GITHUB_OUTPUT) {
    Add-Content -Encoding UTF8 -Path $env:GITHUB_OUTPUT -Value @("build_id=$BuildId", "game_zip=$gameZip", "symbols_zip=$symbolsZip", "notes=$notesFile")
}
Write-Step "Done: $gameZip, $symbolsZip"
