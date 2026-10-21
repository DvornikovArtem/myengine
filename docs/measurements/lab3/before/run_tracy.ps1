# Tracy-замеры «до» ЛР3 (A0): tracy-capture -f -s <секунд>, затем csv-выгрузка зон tracy-csvexport.
# Запуск из корня репозитория:
#   powershell -File docs\measurements\lab3\before\run_tracy.ps1 [-Seconds 25] [-Runs 3] [-Scenes benchmark,rocket_league]
# Нужны сборка RelWithDebInfo и tracy-capture/tracy-csvexport (TracyDir).

param(
    [int]$Seconds = 25,
    [int]$Runs = 3,
    [string[]]$Scenes = @("benchmark", "rocket_league"),
    [string]$TracyDir = (Join-Path $env:USERPROFILE "Documents\apps\tracy")
)

$root = Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")
$exeDir = Join-Path $root "build\app\RelWithDebInfo"
$traces = Join-Path $PSScriptRoot "traces"
New-Item -ItemType Directory -Force $traces | Out-Null

foreach ($scene in $Scenes) {
    for ($run = 1; $run -le $Runs; $run++) {
        $tag = "${scene}_run$run"
        $trace = Join-Path $traces "$tag.tracy"
        if (Test-Path $trace) { Remove-Item $trace -Force }

        # capture ждёт клиента, движок стартует следом (TRACY_ON_DEMAND)
        $capture = Start-Process -FilePath (Join-Path $TracyDir "tracy-capture.exe") `
            -ArgumentList "-o", "`"$trace`"", "-f", "-s", "$Seconds" -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput (Join-Path $traces "$tag.capture.txt")
        Start-Sleep -Seconds 1
        $engine = Start-Process -FilePath (Join-Path $exeDir "myengine.exe") -WorkingDirectory $exeDir `
            -ArgumentList "--scene", "assets/scenes/$scene.json", "--play" -PassThru
        $null = $capture.WaitForExit(($Seconds + 60) * 1000)
        if (-not $engine.HasExited) { Stop-Process -Id $engine.Id -Force }
        "$tag capture exit=$($capture.ExitCode) size=$((Get-Item $trace -ErrorAction SilentlyContinue).Length)"
        Start-Sleep -Seconds 2
    }
}
