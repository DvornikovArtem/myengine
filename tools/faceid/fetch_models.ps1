<#
.SYNOPSIS
  Downloads what Face ID needs (docs/editor/faceid.md): ONNX Runtime and the two models of OpenCV Zoo, and checks them by SHA-256.

.DESCRIPTION
  tools\faceid\fetch_models.ps1 [-Force] [-WithTestImage]

  external\onnxruntime\            include\, lib\onnxruntime.lib, onnxruntime.dll, LICENSE   (ONNX Runtime 1.20.1, MIT, official release of Microsoft)
  assets\faceid\models\            face_detection_yunet_2023mar.onnx (YuNet, MIT), face_recognition_sface_2021dec.onnx (SFace, Apache 2.0)
  assets\faceid\licenses\          the licence texts of the models

  All of it is in .gitignore. With the files in place CMake turns MYENGINE_FACEID on by itself. The script is ASCII on purpose: it has to work in
  Windows PowerShell 5.1 with a Cyrillic path to the repository. A file that is already here and has the right hash is not downloaded again.
#>
[CmdletBinding()]
param(
    [switch]$Force,
    [switch]$WithTestImage   # public domain portraits of two astronauts for the tests (assets\faceid\test\, also ignored by git)
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

$Downloads = @(
    @{ Name = 'onnxruntime-win-x64-1.20.1.zip'; Url = 'https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip';
       Sha256 = '78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581'; Kind = 'ort' },
    @{ Name = 'face_detection_yunet_2023mar.onnx'; Url = 'https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx';
       Sha256 = '8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4'; Kind = 'model' },
    @{ Name = 'face_recognition_sface_2021dec.onnx'; Url = 'https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/face_recognition_sface_2021dec.onnx';
       Sha256 = '0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79'; Kind = 'model' }
)
$Licenses = @(
    @{ Name = 'yunet-LICENSE.txt'; Url = 'https://raw.githubusercontent.com/opencv/opencv_zoo/main/models/face_detection_yunet/LICENSE' },
    @{ Name = 'sface-LICENSE.txt'; Url = 'https://raw.githubusercontent.com/opencv/opencv_zoo/main/models/face_recognition_sface/LICENSE' }
)

function Get-Sha256([string]$Path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant() }

function Get-Checked($Item, [string]$Target) {
    if ((Test-Path -LiteralPath $Target) -and -not $Force -and (Get-Sha256 $Target) -eq $Item.Sha256) {
        Write-Host ("[have] {0}" -f $Item.Name)
        return
    }
    Write-Host ("[get]  {0}" -f $Item.Name)
    $dir = Split-Path -Parent $Target
    New-Item -ItemType Directory -Force $dir | Out-Null
    Invoke-WebRequest -UseBasicParsing -Uri $Item.Url -OutFile $Target
    $hash = Get-Sha256 $Target
    if ($hash -ne $Item.Sha256) {
        Remove-Item -Force -LiteralPath $Target
        throw ("SHA-256 of {0} is {1}, expected {2}: the file is not the one the code was checked with" -f $Item.Name, $hash, $Item.Sha256)
    }
}

$modelsDir = Join-Path $Root 'assets\faceid\models'
$licensesDir = Join-Path $Root 'assets\faceid\licenses'
$ortDir = Join-Path $Root 'external\onnxruntime'
$cache = Join-Path $env:TEMP 'myengine-faceid-download'
New-Item -ItemType Directory -Force $cache, $modelsDir, $licensesDir | Out-Null

foreach ($item in $Downloads) {
    if ($item.Kind -eq 'model') {
        Get-Checked $item (Join-Path $modelsDir $item.Name)
    } else {
        $zip = Join-Path $cache $item.Name
        Get-Checked $item $zip
        if ($Force -or -not (Test-Path (Join-Path $ortDir 'lib\onnxruntime.lib'))) {
            $unpacked = Join-Path $cache 'ort'
            if (Test-Path -LiteralPath $unpacked) { Remove-Item -Recurse -Force -LiteralPath $unpacked }
            Expand-Archive -LiteralPath $zip -DestinationPath $unpacked -Force
            $inner = Get-ChildItem -LiteralPath $unpacked -Directory | Select-Object -First 1
            New-Item -ItemType Directory -Force (Join-Path $ortDir 'lib') | Out-Null
            Copy-Item -Recurse -Force (Join-Path $inner.FullName 'include') $ortDir
            foreach ($file in 'onnxruntime.lib', 'onnxruntime.dll', 'onnxruntime_providers_shared.dll') {
                Copy-Item -Force (Join-Path $inner.FullName "lib\$file") (Join-Path $ortDir 'lib')
            }
            foreach ($file in 'LICENSE', 'ThirdPartyNotices.txt') {
                if (Test-Path (Join-Path $inner.FullName $file)) { Copy-Item -Force (Join-Path $inner.FullName $file) $ortDir }
            }
            Write-Host "[ok]   external\onnxruntime"
        }
    }
}
foreach ($item in $Licenses) {
    $target = Join-Path $licensesDir $item.Name
    if ($Force -or -not (Test-Path -LiteralPath $target)) {
        Invoke-WebRequest -UseBasicParsing -Uri $item.Url -OutFile $target
    }
}
try { [System.IO.Directory]::Delete($cache, $true) } catch { }   # the download cache; a failure to clean it is not a failure

if ($WithTestImage) {
    # two portraits of the public domain (NASA, works of the US federal government): not members of the team
    $testDir = Join-Path $Root 'assets\faceid\test'
    New-Item -ItemType Directory -Force $testDir | Out-Null
    $portraits = @(
        @{ Name = 'public_domain_portrait.jpg'; Url = 'https://commons.wikimedia.org/wiki/Special:FilePath/Neil_Armstrong_pose.jpg?width=640' },
        @{ Name = 'public_domain_portrait2.jpg'; Url = 'https://commons.wikimedia.org/wiki/Special:FilePath/Buzz_Aldrin.jpg?width=640' }
    )
    foreach ($portrait in $portraits) {
        $target = Join-Path $testDir $portrait.Name
        if ($Force -or -not (Test-Path -LiteralPath $target)) {
            Invoke-WebRequest -UseBasicParsing -Uri $portrait.Url -OutFile $target
        }
    }
    # more portraits of the same two men, years apart and in other light: the tests of the faces from photos (assets\faceid\test\photos)
    $photoDir = Join-Path $testDir 'photos'
    New-Item -ItemType Directory -Force $photoDir | Out-Null
    $photos = @(
        @{ Name = 'armstrong1.jpg'; Title = 'Neil_Armstrong_1964_portrait.jpg' },
        @{ Name = 'armstrong2.jpg'; Title = 'Neil_Armstrong_1958_portrait.jpg' },
        @{ Name = 'armstrong3.jpg'; Title = 'Neil_Armstrong.jpg' },
        @{ Name = 'armstrong4.jpg'; Title = 'Astronaut_Neil_A._Armstrong_(1964).jpg' },
        @{ Name = 'armstrong5.jpg'; Title = 'Neil_Armstrong_2.jpg' },
        @{ Name = 'armstrong6.jpg'; Title = 'Portrait_of_Neil_Armstrong.jpg' },
        @{ Name = 'aldrin1.jpg'; Title = 'Buzz_Aldrin_(S69-31743).jpg' },
        @{ Name = 'aldrin2.jpg'; Title = 'PORTRAIT_-_ALDRIN,_EDWIN_E_,_JR_,_ASTRONAUT_(s63-20056).jpg' },
        @{ Name = 'aldrin3.jpg'; Title = 'Portrait_of_Buzz_Aldrin_during_suit-up_for_launch.jpg' },
        @{ Name = 'aldrin4.jpg'; Title = 'EdwinAldrin_big.jpg' },
        @{ Name = 'aldrin5.jpg'; Title = 'Buzz_Aldrin_(Apollo_11).jpg' }
    )
    foreach ($photo in $photos) {
        $target = Join-Path $photoDir $photo.Name
        if ($Force -or -not (Test-Path -LiteralPath $target)) {
            $url = 'https://commons.wikimedia.org/wiki/Special:FilePath/' + [System.Uri]::EscapeDataString($photo.Title) + '?width=640'
            Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $target -UserAgent 'myengine-faceid-test/1.0 (educational project)'
        }
    }
    Write-Host "[ok]   assets\faceid\test"
}
Write-Host "Face ID: dependencies are in place; configure CMake again (MYENGINE_FACEID turns on by itself)."
