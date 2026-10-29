# Замеры звука XAudio2 (OFF) против Wwise (ON): игра --game, матч с ботами по сценарию MYENGINE_UI_SCRIPT, настоящее устройство. Один скрипт для обеих сторон.
# Запуск (сборки RelWithDebInfo двух сторон лежат вне рабочей копии, см. README.md):
#   powershell -File run_wwise_bench.ps1 -ExeDir <каталог с myengine.exe> -OutDir <куда класть> -Side off|on [-Runs 3] [-StartRun 1] [-Tracy] [-Seconds 90] [-TracyDir <папка tracy-capture>]
# Без -Tracy: процесс бежит без захвата, скрипт каждые 0.5 с снимает загрузку процессора и память (это CPU и рабочий набор «за прогон»).
# С -Tracy: перед игрой стартует tracy-capture (-f -s <Seconds>), трейс кладётся в <OutDir>\traces (зоны AudioSystem::Update, PlayEvent, кадр).
# Звук тихий: общая громкость 0.03 (-30 дБ): замеры идут ночью, а голоса при этом не становятся «виртуальными» (порог Wwise около -80 дБ), так что
# микшер работает в полную силу.

param(
    [Parameter(Mandatory = $true)][string]$ExeDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][ValidateSet("off", "on")][string]$Side,
    [int]$Runs = 3,
    [int]$StartRun = 1,
    [switch]$Tracy,
    [int]$Seconds = 90,
    [double]$Master = 0.03,
    [string]$TracyDir = ""
)

$ErrorActionPreference = "Stop"
if ($TracyDir -eq "") {
    foreach ($candidate in @((Join-Path $env:USERPROFILE "source\repos\tracy"), (Join-Path $env:USERPROFILE "Documents\apps\tracy"))) {
        if (Test-Path (Join-Path $candidate "tracy-capture.exe")) { $TracyDir = $candidate; break }
    }
}
$ExeDir = (Resolve-Path $ExeDir).Path
New-Item -ItemType Directory -Force $OutDir | Out-Null
$out = (Resolve-Path $OutDir).Path
$traces = Join-Path $out "traces"
New-Item -ItemType Directory -Force $traces | Out-Null

# Настройки игры для замера: окно 1280x720, VSync и лимит кадров выключены (меряется работа, а не ожидание), 2v2 против ботов, тихая громкость.
$settings = @{
    version = 1
    audio   = @{ master = $Master; effects = 1.0; music = 0.6 }
    video   = @{ windowMode = 0; width = 1280; height = 720; vsync = $false; fpsLimit = 0; shadows = 2; msaa = 4; bloom = $true; uiScalePercent = 100; showFps = $false }
    play    = @{ mode = 2; botLevel = 1 }
    car     = "fusion"
}
$settingsFile = Join-Path $out "game_settings.measure.json"
$settings | ConvertTo-Json -Depth 5 | Set-Content -Path $settingsFile -Encoding utf8

# Сценарий: главное меню -> PLAY -> три раза вниз до START MATCH -> матч 2v2 против ботов; в конце quit. Время от первого кадра, секунды.
# Игрок ничего не жмёт: боты играют сами, стартовый отсчёт 3 с, дальше мяч, моторы, буст, удары, трибуны.
$scriptFile = Join-Path $out "match.script"
@(
    "# ${Side}: menu, match 2v2, quit",
    "4.0 key Enter",
    "6.0 key Down",
    "6.4 key Down",
    "6.8 key Down",
    "8.0 key Enter",
    "$Seconds quit"
) | Set-Content -Path $scriptFile -Encoding ascii

function Invoke-Run([string]$tag) {
    $user = Join-Path $out "userdata\$tag"
    New-Item -ItemType Directory -Force (Join-Path $user "RocketArena") | Out-Null
    $env:MYENGINE_UI_SCRIPT = $scriptFile
    $env:MYENGINE_GAME_SETTINGS = $settingsFile
    $env:APPDATA = $user
    $env:LOCALAPPDATA = $user

    $capture = $null
    $trace = Join-Path $traces "$tag.tracy"
    if ($Tracy) {
        if (Test-Path $trace) { Remove-Item $trace -Force }
        $capture = Start-Process -FilePath (Join-Path $TracyDir "tracy-capture.exe") -ArgumentList "-o", "`"$trace`"", "-f", "-s", "$($Seconds + 20)" `
            -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $traces "$tag.capture.txt")
        Start-Sleep -Seconds 1
    }
    Remove-Item (Join-Path $ExeDir "logs\myengine.log") -Force -ErrorAction SilentlyContinue
    $started = Get-Date
    $engine = Start-Process -FilePath (Join-Path $ExeDir "myengine.exe") -WorkingDirectory $ExeDir -ArgumentList "--game" -PassThru
    $samples = New-Object System.Collections.Generic.List[object]
    while (-not $engine.HasExited) {
        try {
            $engine.Refresh()
            $samples.Add([pscustomobject]@{ t = ((Get-Date) - $started).TotalSeconds; cpu = $engine.TotalProcessorTime.TotalSeconds; ws = $engine.WorkingSet64; pm = $engine.PrivateMemorySize64 })
        } catch { }
        if (((Get-Date) - $started).TotalSeconds -gt ($Seconds + 60)) { Stop-Process -Id $engine.Id -Force; break }
        Start-Sleep -Milliseconds 500
    }
    $engine.WaitForExit()
    $wall = ((Get-Date) - $started).TotalSeconds
    $cpuTotal = $null
    try { $cpuTotal = $engine.TotalProcessorTime.TotalSeconds } catch { }
    if ($capture) { $null = $capture.WaitForExit(60000); if (-not $capture.HasExited) { Stop-Process -Id $capture.Id -Force } }

    # окно усреднения: от 20 с (матч идёт) до конца сценария
    $window = $samples | Where-Object { $_.t -ge 20 -and $_.t -le $Seconds }
    $cpuWindow = $null
    if ($window.Count -ge 2) {
        $first = $window | Select-Object -First 1
        $last = $window | Select-Object -Last 1
        $cpuWindow = ($last.cpu - $first.cpu) / ($last.t - $first.t) * 100.0
    }
    $result = [ordered]@{
        side = $Side; tag = $tag; tracy = [bool]$Tracy; exit = $engine.ExitCode; wallSeconds = [math]::Round($wall, 1)
        cpuSecondsTotal = [math]::Round($cpuTotal, 2)
        cpuPercentOfOneCoreInWindow = if ($cpuWindow -ne $null) { [math]::Round($cpuWindow, 1) } else { $null }
        workingSetMaxMB = [math]::Round((($samples | Measure-Object ws -Maximum).Maximum) / 1MB, 1)
        workingSetWindowMeanMB = if ($window.Count) { [math]::Round((($window | Measure-Object ws -Average).Average) / 1MB, 1) } else { $null }
        privateMaxMB = [math]::Round((($samples | Measure-Object pm -Maximum).Maximum) / 1MB, 1)
        samples = $samples.Count
    }
    $result | ConvertTo-Json | Set-Content -Path (Join-Path $out "$tag.run.json") -Encoding utf8
    # лог движка этого прогона (память Wwise в конце, строки звука): лежит рядом с exe, перезаписывается каждым запуском
    $log = Join-Path $ExeDir "logs\myengine.log"
    if (Test-Path $log) { Copy-Item $log (Join-Path $out "$tag.engine.log") -Force }
    "$tag exit=$($engine.ExitCode) wall=$([math]::Round($wall,1))s cpu=$($result.cpuSecondsTotal)s ws=$($result.workingSetMaxMB)MB"
    Start-Sleep -Seconds 3
}

for ($run = $StartRun; $run -lt $StartRun + $Runs; $run++) {
    $suffix = if ($Tracy) { "tracy" } else { "plain" }
    Invoke-Run "${Side}_${suffix}_run$run"
}
