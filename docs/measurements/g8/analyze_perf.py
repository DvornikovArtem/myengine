"""Разбор Tracy-трейсов G8: python docs/measurements/g8/analyze_perf.py <каталог результатов> [--window 15 60] [--tracy <папка с tracy-csvexport.exe>] [--top 22]

В каталоге ожидаются traces/offline_run<N>.tracy, traces/online_host_run<N>.tracy и traces/online_client_run<N>.tracy (run_perf.ps1).
Для каждого вида прогона и каждого прогона берётся окно после прогрева (секунды от начала захвата) и считается:
  1. темп кадров: интервалы между началами зон Frame (это и есть время кадра, вместе с ограничителем FPS и Present): FPS, медиана, p95, p99, p99.9, максимум;
     сколько кадров дольше 7 мс (бюджет 144 FPS), 10 мс (порог рывка) и 16.7 мс (ниже 60 FPS);
  2. цена зон: вызовов за кадр, медиана и p99 одного вызова, максимум, сумма за кадр (мс).
Результат: results_perf.md и results_perf.json в каталоге результатов; в таблицы идёт среднее по прогонам (максимум - наибольший по прогонам)."""

import collections
import csv
import glob
import io
import json
import os
import statistics
import subprocess
import sys


def percentile(values, q):
    ordered = sorted(values)
    position = (len(ordered) - 1) * q
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def export_zones(tracy_dir, trace):
    exe = os.path.join(tracy_dir, "tracy-csvexport.exe")
    out = subprocess.run([exe, "-u", "-p", trace], capture_output=True, check=True).stdout.decode("utf-8", "replace")
    return list(csv.DictReader(io.StringIO(out)))


def analyse(tracy_dir, trace, window):
    rows = export_zones(tracy_dir, trace)
    zones = collections.defaultdict(list)
    plots = collections.defaultdict(list)
    for row in rows:
        if not row.get("name") or not row.get("ns_since_start"):
            continue
        start = int(row["ns_since_start"])
        if row.get("exec_time_ns"):
            zones[row["name"]].append((start, int(row["exec_time_ns"])))
        elif row.get("value"):
            plots[row["name"]].append((start, float(row["value"])))
    frames = sorted(zones.get("Frame", []))
    if not frames:
        return None
    origin = frames[0][0]
    lo, hi = window[0] * 1e9, window[1] * 1e9
    inside = [f for f in frames if lo <= f[0] - origin <= hi]
    starts = [f[0] for f in inside]
    intervals = [(b - a) / 1e6 for a, b in zip(starts, starts[1:])]
    if len(intervals) < 100:
        return None
    result = {
        "frames": len(inside),
        "seconds": (starts[-1] - starts[0]) / 1e9,
        "fps": 1000.0 / statistics.mean(intervals),
        "interval_ms": {
            "median": statistics.median(intervals),
            "p95": percentile(intervals, 0.95),
            "p99": percentile(intervals, 0.99),
            "p999": percentile(intervals, 0.999),
            "max": max(intervals),
        },
        "over_7ms": sum(1 for v in intervals if v > 7.0),
        "over_10ms": sum(1 for v in intervals if v > 10.0),
        "over_16_7ms": sum(1 for v in intervals if v > 16.7),
        "zones": {},
        "plots": {},
    }
    for name, samples in plots.items():
        chosen = [v for (s, v) in samples if lo <= s - origin <= hi]
        if chosen:
            result["plots"][name] = {"median": statistics.median(chosen), "p99": percentile(chosen, 0.99), "max": max(chosen), "mean": statistics.mean(chosen)}
    for name, calls in zones.items():
        chosen = [c[1] / 1e6 for c in calls if lo <= c[0] - origin <= hi]
        if not chosen:
            continue
        result["zones"][name] = {
            "calls_per_frame": len(chosen) / len(inside),
            "median": statistics.median(chosen),
            "p99": percentile(chosen, 0.99),
            "max": max(chosen),
            "per_frame": sum(chosen) / len(inside),
        }
    return result


def short(name):
    return name.replace("myengine::", "").replace("ecs::systems::", "").replace("render::dx12::", "")


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    out_dir = args[0]
    window = (15.0, 60.0)
    tracy_dir = os.path.join(os.environ.get("USERPROFILE", ""), "Documents", "apps", "tracy")
    top = 22
    i = 1
    while i < len(args):
        if args[i] == "--window":
            window = (float(args[i + 1]), float(args[i + 2]))
            i += 3
        elif args[i] == "--tracy":
            tracy_dir = args[i + 1]
            i += 2
        elif args[i] == "--top":
            top = int(args[i + 1])
            i += 2
        else:
            i += 1

    groups = collections.OrderedDict()
    for kind in ("offline", "online_host", "online_client"):
        traces = sorted(glob.glob(os.path.join(out_dir, "traces", kind + "_run*.tracy")))
        if traces:
            groups[kind] = traces

    report = {"window": window, "groups": {}}
    lines = ["# G8: замеры кадра матча 4v4", "", "Окно анализа %.0f-%.0f с от начала захвата." % window, ""]
    for kind, traces in groups.items():
        runs = [r for r in (analyse(tracy_dir, t, window) for t in traces) if r]
        if not runs:
            continue
        report["groups"][kind] = runs

        def mean(path):
            values = []
            for r in runs:
                node = r
                for key in path:
                    node = node[key]
                values.append(node)
            return statistics.mean(values)

        lines += ["## %s (%d прогонов)" % (kind, len(runs)), ""]
        lines += ["| Показатель | " + " | ".join("прогон %d" % (n + 1) for n in range(len(runs))) + " | среднее |", "| --- |" + " --- |" * (len(runs) + 1)]

        def row(label, getter, fmt):
            vals = [getter(r) for r in runs]
            lines.append("| %s | " % label + " | ".join(fmt % v for v in vals) + " | " + fmt % statistics.mean(vals) + " |")

        row("кадров в окне", lambda r: r["frames"], "%d")
        row("FPS (среднее)", lambda r: r["fps"], "%.0f")
        row("кадр, медиана, мс", lambda r: r["interval_ms"]["median"], "%.2f")
        row("кадр, p95, мс", lambda r: r["interval_ms"]["p95"], "%.2f")
        row("кадр, p99, мс", lambda r: r["interval_ms"]["p99"], "%.2f")
        row("кадр, p99.9, мс", lambda r: r["interval_ms"]["p999"], "%.2f")
        row("кадр, максимум, мс", lambda r: r["interval_ms"]["max"], "%.1f")
        row("кадров дольше 7 мс", lambda r: r["over_7ms"], "%d")
        row("кадров дольше 10 мс", lambda r: r["over_10ms"], "%d")
        row("кадров дольше 16.7 мс", lambda r: r["over_16_7ms"], "%d")
        lines.append("")
        names = set()
        for r in runs:
            names.update(r["zones"].keys())
        ranked = sorted(names, key=lambda n: -statistics.mean(r["zones"].get(n, {"per_frame": 0})["per_frame"] for r in runs))[:top]
        lines += ["| Зона | вызовов/кадр | медиана вызова, мс | p99, мс | максимум, мс | за кадр, мс |", "| --- | --- | --- | --- | --- | --- |"]
        for name in ranked:
            have = [r["zones"][name] for r in runs if name in r["zones"]]
            lines.append("| %s | %.2f | %.3f | %.3f | %.2f | %.3f |" % (
                short(name), statistics.mean(z["calls_per_frame"] for z in have), statistics.mean(z["median"] for z in have),
                statistics.mean(z["p99"] for z in have), max(z["max"] for z in have), statistics.mean(z["per_frame"] for z in have)))
        lines.append("")
        plot_names = sorted({n for r in runs for n in r["plots"] if n.startswith("GPU")})
        if plot_names:
            lines += ["| График (GPU, из Tracy) | медиана, мс | p99, мс | максимум, мс | среднее, мс |", "| --- | --- | --- | --- | --- |"]
            for name in plot_names:
                have = [r["plots"][name] for r in runs if name in r["plots"]]
                lines.append("| %s | %.3f | %.3f | %.2f | %.3f |" % (name, statistics.mean(p["median"] for p in have), statistics.mean(p["p99"] for p in have),
                                                                     max(p["max"] for p in have), statistics.mean(p["mean"] for p in have)))
            lines.append("")

    with open(os.path.join(out_dir, "results_perf.md"), "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    with open(os.path.join(out_dir, "results_perf.json"), "w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=1)
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
