<#
.SYNOPSIS
    Builds the shipping game (RocketArena.exe) and packs it into dist/RocketArena and a zip for players.

.DESCRIPTION
    1. Configures build-shipping/ (MYENGINE_SHIPPING=ON, no tests, Release) and builds the myengine target.
    2. Collects the assets by tools/package_manifest.json (roots plus everything the scenes, materials and data name).
    3. Puts together dist/RocketArena: exe, python314.dll / python314.zip, the Visual C++ runtime app-local,
       assets, config/app.json (starts the Rocket League map), myengine.myproject, README.txt, licenses/.
    4. Writes dist/RocketArena-<version>-win64.zip.
    Nothing but the exe is taken from the build folder: no PDB, no tests, no editor-only files.
    docs/game/packaging.md describes the package and tools/check_package.ps1 checks it on a clean folder.

.PARAMETER Config
    Shipping (the default) or Release: both build the Release configuration of CMake.
.PARAMETER Out
    The output folder (relative to the repository root): <Out>/RocketArena and the zip.
.PARAMETER BuildDir
    The build folder of the shipping build (relative to the repository root).
.PARAMETER SkipBuild
    Pack the exe that is already in the build folder.
.PARAMETER NoZip
    Do not write the zip.
.PARAMETER WithSteam
    Build the exe with Steam (MYENGINE_STEAM=ON against -SteamSdk), put steam_api64.dll next to it and write steam_appid.txt.
.PARAMETER SteamSdk
    The folder of the unpacked Steamworks SDK (the one with redistributable_bin).
#>
[CmdletBinding()]
param(
    [ValidateSet('Shipping', 'Release')][string]$Config = 'Shipping',
    [string]$Out = 'dist',
    [string]$BuildDir = 'build-shipping',
    [switch]$SkipBuild,
    [switch]$NoZip,
    [switch]$WithSteam,
    [string]$SteamSdk = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outDir = [System.IO.Path]::GetFullPath((Join-Path $root $Out))
$buildPath = [System.IO.Path]::GetFullPath((Join-Path $root $BuildDir))
$manifestPath = Join-Path $PSScriptRoot 'package_manifest.json'
$manifest = Get-Content -Raw -Encoding UTF8 $manifestPath | ConvertFrom-Json
$gameName = $manifest.name
$version = $manifest.version
$dist = Join-Path $outDir $gameName

function Write-Step([string]$text) { Write-Host "[PACKAGE] $text" }

function Invoke-Native([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$exe failed with exit code $LASTEXITCODE"
    }
}

function Write-TextFile([string]$path, [string]$text, [bool]$bom) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($bom)))
}

# Visual Studio: the generator for CMake and the folder with the redistributable runtime
function Find-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found: install Visual Studio with the C++ workload' }
    $lines = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | Out-String
    $found = $lines | ConvertFrom-Json
    if (-not $found) { throw 'Visual Studio with the C++ x64 tools not found' }
    $entry = @($found)[0]
    $major = [int]($entry.installationVersion.Split('.')[0])
    $generator = switch ($major) { 18 { 'Visual Studio 18 2026' } 17 { 'Visual Studio 17 2022' } default { throw "Unsupported Visual Studio $major" } }
    return [pscustomobject]@{ Path = $entry.installationPath; Generator = $generator }
}

# ---------------------------------------------------------------- build
$exe = Join-Path $buildPath 'app\Release\RocketArena.exe'
$vs = $null
$steamDll = $null
if ($WithSteam) {
    # before the long build: the SDK must be there (a path with non-ASCII letters is fine for CMake and the linker)
    if (-not $SteamSdk) { throw '-WithSteam needs -SteamSdk <folder of the Steamworks SDK>' }
    $SteamSdk = [System.IO.Path]::GetFullPath($SteamSdk)
    $steamDll = Join-Path $SteamSdk ($manifest.steam.dll.Replace('/', '\'))
    if (-not (Test-Path $steamDll)) { throw "steam_api64.dll not found: $steamDll" }
    if (-not (Test-Path (Join-Path $SteamSdk 'public\steam\steam_api.h'))) { throw "public\steam\steam_api.h not found in $SteamSdk" }
}
if (-not $SkipBuild) {
    $vs = Find-VisualStudio
    $cache = Join-Path $buildPath 'CMakeCache.txt'
    if (-not (Test-Path $cache)) {
        Write-Step "Configuring $BuildDir ($($vs.Generator))"
        $configure = @('-S', $root, '-B', $buildPath, '-G', $vs.Generator, '-A', 'x64',
            '-DMYENGINE_SHIPPING=ON', '-DMYENGINE_BUILD_TESTS=OFF', '-DMYENGINE_COPY_RUNTIME_ASSETS=OFF')
        if ($WithSteam) { $configure += @('-DMYENGINE_STEAM=ON', "-DSTEAMWORKS_SDK_DIR=$($SteamSdk.Replace('\', '/'))") }
        Invoke-Native 'cmake' $configure
    }
    elseif (-not (Select-String -Path $cache -Pattern '^MYENGINE_SHIPPING:BOOL=ON' -Quiet)) {
        throw "$BuildDir was configured without MYENGINE_SHIPPING=ON: use another folder or delete it"
    }
    elseif ($WithSteam -and -not (Select-String -Path $cache -Pattern '^MYENGINE_STEAM:BOOL=ON' -Quiet)) {
        throw "$BuildDir was configured without MYENGINE_STEAM=ON: use another folder (-BuildDir) or delete it"
    }
    Write-Step 'Building the myengine target (Release)'
    Invoke-Native 'cmake' @('--build', $buildPath, '--config', 'Release', '--target', 'myengine')
}
if (-not (Test-Path $exe)) { throw "$exe not found" }
if ($WithSteam -and -not (Select-String -Path (Join-Path $buildPath 'CMakeCache.txt') -Pattern '^MYENGINE_STEAM:BOOL=ON' -Quiet)) {
    throw "$BuildDir was built without MYENGINE_STEAM=ON: the exe has no Steam code, steam_api64.dll next to it would do nothing"
}

# ---------------------------------------------------------------- assets by the manifest
function ConvertTo-Relative([string]$full) {
    $rootWithSlash = $root.TrimEnd('\') + '\'
    return $full.Substring($rootWithSlash.Length).Replace('\', '/')
}

function ConvertTo-ExcludeRegex([string]$glob) {
    $builder = New-Object System.Text.StringBuilder
    $i = 0
    while ($i -lt $glob.Length) {
        $c = $glob[$i]
        if ($c -eq '*') {
            if ($i + 1 -lt $glob.Length -and $glob[$i + 1] -eq '*') {
                if ($i + 2 -lt $glob.Length -and $glob[$i + 2] -eq '/') { [void]$builder.Append('(.*/)?'); $i += 3; continue }
                [void]$builder.Append('.*'); $i += 2; continue
            }
            [void]$builder.Append('[^/]*')
        }
        elseif ($c -eq '?') { [void]$builder.Append('[^/]') }
        else { [void]$builder.Append([regex]::Escape([string]$c)) }
        $i++
    }
    return '^' + $builder.ToString() + '$'
}

$excludes = @($manifest.exclude | ForEach-Object { [regex]::new((ConvertTo-ExcludeRegex $_), 'IgnoreCase') })
function Test-Excluded([string]$relative) {
    foreach ($pattern in $excludes) { if ($pattern.IsMatch($relative)) { return $true } }
    return $false
}

$files = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
$queue = New-Object 'System.Collections.Generic.Queue[string]'

function Add-AssetFile([string]$relative) {
    if ((Test-Excluded $relative) -or -not $files.Add($relative)) { return }
    $queue.Enqueue($relative)
}

Write-Step 'Collecting the assets'
foreach ($asset in $manifest.assets) {
    $full = Join-Path $root $asset
    if (Test-Path $full -PathType Container) {
        Get-ChildItem -Path $full -Recurse -File | ForEach-Object { Add-AssetFile (ConvertTo-Relative $_.FullName) }
    }
    elseif (Test-Path $full -PathType Leaf) { Add-AssetFile (ConvertTo-Relative $full) }
    else { throw "The manifest names a missing asset: $asset" }
}

# Everything a taken JSON file names: a path in quotes, relative to the repository or to the file itself
$referencePattern = [regex]::new('"([^"\\\r\n]+\.(?:obj|material\.json|shader\.json|tga|dds|bmp|png|jpg|hlsl|wav|ttf))"', 'IgnoreCase')
$missing = New-Object 'System.Collections.Generic.List[string]'
if ($manifest.followReferences) {
    while ($queue.Count -gt 0) {
        $relative = $queue.Dequeue()
        if (-not $relative.EndsWith('.json', [System.StringComparison]::OrdinalIgnoreCase)) { continue }
        $fullFile = Join-Path $root $relative
        $text = [System.IO.File]::ReadAllText($fullFile)
        foreach ($match in $referencePattern.Matches($text)) {
            $name = $match.Groups[1].Value
            $found = $false
            foreach ($baseDirectory in @($root, (Split-Path -Parent $fullFile))) {
                $candidate = [System.IO.Path]::GetFullPath((Join-Path $baseDirectory $name))
                if ($candidate.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase) -and (Test-Path $candidate -PathType Leaf)) {
                    Add-AssetFile (ConvertTo-Relative $candidate)
                    $found = $true
                    break
                }
            }
            if (-not $found -and ($name.StartsWith('assets/') -or $name.StartsWith('../'))) { $missing.Add("$relative -> $name") }
        }
    }
}
foreach ($line in $missing) { Write-Warning "Referenced file not found: $line" }
Write-Step "$($files.Count) asset files"

# ---------------------------------------------------------------- the folder
Write-Step "Assembling $dist"
$outRoot = $outDir.TrimEnd('\') + '\'
if (-not $dist.StartsWith($outRoot, [System.StringComparison]::OrdinalIgnoreCase)) { throw "Refusing to clean $dist" }
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force -Path $dist | Out-Null

Copy-Item $exe (Join-Path $dist $manifest.exe)
foreach ($runtimeFile in $manifest.runtime) {
    $source = Join-Path $root $runtimeFile
    if (-not (Test-Path $source)) { throw "Runtime file not found: $runtimeFile" }
    Copy-Item $source $dist
}

foreach ($relative in ($files | Sort-Object)) {
    $target = Join-Path $dist $relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item (Join-Path $root $relative) $target
}

# The Visual C++ runtime app-local (VC\Redist\MSVC\<version>\x64\Microsoft.VC*.CRT): the redistributable folder of Visual Studio
if ($null -eq $vs) { $vs = Find-VisualStudio }
$redistRoot = Join-Path $vs.Path 'VC\Redist\MSVC'
$crt = Get-ChildItem -Path $redistRoot -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^\d' } | Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Get-ChildItem -Path (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue } |
    Select-Object -First 1
if ($null -eq $crt) { throw "The Visual C++ redistributable folder was not found under $redistRoot" }
Copy-Item (Join-Path $crt.FullName '*.dll') $dist
Write-Step "Visual C++ runtime from $($crt.FullName)"

# config, project, readme, licenses
$appConfig = $manifest.config | ConvertTo-Json -Depth 8
Write-TextFile (Join-Path $dist 'config\app.json') $appConfig $false
$project = $manifest.project | ConvertTo-Json -Depth 4
Write-TextFile (Join-Path $dist 'myengine.myproject') $project $false

$readme = [System.IO.File]::ReadAllText((Join-Path $PSScriptRoot 'package_readme.txt')).Replace('{VERSION}', $version)
Write-TextFile (Join-Path $dist 'README.txt') $readme $true

$licenseDir = Join-Path $dist 'licenses'
New-Item -ItemType Directory -Force -Path $licenseDir | Out-Null
Copy-Item (Join-Path (Join-Path $root $manifest.licenses.notices) '*') $licenseDir
foreach ($license in $manifest.licenses.copy) {
    $source = Join-Path $root $license.from
    if (-not (Test-Path $source)) { throw "License file not found: $($license.from)" }
    Copy-Item $source (Join-Path $licenseDir $license.to)
}

$commit = 'unknown'
try { $commit = (& git -C $root rev-parse --short HEAD 2>$null | Out-String).Trim() } catch { }
Write-TextFile (Join-Path $dist 'version.txt') ("$gameName $version`r`ncommit $commit`r`n") $false

# Steam (only with the SDK; see docs/game/rl-online.md section 7.4)
if ($WithSteam) {
    Copy-Item $steamDll $dist
    [System.IO.File]::WriteAllText((Join-Path $dist 'steam_appid.txt'), $manifest.steam.appId, [System.Text.Encoding]::ASCII)
    Write-Step "Steam: steam_api64.dll and steam_appid.txt ($($manifest.steam.appId))"
}

# ---------------------------------------------------------------- size and zip
function Get-FolderSize([string]$path) {
    $sum = (Get-ChildItem -Path $path -Recurse -File | Measure-Object -Property Length -Sum).Sum
    if ($null -eq $sum) { return 0 }
    return $sum
}
$total = Get-FolderSize $dist
Write-Step ('{0}: {1:N1} MB' -f $dist, ($total / 1MB))
Get-ChildItem -Path $dist | ForEach-Object {
    $size = if ($_.PSIsContainer) { Get-FolderSize $_.FullName } else { $_.Length }
    Write-Host ('    {0,-28} {1,8:N1} MB' -f $_.Name, ($size / 1MB))
}

if (-not $NoZip) {
    $zip = Join-Path $outDir "$gameName-$version-win64.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory($dist, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $true)
    Write-Step ('{0}: {1:N1} MB' -f $zip, ((Get-Item $zip).Length / 1MB))
}
Write-Step 'Done'
