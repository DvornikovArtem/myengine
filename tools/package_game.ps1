<#
.SYNOPSIS
    Builds the shipping game (RocketArena.exe) and packs it into dist/RocketArena and a zip for players.

.DESCRIPTION
    1. Configures build-shipping/ (MYENGINE_SHIPPING=ON, no tests, Release) and builds the myengine target. With the Steamworks SDK found
       (see -SteamSdk) the exe is built with Steam; without it, with a warning, the package has no Steam.
    2. Collects the assets by tools/package_manifest.json (roots plus everything the scenes, materials and data name).
    3. Puts together dist/RocketArena: exe, python314.dll / python314.zip, the Visual C++ runtime app-local,
       assets, config/app.json (starts the Rocket League map), myengine.myproject, README.txt, licenses/.
    4. Writes dist/RocketArena-<version>-win64-steam.zip (or -nosteam.zip). The version is the one of project(myengine VERSION ...) in the
       root CMakeLists.txt, the same as in the exe resources and in the menu.
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
    Steam is required: stop if the SDK is not found (without the switch the SDK is looked for, and a missing one is only a warning).
.PARAMETER NoSteam
    Do not look for the SDK: the package has no Steam (LAN and IP only).
.PARAMETER SteamSdk
    The folder of the unpacked Steamworks SDK (the one with redistributable_bin). Without it: the environment variable STEAMWORKS_SDK_DIR,
    the first line of tools\steam_sdk.txt (a local file), then the folders "sdk" / "steamworks_sdk" next to the repository (up the tree).
.PARAMETER CopyZipTo
    Also copy the zip to this folder (created if needed): the place of the files that go to friends.
#>
[CmdletBinding()]
param(
    [ValidateSet('Shipping', 'Release')][string]$Config = 'Shipping',
    [string]$Out = 'dist',
    [string]$BuildDir = 'build-shipping',
    [switch]$SkipBuild,
    [switch]$NoZip,
    [switch]$WithSteam,
    [switch]$NoSteam,
    [string]$SteamSdk = '',
    [string]$CopyZipTo = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outDir = [System.IO.Path]::GetFullPath((Join-Path $root $Out))
$buildPath = [System.IO.Path]::GetFullPath((Join-Path $root $BuildDir))
$manifestPath = Join-Path $PSScriptRoot 'package_manifest.json'
$manifest = Get-Content -Raw -Encoding UTF8 $manifestPath | ConvertFrom-Json
$gameName = $manifest.name
# one place of the version: project(myengine VERSION x.y.z) in the root CMakeLists.txt (the exe resources, the menu and the crash report read it from there too)
$versionMatch = [regex]::Match([System.IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt')), 'project\(\s*myengine\s+VERSION\s+(\d+\.\d+\.\d+)')
if (-not $versionMatch.Success) { throw 'project(myengine VERSION x.y.z) not found in CMakeLists.txt' }
$version = $versionMatch.Groups[1].Value
$dist = Join-Path $outDir $gameName
if ($WithSteam -and $NoSteam) { throw '-WithSteam and -NoSteam exclude each other' }

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

# ---------------------------------------------------------------- Steam: the SDK is looked for before the long build
function Test-SteamSdk([string]$folder) {
    return $folder -and (Test-Path (Join-Path $folder ($manifest.steam.dll.Replace('/', '\')))) -and (Test-Path (Join-Path $folder 'public\steam\steam_api.h'))
}

function Find-SteamSdk {
    $candidates = New-Object 'System.Collections.Generic.List[string]'
    if ($SteamSdk) { $candidates.Add($SteamSdk) }
    foreach ($name in @('STEAMWORKS_SDK_DIR', 'STEAMWORKS_SDK')) {
        $value = [Environment]::GetEnvironmentVariable($name)
        if ($value) { $candidates.Add($value) }
    }
    $localFile = Join-Path $PSScriptRoot 'steam_sdk.txt'
    if (Test-Path $localFile) {
        $first = (Get-Content -Encoding UTF8 -TotalCount 1 $localFile)
        if ($first) { $candidates.Add($first.Trim()) }
    }
    for ($folder = Split-Path -Parent $root; $folder; $folder = Split-Path -Parent $folder) {
        foreach ($name in @('sdk', 'steamworks_sdk', 'steamworks_sdk\sdk')) { $candidates.Add((Join-Path $folder $name)) }
    }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-SteamSdk $candidate)) { return [System.IO.Path]::GetFullPath($candidate) }
        if ($candidate -and $candidate -eq $SteamSdk) { throw "-SteamSdk $candidate is not the Steamworks SDK (public\steam\steam_api.h and $($manifest.steam.dll) are expected)" }
    }
    return $null
}

$useSteam = $false
$steamDll = $null
if (-not $NoSteam) {
    $found = Find-SteamSdk
    if ($found) {
        $useSteam = $true
        $SteamSdk = $found
        $steamDll = Join-Path $SteamSdk ($manifest.steam.dll.Replace('/', '\'))
        Write-Step "Steamworks SDK: $SteamSdk"
    }
    elseif ($WithSteam) { throw '-WithSteam: the Steamworks SDK was not found (-SteamSdk <folder>, STEAMWORKS_SDK_DIR or tools\steam_sdk.txt)' }
    else { Write-Warning 'The Steamworks SDK was not found: the package is built WITHOUT Steam (LAN and IP only). -SteamSdk <folder> or STEAMWORKS_SDK_DIR fixes that; docs/game/steam-setup.md.' }
}
$steamTag = if ($useSteam) { 'steam' } else { 'nosteam' }
# a build folder is configured for one of the two: the second one gets a folder of its own unless -BuildDir says otherwise
if (-not $PSBoundParameters.ContainsKey('BuildDir') -and -not $useSteam) { $BuildDir = 'build-shipping-nosteam'; $buildPath = [System.IO.Path]::GetFullPath((Join-Path $root $BuildDir)) }

# ---------------------------------------------------------------- build
$exe = Join-Path $buildPath 'app\Release\RocketArena.exe'
$vs = $null
if (-not $SkipBuild) {
    $vs = Find-VisualStudio
    $cache = Join-Path $buildPath 'CMakeCache.txt'
    if (Test-Path $cache) {
        # CMAKE_SUPPRESS_REGENERATION is on: a change of the CMake files (the version, the resources of the exe) reaches the build only by this
        Write-Step "Re-running CMake for $BuildDir"
        Invoke-Native 'cmake' @('-S', $root, '-B', $buildPath)
    }
    if (-not (Test-Path $cache)) {
        Write-Step "Configuring $BuildDir ($($vs.Generator))"
        $configure = @('-S', $root, '-B', $buildPath, '-G', $vs.Generator, '-A', 'x64',
            '-DMYENGINE_SHIPPING=ON', '-DMYENGINE_BUILD_TESTS=OFF', '-DMYENGINE_COPY_RUNTIME_ASSETS=OFF')
        if ($useSteam) { $configure += @('-DMYENGINE_STEAM=ON', "-DSTEAMWORKS_SDK_DIR=$($SteamSdk.Replace('\', '/'))") }
        Invoke-Native 'cmake' $configure
    }
    elseif (-not (Select-String -Path $cache -Pattern '^MYENGINE_SHIPPING:BOOL=ON' -Quiet)) {
        throw "$BuildDir was configured without MYENGINE_SHIPPING=ON: use another folder or delete it"
    }
    elseif ($useSteam -and -not (Select-String -Path $cache -Pattern '^MYENGINE_STEAM:BOOL=ON' -Quiet)) {
        throw "$BuildDir was configured without MYENGINE_STEAM=ON: use another folder (-BuildDir) or delete it"
    }
    elseif (-not $useSteam -and (Select-String -Path $cache -Pattern '^MYENGINE_STEAM:BOOL=ON' -Quiet)) {
        throw "$BuildDir was configured with MYENGINE_STEAM=ON, and this package is without Steam: use another folder (-BuildDir) or delete it"
    }
    Write-Step 'Building the myengine target (Release)'
    Invoke-Native 'cmake' @('--build', $buildPath, '--config', 'Release', '--target', 'myengine')
}
if (-not (Test-Path $exe)) { throw "$exe not found" }
if ($useSteam -and -not (Select-String -Path (Join-Path $buildPath 'CMakeCache.txt') -Pattern '^MYENGINE_STEAM:BOOL=ON' -Quiet)) {
    throw "$BuildDir was built without MYENGINE_STEAM=ON: the exe has no Steam code, steam_api64.dll next to it would do nothing"
}
# the exe says the same version as the project (the resources of the exe: app/resources/RocketArena.rc.in)
$exeInfo = (Get-Item $exe).VersionInfo
if ($exeInfo.ProductVersion -notlike "$version*") { throw "The exe says version '$($exeInfo.ProductVersion)', the project says ${version}: reconfigure and rebuild $BuildDir" }

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
# the symbols of this very exe for reading its crash dumps: next to the package, not in it
$pdb = [System.IO.Path]::ChangeExtension($exe, '.pdb')
if (Test-Path $pdb) {
    $symbols = Join-Path $outDir 'symbols'
    New-Item -ItemType Directory -Force -Path $symbols | Out-Null
    Copy-Item -Force $pdb (Join-Path $symbols "$gameName-$version-$steamTag.pdb")
}
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
$readme = $readme.Replace('{STEAM}', $(if ($useSteam) { 'yes' } else { 'no' }))
$readme = $readme -replace "`r?`n", "`r`n"
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
Write-TextFile (Join-Path $dist 'version.txt') ("$gameName $version`r`ncommit $commit`r`nsteam $(if ($useSteam) { 'yes' } else { 'no' })`r`n") $false

# Steam (only with the SDK; see docs/game/rl-online.md section 7.4)
if ($useSteam) {
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
    $zip = Join-Path $outDir "$gameName-$version-win64-$steamTag.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory($dist, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $true)
    Write-Step ('{0}: {1:N1} MB' -f $zip, ((Get-Item $zip).Length / 1MB))
    if ($CopyZipTo) {
        $copyDir = [System.IO.Path]::GetFullPath($CopyZipTo)
        New-Item -ItemType Directory -Force -Path $copyDir | Out-Null
        Copy-Item -Force $zip $copyDir
        Write-Step "Copied to $copyDir"
    }
}
Write-Step 'Done'
