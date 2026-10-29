<#
.SYNOPSIS
    Downloads Freedoom (the free game data for FlyDoom) into assets/doom.

.DESCRIPTION
    Freedoom is a free replacement of the data of DOOM (a modified BSD license, assets/doom/COPYING-freedoom.txt after the download).
    The script takes the release zip from GitHub, checks its SHA-256 against the value pinned below, and unpacks freedoom1.wad
    (the Phase 1 episodes, the replacement of Ultimate DOOM) and the license into assets/doom. The files are not in git (.gitignore).
    The original DOOM.WAD is never downloaded and never copied: FlyDoom finds the one the player owns (docs/game/doom.md).
.PARAMETER Force
    Download again even when assets/doom/freedoom1.wad is already there.
#>
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$version = '0.13.0'
$url = "https://github.com/freedoom/freedoom/releases/download/v${version}/freedoom-${version}.zip"
$sha256 = '3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$target = Join-Path $root 'assets\doom'
$wad = Join-Path $target 'freedoom1.wad'

if ((Test-Path $wad) -and -not $Force) {
    Write-Host "freedoom1.wad is already in $target (use -Force to download again)"
    exit 0
}

New-Item -ItemType Directory -Force -Path $target | Out-Null
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("freedoom-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
    $zip = Join-Path $work 'freedoom.zip'
    Write-Host "Downloading $url"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing

    $actual = (Get-FileHash -Algorithm SHA256 -Path $zip).Hash.ToLowerInvariant()
    if ($actual -ne $sha256) {
        throw "SHA-256 of the download is $actual, expected $sha256. Nothing was unpacked."
    }
    Write-Host 'SHA-256 matches'

    $out = Join-Path $work 'unpacked'
    Expand-Archive -Path $zip -DestinationPath $out -Force
    $inner = Join-Path $out "freedoom-${version}"
    Copy-Item (Join-Path $inner 'freedoom1.wad') $wad -Force
    Copy-Item (Join-Path $inner 'COPYING.txt') (Join-Path $target 'COPYING-freedoom.txt') -Force
    Copy-Item (Join-Path $inner 'CREDITS.txt') (Join-Path $target 'CREDITS-freedoom.txt') -Force
    Write-Host "Freedoom ${version} is in $target"
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
