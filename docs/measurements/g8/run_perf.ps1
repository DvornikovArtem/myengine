# G8: замер кадра матча 4v4 в --game. Один скрипт для «до» и «после»: меняются только каталог сборки и каталог результатов.
# Запуск из корня репозитория:
#   powershell -File docs\measurements\g8\run_perf.ps1 -ExeDir <каталог с myengine.exe, RelWithDebInfo> -OutDir <куда класть> [-Scenario offline|online] [-Runs 3] [-Seconds 62] [-SimLatency 25] [-Settings <файл>]
# Нужны сборка RelWithDebInfo (Tracy включён только в ней) и tracy-capture.exe (-TracyDir, по умолчанию Documents\apps\tracy).
# Игра запускается со своей папкой пользователя (APPDATA во временной папке), настройками из -Settings и скриптом сценария (MYENGINE_UI_SCRIPT),
# так что настройки и результаты человека не трогаются. Онлайн: хост (порт Tracy -TracyPort) и клиент (-TracyPort + 1) в двух окнах на одном ПК,
# --net-sim latency=<SimLatency> в обоих (RTT около 4 x latency), Tracy снимает оба.
# Разбор: python docs\measurements\g8\analyze_perf.py <OutDir>

param(
    [Parameter(Mandatory = $true)][string]$ExeDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$Scenario = "offline",
    [int]$Runs = 3,
    [int]$Seconds = 62,
    [int]$SimLatency = 25,
    [string]$NetSimExtra = "",             # more of --net-sim, e.g. "jitter=30,loss=3"
    [string]$OfflineScript = "scenario-offline.txt",    # the script of the offline run
    [string]$ClientScript = "scenario-client.txt",   # "scenario-client-active.txt": the guest drives (rollbacks)
    [string]$Settings = "",
    [string]$ClientSettings = "",             # online: the settings of the guest (the host keeps -Settings): one player is one PC, here the host plays without a limit and does not hold the GPU
    [int]$TracyPort = 8186,   # not the default 8086: other engines of the team may run on this PC with Tracy attached
    [string]$TracyDir = (Join-Path $env:USERPROFILE "Documents\apps\tracy"),
    [string[]]$ExtraArgs = @()
)

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($Settings -eq "") { $Settings = Join-Path $here "settings-perf.json" }
if ($ClientSettings -eq "") { $ClientSettings = $Settings }
$ExeDir = (Resolve-Path $ExeDir).Path
New-Item -ItemType Directory -Force $OutDir | Out-Null
$traces = Join-Path (Resolve-Path $OutDir).Path "traces"
New-Item -ItemType Directory -Force $traces | Out-Null

function New-UserFolder([string]$name, [string]$settingsFile) {
    $folder = Join-Path (Split-Path -Parent $traces) "user_$name"   # not in %TEMP%: its 8.3 name (~1) does not go with Remove-Item
    if (Test-Path $folder) { Remove-Item -Recurse -Force -LiteralPath $folder }
    New-Item -ItemType Directory -Force $folder | Out-Null
    Copy-Item $settingsFile (Join-Path $folder "game_settings.json")
    return $folder
}

function Start-Game([string]$name, [string]$scriptFile, [string[]]$gameArgs, [int]$port, [string]$settingsFile) {
    $folder = New-UserFolder $name $settingsFile
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = Join-Path $ExeDir "myengine.exe"
    $startInfo.WorkingDirectory = $ExeDir
    $startInfo.Arguments = ($gameArgs + $ExtraArgs) -join " "
    $startInfo.UseShellExecute = $false
    $startInfo.EnvironmentVariables["APPDATA"] = $folder
    $startInfo.EnvironmentVariables["MYENGINE_GAME_SETTINGS"] = Join-Path $folder "game_settings.json"
    $startInfo.EnvironmentVariables["MYENGINE_UI_SCRIPT"] = $scriptFile
    $startInfo.EnvironmentVariables["TRACY_PORT"] = "$port"
    return [System.Diagnostics.Process]::Start($startInfo)
}

function Start-Capture([string]$trace, [int]$port) {
    if (Test-Path $trace) { Remove-Item -LiteralPath $trace -Force }
    return Start-Process -FilePath (Join-Path $TracyDir "tracy-capture.exe") -ArgumentList "-o", "`"$trace`"", "-f", "-s", "$Seconds", "-p", "$port" `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput ($trace + ".capture.txt")
}

function Get-BackgroundLoad() {
    # the load of the processor by everything else before the run (other programs on this PC distort the frame time): the mean of 5 samples, percent
    $samples = (Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 5).CounterSamples | ForEach-Object { $_.CookedValue }
    return [math]::Round(($samples | Measure-Object -Average).Average, 1)
}

for ($run = 1; $run -le $Runs; $run++) {
    $load = Get-BackgroundLoad
    "$Scenario run ${run}: background load before the run $load %"
    "$load" | Out-File -Encoding ascii (Join-Path $traces "${Scenario}_run${run}.load.txt")
    if ($Scenario -eq "offline") {
        $trace = Join-Path $traces "offline_run$run.tracy"
        $capture = Start-Capture $trace $TracyPort
        Start-Sleep -Seconds 1
        $engine = Start-Game "offline" (Join-Path $here $OfflineScript) @("--game") $TracyPort $Settings
        $null = $capture.WaitForExit(($Seconds + 60) * 1000)
        if (-not $engine.HasExited) { Stop-Process -Id $engine.Id -Force }
        "offline run $run capture exit=$($capture.ExitCode) size=$((Get-Item $trace -ErrorAction SilentlyContinue).Length)"
    }
    else {
        $hostTrace = Join-Path $traces "online_host_run$run.tracy"
        $clientTrace = Join-Path $traces "online_client_run$run.tracy"
        $hostCapture = Start-Capture $hostTrace $TracyPort
        $clientCapture = Start-Capture $clientTrace ($TracyPort + 1)
        Start-Sleep -Seconds 1
        $sim = "latency=$SimLatency"
        if ($NetSimExtra -ne "") { $sim = "$sim,$NetSimExtra" }
        $hostEngine = Start-Game "host" (Join-Path $here "scenario-host.txt") @("--game", "--rl-host", "47777", "--rl-mode", "4", "--rl-clients", "1", "--net-sim", $sim) $TracyPort $Settings
        Start-Sleep -Seconds 4
        $clientEngine = Start-Game "client" (Join-Path $here $ClientScript) @("--game", "--rl-join", "127.0.0.1:47777", "--net-sim", $sim) ($TracyPort + 1) $ClientSettings
        $null = $hostCapture.WaitForExit(($Seconds + 60) * 1000)
        $null = $clientCapture.WaitForExit(60 * 1000)
        foreach ($engine in @($hostEngine, $clientEngine)) { if (-not $engine.HasExited) { Stop-Process -Id $engine.Id -Force } }
        "online run $run host exit=$($hostCapture.ExitCode) size=$((Get-Item $hostTrace -ErrorAction SilentlyContinue).Length); client exit=$($clientCapture.ExitCode) size=$((Get-Item $clientTrace -ErrorAction SilentlyContinue).Length)"
    }
    Start-Sleep -Seconds 3
}
