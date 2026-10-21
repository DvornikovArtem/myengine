# Замеры «до» ЛР3 (A0): запускает движок по 3 раза на сцену и снимает --frame-stats 900 (draws, PSO, барьеры, GPU ms по проходам).
# Запуск из корня репозитория:  powershell -File docs\measurements\lab3\before\run_before.ps1
# Нужна сборка RelWithDebInfo (build\app\RelWithDebInfo\myengine.exe).

$root = Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")
$exeDir = Join-Path $root "build\app\RelWithDebInfo"
$out = $PSScriptRoot
$scenes = @("benchmark", "rocket_league")
$runs = 3
$frame = 900

foreach ($scene in $scenes) {
    for ($run = 1; $run -le $runs; $run++) {
        $tag = "${scene}_run$run"
        $log = Join-Path $exeDir "logs\myengine.log"
        if (Test-Path $log) { Remove-Item $log -Force }
        $p = Start-Process -FilePath (Join-Path $exeDir "myengine.exe") -WorkingDirectory $exeDir `
            -ArgumentList "--scene", "assets/scenes/$scene.json", "--play", "--frame-stats", "$frame", "--frame-stats-out", (Join-Path $out "$tag.stats.json") `
            -PassThru -Wait -RedirectStandardOutput (Join-Path $out "$tag.stats.md")
        "$tag exit=$($p.ExitCode)"
    }
}
