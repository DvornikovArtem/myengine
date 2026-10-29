<#
.SYNOPSIS
    Downloads the MaleCNS v1.0 connectome and converts it to assets/flybrain/malecns_v1.fbrain (docs/flybrain/flybrain.md).

.DESCRIPTION
    Two sources of the same release (CC BY 4.0):
      -Source hf       (default) the HuggingFace mirror QuixiAI/MaleCNS: model.safetensors (2.7 GB, the edge list of all
                       88 million bodies), neurons.parquet, neurotransmitters.parquet. Saved as hf-*.
      -Source janelia  the Janelia flat connectome: three feather files (1.1 GB). The Google bucket answers 403 in some
                       regions; use the mirror there.

    1. Takes the files from -SourceDir when they are there (downloaded by hand, never deleted unless -DeleteSource),
       otherwise downloads them into -WorkDir with curl.exe (resumes a broken download). Size and sha256 are checked.
    2. Checks the free space: 3 GB must stay free on the disk of -WorkDir after the download and the venv.
    3. Creates a temporary venv in -WorkDir with pyarrow and numpy and runs tools/flybrain/convert_malecns.py, which keeps
       the traced neurons and stops unless the counts are 164,789 neurons, 25,563,197 edges, 124,025,046 contacts.
    4. Deletes what it downloaded itself and the venv (-KeepRaw keeps them).

    The result (about 155 MB) and its report (malecns_v1.report.json) are not in git: see .gitignore.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\flybrain\fetch_malecns.ps1 -WorkDir D:\flybrain-tmp

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\flybrain\fetch_malecns.ps1 -SourceDir D:\flydoom-n0\raw
#>
param(
    [ValidateSet('hf', 'janelia')]
    [string]$Source = 'hf',
    [string]$SourceDir = '',
    [string]$WorkDir = '',
    [string]$Output = '',
    [string]$Python = '',
    [switch]$KeepRaw,
    [switch]$DeleteSource
)

$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
if (-not $Output) { $Output = Join-Path $repo 'assets\flybrain\malecns_v1.fbrain' }

$sources = @{
    hf = @(
        @{ Name = 'hf-model.safetensors'; Url = 'https://huggingface.co/QuixiAI/MaleCNS/resolve/main/model.safetensors'; Bytes = 2729988074; Sha256 = 'ad22a84eb8fd7358af29e691e01183b6a7ffecb9e1a5242b88cd2923cc7d792f' },
        @{ Name = 'hf-neurons.parquet'; Url = 'https://huggingface.co/QuixiAI/MaleCNS/resolve/main/neurons.parquet'; Bytes = 7073048; Sha256 = '41b25faea22c856742e9b1e4aec3816d1c05a1a53d2f6a5b1969066a733df8db' },
        @{ Name = 'hf-neurotransmitters.parquet'; Url = 'https://huggingface.co/QuixiAI/MaleCNS/resolve/main/neurotransmitters.parquet'; Bytes = 15704785; Sha256 = '413af83a11a22c6ea437e11e1dc0c9b1cf89cb39678cf7e994412f78da910bb2' }
    )
    janelia = @(
        @{ Name = 'body-annotations-male-cns-v1.0-minconf-0.5.feather'; Url = 'https://storage.googleapis.com/flyem-male-cns/v1.0/connectome-data/flat-connectome/body-annotations-male-cns-v1.0-minconf-0.5.feather'; Bytes = 14483314; Sha256 = '2177e246113e4cfbf1e7772ec37c6da1955ff22e8063d0b1f833101f99a9a3b2' },
        @{ Name = 'body-neurotransmitters-male-cns-v1.0.feather'; Url = 'https://storage.googleapis.com/flyem-male-cns/v1.0/connectome-data/flat-connectome/body-neurotransmitters-male-cns-v1.0.feather'; Bytes = 43282834; Sha256 = '95c9289220663abeb3409f3ad9e5a7f8a53f8093f5139d15502cd08da8879621' },
        @{ Name = 'connectome-weights-male-cns-v1.0-minconf-0.5.feather'; Url = 'https://storage.googleapis.com/flyem-male-cns/v1.0/connectome-data/flat-connectome/connectome-weights-male-cns-v1.0-minconf-0.5.feather'; Bytes = 1051241946; Sha256 = 'e35da783d1c686b2b58b3b87cd6a403ae43bfcfba8bff28e08ef752c1a56afc1' }
    )
}
$files = $sources[$Source]

function Write-Step([string]$text) { Write-Host "[flybrain] $text" }

function Get-FreeGb([string]$path) {
    $full = [System.IO.Path]::GetFullPath($path)
    $drive = New-Object System.IO.DriveInfo ([System.IO.Path]::GetPathRoot($full))
    return [math]::Round($drive.AvailableFreeSpace / 1GB, 1)
}

function Test-Source([string]$path, $file) {
    if (-not (Test-Path -LiteralPath $path)) { return 'missing' }
    $size = (Get-Item -LiteralPath $path).Length
    if ($size -ne $file.Bytes) { return "size $size, expected $($file.Bytes)" }
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $file.Sha256) { return "sha256 $hash, expected $($file.Sha256)" }
    return ''
}

# The team keeps the downloaded mirror on the data disk (D:\flydoom-n0\raw): take it when nothing else is named
if (-not $SourceDir -and (Test-Path -LiteralPath (Join-Path 'D:\flydoom-n0\raw' $files[0].Name))) { $SourceDir = 'D:\flydoom-n0\raw' }
if (-not $WorkDir) {
    $WorkDir = if ($SourceDir) { Join-Path (Split-Path -Parent ([System.IO.Path]::GetFullPath($SourceDir))) 'flybrain-work' } else { Join-Path $env:TEMP 'myengine-flybrain' }
}
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Output) | Out-Null

# ---------------------------------------------------------------- space
$downloadGb = 0
if (-not $SourceDir) { foreach ($file in $files) { $downloadGb += $file.Bytes / 1GB } }
$needGb = [math]::Round($downloadGb + 0.4, 1) # plus the venv
$freeWork = Get-FreeGb $WorkDir
$freeOut = Get-FreeGb $Output
Write-Step "source: $Source; free space: $freeWork GB at $WorkDir, $freeOut GB at $(Split-Path -Parent $Output)"
# The rule of the team: at least 3 GB must stay free on the disk after the download
if ($freeWork - $needGb -lt 3) {
    throw "Not enough space at ${WorkDir}: $freeWork GB free, $needGb GB needed and 3 GB must stay free. Pass -WorkDir on another disk."
}
if ($freeOut -lt 0.5) { throw "Not enough space for $Output ($freeOut GB free, 0.5 GB needed)" }

# ---------------------------------------------------------------- data
$rawDir = if ($SourceDir) { $SourceDir } else { Join-Path $WorkDir 'raw' }
New-Item -ItemType Directory -Force -Path $rawDir | Out-Null
foreach ($file in $files) {
    $path = Join-Path $rawDir $file.Name
    Write-Step "$($file.Name): checking"
    $problem = Test-Source $path $file
    if ($problem -eq '') {
        Write-Step "$($file.Name): present, sha256 ok"
        continue
    }
    if ($SourceDir) { throw "$path : $problem. Download it from $($file.Url)" }

    Write-Step "$($file.Name): downloading $([math]::Round($file.Bytes / 1MB)) MB"
    # curl.exe ships with Windows 10+; -C - resumes a broken download
    & curl.exe --fail --location --retry 5 --continue-at - --output $path $file.Url
    if ($LASTEXITCODE -ne 0) {
        throw "Download of $($file.Name) failed (curl exit code $LASTEXITCODE). If the server answers 403 'not available in your location', use -Source hf or download the files by hand and pass their folder with -SourceDir."
    }
    $problem = Test-Source $path $file
    if ($problem -ne '') { throw "$($file.Name) after download: $problem" }
    Write-Step "$($file.Name): sha256 ok"
}

# ---------------------------------------------------------------- Python
$venv = Join-Path $WorkDir 'venv'
$venvPython = Join-Path $venv 'Scripts\python.exe'
if (-not (Test-Path $venvPython)) {
    $base = $Python
    if (-not $base) {
        if (Get-Command py -ErrorAction SilentlyContinue) { $base = 'py' }
        elseif (Get-Command python -ErrorAction SilentlyContinue) { $base = 'python' }
        else { throw 'Python 3.10+ is needed for the conversion: install it or pass -Python <path to python.exe>' }
    }
    Write-Step "creating a temporary venv in $venv"
    if ($base -eq 'py') { & py -3 -m venv $venv } else { & $base -m venv $venv }
    if ($LASTEXITCODE -ne 0) { throw 'venv creation failed' }
}
Write-Step 'installing pyarrow and numpy into the venv'
& $venvPython -m pip install --quiet --disable-pip-version-check pyarrow numpy
if ($LASTEXITCODE -ne 0) { throw 'pip install pyarrow numpy failed' }

# ---------------------------------------------------------------- conversion
$converter = Join-Path $PSScriptRoot 'convert_malecns.py'
Write-Step "converting to $Output"
# -I: isolated mode, nothing is imported from the current folder or the data folder
& $venvPython -I $converter --source $rawDir --out $Output
if ($LASTEXITCODE -ne 0) { throw 'conversion failed' }

# ---------------------------------------------------------------- cleanup
if ($KeepRaw) {
    Write-Step "kept the source files in $rawDir and the venv in $venv (-KeepRaw)"
}
else {
    if (-not $SourceDir -or $DeleteSource) {
        foreach ($file in $files) { Remove-Item -LiteralPath (Join-Path $rawDir $file.Name) -Force -ErrorAction SilentlyContinue }
        Write-Step "deleted the source files from $rawDir"
        if (-not $SourceDir) { Remove-Item -LiteralPath $rawDir -Recurse -Force -ErrorAction SilentlyContinue }
    }
    else {
        Write-Step "the source files in $SourceDir stay (pass -DeleteSource to delete them)"
    }
    Remove-Item -LiteralPath $venv -Recurse -Force -ErrorAction SilentlyContinue
    if (@(Get-ChildItem -LiteralPath $WorkDir -Force).Count -eq 0) { Remove-Item -LiteralPath $WorkDir -Force }
    Write-Step 'deleted the temporary venv'
}
$size = [math]::Round((Get-Item -LiteralPath $Output).Length / 1MB, 1)
Write-Step "done: $Output ($size MB). Attribution (CC BY 4.0): docs/flybrain/flybrain.md"
