# CPU по потокам процесса в том же матче (куда уходит лишнее время Wwise): один прогон стороны, перед концом сценария снимаются потоки процесса и их TotalProcessorTime.
# Запуск: powershell -File thread_cpu.ps1 -ExeDir <каталог с myengine.exe> -OutDir <куда класть> -Side off|on [-Seconds 90]
# Потоки не названы (Windows 10 не отдаёт имена через .NET), поэтому считается, сколько потоков съели больше 0.2 с и сколько всего CPU у потоков, которых в другой стороне нет:
# сравнивать нужно списки двух сторон рядом (thread_cpu_off.txt и thread_cpu_on.txt).

param(
    [Parameter(Mandatory = $true)][string]$ExeDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][ValidateSet("off", "on")][string]$Side,
    [int]$Seconds = 90,
    [double]$Master = 0.03
)

$ErrorActionPreference = "Stop"
$ExeDir = (Resolve-Path $ExeDir).Path
New-Item -ItemType Directory -Force $OutDir | Out-Null
$out = (Resolve-Path $OutDir).Path
$user = Join-Path $out "userdata\threads_$Side"
New-Item -ItemType Directory -Force (Join-Path $user "RocketArena") | Out-Null

$settings = @{ version = 1; audio = @{ master = $Master; effects = 1.0; music = 0.6 }; video = @{ windowMode = 0; width = 1280; height = 720; vsync = $false; fpsLimit = 0; shadows = 2; msaa = 4; bloom = $true; uiScalePercent = 100; showFps = $false }; play = @{ mode = 2; botLevel = 1 }; car = "fusion" }
$settingsFile = Join-Path $out "game_settings.measure.json"
$settings | ConvertTo-Json -Depth 5 | Set-Content -Path $settingsFile -Encoding utf8
$scriptFile = Join-Path $out "match.script"
@("# ${Side}: menu, match 2v2, quit", "4.0 key Enter", "6.0 key Down", "6.4 key Down", "6.8 key Down", "8.0 key Enter", "$Seconds quit") | Set-Content -Path $scriptFile -Encoding ascii

$env:MYENGINE_UI_SCRIPT = $scriptFile
$env:MYENGINE_GAME_SETTINGS = $settingsFile
$env:APPDATA = $user
$env:LOCALAPPDATA = $user
$engine = Start-Process -FilePath (Join-Path $ExeDir "myengine.exe") -WorkingDirectory $ExeDir -ArgumentList "--game" -PassThru
$started = Get-Date
$last = $null
while (-not $engine.HasExited) {
    $elapsed = ((Get-Date) - $started).TotalSeconds
    if ($elapsed -gt ($Seconds - 3) -and $elapsed -lt $Seconds + 5) {
        try {
            $engine.Refresh()
            $last = @($engine.Threads | ForEach-Object { [pscustomobject]@{ id = $_.Id; cpu = $_.TotalProcessorTime.TotalSeconds; state = "$($_.ThreadState)" } })
        } catch { }
    }
    Start-Sleep -Milliseconds 400
}
$lines = @("# ${Side}: threads of the process, CPU seconds over a run of $([math]::Round(((Get-Date) - $started).TotalSeconds)) s (taken 3 s before the end of the script)", "threads: $($last.Count)")
$sorted = $last | Sort-Object cpu -Descending
$lines += "sum of the CPU of the threads: $([math]::Round(($sorted | Measure-Object cpu -Sum).Sum, 2)) s"
$lines += "threads with CPU over 0.2 s: $(@($sorted | Where-Object { $_.cpu -gt 0.2 }).Count)"
$lines += ""
foreach ($t in ($sorted | Select-Object -First 14)) { $lines += ("thread {0,6}  cpu {1,7:N2} s  {2}" -f $t.id, $t.cpu, $t.state) }
$lines | Set-Content -Path (Join-Path $out "thread_cpu_$Side.txt") -Encoding utf8
$lines
