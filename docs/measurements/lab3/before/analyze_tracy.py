"""Разбор Tracy-трейсов «до» ЛР3: python docs/measurements/lab3/before/analyze_tracy.py [--window 5 20]
Выгружает зоны через tracy-csvexport -u, считает медиану, p95, p99 (линейная интерполяция) в окне прогрева-после
и пишет results_tracy.md и results_tracy.json; csv зон Frame и RenderFrame сохраняет в csv/ (как в ЛР1)."""

import csv
import glob
import io
import json
import os
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TRACY = os.path.join(os.environ["USERPROFILE"], "Documents", "apps", "tracy", "tracy-csvexport.exe")

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
CSV_ZONES = {"Frame", "RenderFrame"}


def percentile(values, q):
    ordered = sorted(values)
    position = (len(ordered) - 1) * q
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def main():
    window = (5.0, 20.0)
    if "--window" in sys.argv:
        index = sys.argv.index("--window")
        window = (float(sys.argv[index + 1]), float(sys.argv[index + 2]))
    start_ns, end_ns = window[0] * 1e9, window[1] * 1e9

    os.makedirs(os.path.join(HERE, "traces", "csv"), exist_ok=True)
    wanted = {name for name, _ in ZONES}
    result = {"window_s": window, "scenes": {}}

    for scene in ("benchmark", "rocket_league"):
        runs = []
        for trace in sorted(glob.glob(os.path.join(HERE, "traces", "%s_run*.tracy" % scene))):
            tag = os.path.splitext(os.path.basename(trace))[0]
            samples = {name: [] for name in wanted}
            frames_in_window = 0
            process = subprocess.Popen([TRACY, "-u", trace], stdout=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
            reader = csv.reader(process.stdout)
            next(reader)
            csv_rows = {name: [] for name in CSV_ZONES}
            for row in reader:
                name = row[0]
                if name not in wanted:
                    continue
                begin = float(row[3])
                if name in CSV_ZONES:
                    csv_rows[name].append(row)
                if start_ns <= begin < end_ns:
                    samples[name].append(float(row[4]) / 1e6)
            process.wait()
            for name, rows in csv_rows.items():
                with open(os.path.join(HERE, "traces", "csv", "%s_%s.csv" % (tag, name)), "w", newline="", encoding="utf-8") as handle:
                    writer = csv.writer(handle)
                    writer.writerow(["name", "src_file", "src_line", "ns_since_start", "exec_time_ns", "thread", "value"])
                    writer.writerows(rows)
            runs.append((tag, samples))

        scene_result = {}
        for name, label in ZONES:
            per_run = []
            for tag, samples in runs:
                values = samples[name]
                if not values:
                    per_run.append(None)
                    continue
                per_run.append({
                    "count": len(values),
                    "median": statistics.median(values),
                    "p95": percentile(values, 0.95),
                    "p99": percentile(values, 0.99),
                    "sum": sum(values),
                })
            scene_result[label] = per_run
        frame_counts = [len(samples["Frame"]) for _, samples in runs]
        for label, per_run in scene_result.items():
            for entry in per_run:
                pass
        result["scenes"][scene] = {"zones": scene_result, "frames_in_window": frame_counts}

    with open(os.path.join(HERE, "results_tracy.json"), "w", encoding="utf-8") as handle:
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
    with open(os.path.join(HERE, "results_tracy.md"), "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))
    print("\n".join(lines))


if __name__ == "__main__":
    main()
