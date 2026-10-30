<#
.SYNOPSIS
  Puts the public key of the publisher into the engine (docs/editor/faceid.md, "The shared database").

.DESCRIPTION
  tools\faceid\set_trusted_key.ps1 -Key <128 hex digits>
  tools\faceid\set_trusted_key.ps1 -File <a text file with the key>
  tools\faceid\set_trusted_key.ps1 -Clear

  Window > Face ID > Publisher > "Copy the public key" puts the 128 hex digits on the clipboard. This script writes them into
  engine\include\myengine\faceid\FaceIdTrustedKey.h; after that the engine has to be built again, and every copy built from it accepts
  only databases that are signed with the private key of this public key. The PUBLIC key is not a secret: the header may go to git.
  The private key (%LOCALAPPDATA%\myengine\faceid\signing.key) is never touched here.
  The script is ASCII on purpose: it has to work in Windows PowerShell 5.1 with a Cyrillic path to the repository.
#>
[CmdletBinding()]
param(
    [string]$Key,
    [string]$File,
    [switch]$Clear
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Header = Join-Path $Root 'engine\include\myengine\faceid\FaceIdTrustedKey.h'

if ($Clear) {
    $Key = ''
} else {
    if ($File) {
        $Key = (Get-Content -LiteralPath $File -Raw)
    }
    if ($null -eq $Key -or $Key -eq '') {
        throw 'Give the key: -Key <128 hex digits>, -File <file> or -Clear.'
    }
    $Key = ($Key -replace '\s', '').ToLowerInvariant()
    if ($Key -notmatch '^[0-9a-f]{128}$') {
        throw 'The key must be exactly 128 hex digits (X and Y of the point of ECDSA P-256), as "Copy the public key" gives it.'
    }
}

$text = [System.IO.File]::ReadAllText($Header)
$pattern = '(inline constexpr char kTrustedPublicKeyHex\[\] = ")[0-9a-fA-F]*(";)'
if ($text -notmatch $pattern) {
    throw "kTrustedPublicKeyHex was not found in $Header"
}
$updated = [regex]::Replace($text, $pattern, ('${1}' + $Key + '${2}'))
[System.IO.File]::WriteAllText($Header, $updated, (New-Object System.Text.UTF8Encoding($false)))

if ($Key -eq '') {
    Write-Host 'The built-in key is removed: no shared database is accepted until a key is put in.'
} else {
    Write-Host "The key is in $Header"
    Write-Host 'Build the engine again (build.bat), then sign the database with this publisher key.'
}
