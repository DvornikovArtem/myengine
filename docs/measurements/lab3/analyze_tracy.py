"""Разбор Tracy-трейсов ЛР3: python docs/measurements/lab3/analyze_tracy.py <каталог результатов> [--window 5 20] [--tracy <папка с tracy-csvexport.exe>]

В каталоге ожидается traces/<сцена>_run<N>.tracy (обычные прогоны) и traces/benchmark_reload<N>.tracy (прогоны с перезагрузкой шейдера).
Выгружает зоны через tracy-csvexport -u и пишет в каталог results_tracy.md и results_tracy.json:
  1. время одного вызова каждой зоны кадра в окне после прогрева (медиана, p95, p99 линейной интерполяцией; сумма за кадр);
  2. цена шейдерных зон (Shader::Compile, Shader::Reflect, Shader::BuildPipeline, ResourceManager::MaterialParams) на старте и при перезагрузке
     шейдера: вызовов, сумма и максимум, мс; а также самый длинный кадр сразу после перезагрузки.
CSV зон Frame и RenderFrame сохраняются в traces/csv/ (как в ЛР1). Одна и та же программа служит для «до» и для «после»: в «до» шейдерных зон нет."""

import csv
import glob
import json
import os
import statistics
import subprocess
import sys

# точное имя зоны -> подпись в таблице
ZONES = [
    ("Frame", "Frame"),
    ("RenderFrame", "RenderFrame"),
    ("myengine::ecs::systems::RenderSystem::Render", "RenderSystem::Render"),
    ("CollectRenderables", "CollectRenderables"),
    ("FrustumCulling", "FrustumCulling"),
    ("BuildDrawItems", "BuildDrawItems"),
    ("SubmitDraws", "SubmitDraws"),
    ("myengine::render::dx12::Dx12RenderAdapter::Draw", "Dx12RenderAdapter::Draw (за вызов)"),
    ("myengine::render::dx12::Dx12RenderAdapter::DrawUiGeometry", "Dx12RenderAdapter::DrawUiGeometry"),
    ("myengine::ecs::systems::DebugRenderSystem::Render", "DebugRenderSystem::Render"),
    ("myengine::ui::UiManager::RenderWindow", "UiManager::RenderWindow"),
    ("myengine::render::dx12::Dx12RenderAdapter::BeginFrame", "Dx12RenderAdapter::BeginFrame"),
    ("myengine::render::dx12::Dx12RenderAdapter::EndFrame", "Dx12RenderAdapter::EndFrame"),
    ("Present", "Present"),
    ("Physics::Step", "Physics::Step"),
]
SHADER_ZONES = [
    "Shader::Compile",
    "Shader::Reflect",
    "Shader::BuildPipeline",
    "ResourceManager::MaterialParams",
]
CSV_ZONES = {"Frame", "RenderFrame"}
RELOAD_FROM_S = 11.0   # перезагрузка шейдера делается на 12-й секунде после старта движка (около 13-й секунды трейса); раньше - старт
RELOAD_FRAME_WINDOW_S = (11.0, 16.0)


def percentile(values, q):
    ordered = sorted(values)
    position = (len(ordered) - 1) * q
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def find_csvexport(explicit):
    candidates = []
    if explicit:
        candidates.append(os.path.join(explicit, "tracy-csvexport.exe"))
    profile = os.environ.get("USERPROFILE", "")
    candidates.append(os.path.join(profile, "source", "repos", "tracy", "tracy-csvexport.exe"))
    candidates.append(os.path.join(profile, "Documents", "apps", "tracy", "tracy-csvexport.exe"))
    for candidate in candidates:
        if os.path.exists(candidate):
            return candidate
    raise SystemExit("tracy-csvexport.exe не найден: укажите --tracy <папка>")


def read_trace(csvexport, trace, wanted, csv_rows):
    """Все события нужных зон: {имя: [(начало_нс, длительность_мс), ...]}."""
    events = {name: [] for name in wanted}
    process = subprocess.Popen([csvexport, "-u", trace], stdout=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
    reader = csv.reader(process.stdout)
    next(reader)
    for row in reader:
        name = row[0]
        if name not in wanted:
            continue
        events[name].append((float(row[3]), float(row[4]) / 1e6))
        if name in CSV_ZONES and csv_rows is not None:
            csv_rows[name].append(row)
    process.wait()
    return events


def stats_of(values):
    return {
        "count": len(values),
        "median": statistics.median(values),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
        "sum": sum(values),
    }


def main():
    args = sys.argv[1:]
    if not args or args[0].startswith("--"):
        raise SystemExit(__doc__)
    out_dir = os.path.abspath(args[0])
    window = (5.0, 20.0)
    tracy_dir = None
    if "--window" in args:
        index = args.index("--window")
        window = (float(args[index + 1]), float(args[index + 2]))
    if "--tracy" in args:
        tracy_dir = args[args.index("--tracy") + 1]
    csvexport = find_csvexport(tracy_dir)
    start_ns, end_ns = window[0] * 1e9, window[1] * 1e9
    traces = os.path.join(out_dir, "traces")
    os.makedirs(os.path.join(traces, "csv"), exist_ok=True)
    wanted = {name for name, _ in ZONES} | set(SHADER_ZONES)
    result = {"window_s": window, "scenes": {}, "shader_zones": {}}

    for scene in ("benchmark", "rocket_league"):
        runs = []
        for trace in sorted(glob.glob(os.path.join(traces, "%s_run*.tracy" % scene))):
            tag = os.path.splitext(os.path.basename(trace))[0]
            csv_rows = {name: [] for name in CSV_ZONES}
            events = read_trace(csvexport, trace, wanted, csv_rows)
            for name, rows in csv_rows.items():
                with open(os.path.join(traces, "csv", "%s_%s.csv" % (tag, name)), "w", newline="", encoding="utf-8") as handle:
                    writer = csv.writer(handle)
                    writer.writerow(["name", "src_file", "src_line", "ns_since_start", "exec_time_ns", "thread", "value"])
                    writer.writerows(rows)
            samples = {name: [d for (b, d) in events[name] if start_ns <= b < end_ns] for name in wanted}
            runs.append((tag, samples, events))

        scene_result = {}
        for name, label in ZONES:
            per_run = []
            for tag, samples, events in runs:
                values = samples[name]
                per_run.append(stats_of(values) if values else None)
            scene_result[label] = per_run
        frame_counts = [len(samples["Frame"]) for _, samples, _ in runs]
        result["scenes"][scene] = {"zones": scene_result, "frames_in_window": frame_counts}

        # шейдерные зоны за весь обычный прогон (старт сцены: шейдеры, материалы)
        startup = {}
        for name in SHADER_ZONES:
            per_run = []
            for tag, samples, events in runs:
                values = [d for (b, d) in events[name]]
                per_run.append({"count": len(values), "sum": sum(values), "max": max(values)} if values else None)
            startup[name] = per_run
        result["shader_zones"].setdefault("startup", {})[scene] = startup

    # прогоны с перезагрузкой шейдера
    reload_runs = []
    for trace in sorted(glob.glob(os.path.join(traces, "benchmark_reload*.tracy"))):
        tag = os.path.splitext(os.path.basename(trace))[0]
        reload_runs.append((tag, read_trace(csvexport, trace, wanted, None)))
    if reload_runs:
        reload_result = {}
        for name in SHADER_ZONES:
            per_run = []
            for tag, events in reload_runs:
                values = [d for (b, d) in events[name] if b >= RELOAD_FROM_S * 1e9]
                per_run.append({"count": len(values), "sum": sum(values), "max": max(values)} if values else None)
            reload_result[name] = per_run
        frame_spike = []
        frame_median = []
        for tag, events in reload_runs:
            near = [d for (b, d) in events["Frame"] if RELOAD_FRAME_WINDOW_S[0] * 1e9 <= b < RELOAD_FRAME_WINDOW_S[1] * 1e9]
            steady = [d for (b, d) in events["Frame"] if start_ns <= b < RELOAD_FROM_S * 1e9]
            frame_spike.append(max(near) if near else None)
            frame_median.append(statistics.median(steady) if steady else None)
        result["shader_zones"]["reload"] = {"zones": reload_result, "longest_frame_after_ms": frame_spike, "median_frame_before_ms": frame_median}

    with open(os.path.join(out_dir, "results_tracy.json"), "w", encoding="utf-8") as handle:
        json.dump(result, handle, indent=2, ensure_ascii=False)

    lines = ["Окно анализа %g-%g с после старта захвата; время одного вызова зоны, мс (run 1 / 2 / 3).\n" % window]
    for scene, data in result["scenes"].items():
        lines.append("### %s (кадров в окне: %s)\n" % (scene, " / ".join(str(c) for c in data["frames_in_window"])))
        lines.append("| Зона | Вызовов в окне | Медиана | p95 | p99 | Сумма за кадр, мс |")
        lines.append("| --- | --- | --- | --- | --- | --- |")
        for label, per_run in data["zones"].items():
            if all(entry is None for entry in per_run):
                continue
            small = max((e["p99"] for e in per_run if e), default=0) < 0.05  # микросекундные зоны: 4 знака

            def column(key):
                return " / ".join(("%.4f" if small else "%.3f") % e[key] if e else "-" for e in per_run)
            counts = " / ".join(str(e["count"]) if e else "-" for e in per_run)
            per_frame = " / ".join("%.2f" % (e["sum"] / frames) if e and frames else "-"
                                   for e, frames in zip(per_run, data["frames_in_window"]))
            lines.append("| `%s` | %s | %s | %s | %s | %s |" % (label, counts, column("median"), column("p95"), column("p99"), per_frame))
        lines.append("")

    lines.append("### Шейдерные зоны на старте (весь прогон, run 1 / 2 / 3): вызовов, сумма мс, максимум мс\n")
    lines.append("| Сцена | Зона | Вызовов | Сумма, мс | Максимум, мс |")
    lines.append("| --- | --- | --- | --- | --- |")
    present = False
    for scene, zones in result["shader_zones"].get("startup", {}).items():
        for name, per_run in zones.items():
            if all(entry is None for entry in per_run):
                continue
            present = True
            lines.append("| %s | `%s` | %s | %s | %s |" % (
                scene, name,
                " / ".join(str(e["count"]) if e else "-" for e in per_run),
                " / ".join("%.3f" % e["sum"] if e else "-" for e in per_run),
                " / ".join("%.3f" % e["max"] if e else "-" for e in per_run)))
    if not present:
        lines.append("| - | зон нет в этой сборке | - | - | - |")
    lines.append("")

    reload_data = result["shader_zones"].get("reload")
    if reload_data:
        lines.append("### Перезагрузка шейдера (benchmark, `textured_lit.hlsl` изменён на 12-й секунде; run 1 / 2 / 3)\n")
        lines.append("| Зона | Вызовов | Сумма, мс | Максимум, мс |")
        lines.append("| --- | --- | --- | --- |")
        shown = False
        for name, per_run in reload_data["zones"].items():
            if all(entry is None for entry in per_run):
                continue
            shown = True
            lines.append("| `%s` | %s | %s | %s |" % (
                name,
                " / ".join(str(e["count"]) if e else "-" for e in per_run),
                " / ".join("%.3f" % e["sum"] if e else "-" for e in per_run),
                " / ".join("%.3f" % e["max"] if e else "-" for e in per_run)))
        if not shown:
            lines.append("| зон нет в этой сборке | - | - | - |")
        lines.append("")
        lines.append("Самый длинный кадр в окне %g-%g с, мс: %s; медиана кадра до перезагрузки, мс: %s.\n" % (
            RELOAD_FRAME_WINDOW_S[0], RELOAD_FRAME_WINDOW_S[1],
            " / ".join("%.2f" % v if v is not None else "-" for v in reload_data["longest_frame_after_ms"]),
            " / ".join("%.2f" % v if v is not None else "-" for v in reload_data["median_frame_before_ms"])))

    with open(os.path.join(out_dir, "results_tracy.md"), "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))
    print("\n".join(lines))


if __name__ == "__main__":
    main()
