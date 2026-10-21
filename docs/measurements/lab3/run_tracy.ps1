# Tracy-замеры ЛР3: tracy-capture -f -s <секунд> на каждый прогон. Один скрипт для «до» и «после»: меняются только каталог сборки и каталог результатов.
# Запуск из корня репозитория:
#   powershell -File docs\measurements\lab3\run_tracy.ps1 -ExeDir <каталог с myengine.exe, RelWithDebInfo> -OutDir <куда класть> [-Seconds 25] [-Runs 3] [-Scenes benchmark,rocket_league] [-ReloadRuns 3]
# Нужны сборка RelWithDebInfo (TRACY_ENABLE только в ней) и tracy-capture.exe (-TracyDir).
# Кроме обычных прогонов делает -ReloadRuns прогонов benchmark с перезагрузкой шейдера на 12-й секунде после старта движка: у файла
# assets\shaders\textured_lit.hlsl в корне проекта (три каталога выше -ExeDir) переставляется время изменения, содержимое не меняется;
# движок опрашивает время файлов раз в 500 мс и перезагружает шейдер. После прогона время файла возвращается. -ReloadOnly делает только эти прогоны.

param(
    [Parameter(Mandatory = $true)][string]$ExeDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [int]$Seconds = 25,
    [int]$Runs = 3,
    [int]$ReloadRuns = 3,
    [int]$ReloadAt = 12,
    [switch]$ReloadOnly,
    [string[]]$Scenes = @("benchmark", "rocket_league"),
    [string]$TracyDir = ""
)

if ($TracyDir -eq "") {
    foreach ($candidate in @((Join-Path $env:USERPROFILE "source\repos\tracy"), (Join-Path $env:USERPROFILE "Documents\apps\tracy"))) {
        if (Test-Path (Join-Path $candidate "tracy-capture.exe")) { $TracyDir = $candidate; break }
    }
}
$ExeDir = (Resolve-Path $ExeDir).Path
New-Item -ItemType Directory -Force $OutDir | Out-Null
$traces = Join-Path (Resolve-Path $OutDir).Path "traces"
New-Item -ItemType Directory -Force $traces | Out-Null
$projectRoot = (Resolve-Path (Join-Path $ExeDir "..\..\..")).Path
$shaderFile = Join-Path $projectRoot "assets\shaders\textured_lit.hlsl"

function Invoke-Capture([string]$tag, [string]$scene, [bool]$reload) {
    $trace = Join-Path $traces "$tag.tracy"
    if (Test-Path $trace) { Remove-Item $trace -Force }

    # capture ждёт клиента, движок стартует следом (TRACY_ON_DEMAND)
    $capture = Start-Process -FilePath (Join-Path $TracyDir "tracy-capture.exe") `
        -ArgumentList "-o", "`"$trace`"", "-f", "-s", "$Seconds" -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $traces "$tag.capture.txt")
    Start-Sleep -Seconds 1
    $engine = Start-Process -FilePath (Join-Path $ExeDir "myengine.exe") -WorkingDirectory $ExeDir `
        -ArgumentList "--scene", "assets/scenes/$scene.json", "--play" -PassThru
    $originalTime = $null
    if ($reload) {
        Start-Sleep -Seconds $ReloadAt
        $originalTime = (Get-Item $shaderFile).LastWriteTimeUtc
        (Get-Item $shaderFile).LastWriteTimeUtc = [DateTime]::UtcNow
    }
    $null = $capture.WaitForExit(($Seconds + 60) * 1000)
    if (-not $engine.HasExited) { Stop-Process -Id $engine.Id -Force }
    if ($originalTime -ne $null) { (Get-Item $shaderFile).LastWriteTimeUtc = $originalTime }
    "$tag capture exit=$($capture.ExitCode) size=$((Get-Item $trace -ErrorAction SilentlyContinue).Length)"
    Start-Sleep -Seconds 2
}

if (-not $ReloadOnly) {
    foreach ($scene in $Scenes) {
        for ($run = 1; $run -le $Runs; $run++) {
            Invoke-Capture "${scene}_run$run" $scene $false
        }
    }
}
for ($run = 1; $run -le $ReloadRuns; $run++) {
    Invoke-Capture "benchmark_reload$run" "benchmark" $true
}
