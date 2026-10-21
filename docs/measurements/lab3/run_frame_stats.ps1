# Замеры ЛР3 --frame-stats: движок 3 раза на сцену, кадр 900 (draws, PSO, барьеры, GPU ms по проходам). Один скрипт для «до» и «после».
# Запуск из корня репозитория:
#   powershell -File docs\measurements\lab3\run_frame_stats.ps1 -ExeDir <каталог с myengine.exe, RelWithDebInfo> -OutDir <куда класть> [-Runs 3] [-Frame 900]

param(
    [Parameter(Mandatory = $true)][string]$ExeDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [int]$Runs = 3,
    [int]$Frame = 900,
    [string[]]$Scenes = @("benchmark", "rocket_league")
)

$ExeDir = (Resolve-Path $ExeDir).Path
New-Item -ItemType Directory -Force $OutDir | Out-Null
$out = (Resolve-Path $OutDir).Path

foreach ($scene in $Scenes) {
    for ($run = 1; $run -le $Runs; $run++) {
        $tag = "${scene}_run$run"
        $p = Start-Process -FilePath (Join-Path $ExeDir "myengine.exe") -WorkingDirectory $ExeDir `
            -ArgumentList "--scene", "assets/scenes/$scene.json", "--play", "--frame-stats", "$Frame", "--frame-stats-out", (Join-Path $out "$tag.stats.json") `
            -PassThru -Wait -RedirectStandardOutput (Join-Path $out "$tag.stats.md")
        "$tag exit=$($p.ExitCode)"
    }
}
