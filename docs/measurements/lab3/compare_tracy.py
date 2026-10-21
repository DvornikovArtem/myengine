"""Сравнение «до» и «после» по результатам analyze_tracy.py: python docs/measurements/lab3/compare_tracy.py <до> <после> [<до_stats_каталог> <после_stats_каталог>]
Берёт results_tracy.json из обоих каталогов (результаты analyze_tracy.py), усредняет по трём прогонам медиану и p95 каждой зоны и печатает
таблицы «до / после / разница» в Markdown. Если даны каталоги --frame-stats, добавляет сравнение draws, PSO, барьеров, GPU мс (медиана трёх прогонов)."""

import glob
import json
import os
import re
import statistics
import sys


def load(path):
    with open(os.path.join(path, "results_tracy.json"), encoding="utf-8") as handle:
        return json.load(handle)


def mean(values):
    values = [v for v in values if v is not None]
    return sum(values) / len(values) if values else None


def cell(value, digits):
    return "-" if value is None else ("%." + str(digits) + "f") % value


def delta(before, after, digits):
    if before is None or after is None:
        return "-", "-"
    diff = after - before
    percent = diff / before * 100.0 if before else 0.0
    return ("%+." + str(digits) + "f") % diff, "%+.0f %%" % percent


def zone_tables(before, after):
    lines = []
    for scene in ("benchmark", "rocket_league"):
        lines.append("#### %s\n" % scene)
        lines.append("| Зона | Медиана до, мс | Медиана после, мс | Разница | p95 до, мс | p95 после, мс | Разница | Сумма за кадр до -> после, мс |")
        lines.append("| --- | --- | --- | --- | --- | --- | --- | --- |")
        for label, runs_before in before["scenes"][scene]["zones"].items():
            runs_after = after["scenes"][scene]["zones"].get(label)
            if runs_after is None:
                continue
            small = label.startswith("Dx12RenderAdapter::Draw")
            digits = 4 if small else 3
            med_b = mean([e["median"] if e else None for e in runs_before])
            med_a = mean([e["median"] if e else None for e in runs_after])
            p95_b = mean([e["p95"] if e else None for e in runs_before])
            p95_a = mean([e["p95"] if e else None for e in runs_after])
            frames_b = before["scenes"][scene]["frames_in_window"]
            frames_a = after["scenes"][scene]["frames_in_window"]
            sum_b = mean([e["sum"] / f if e and f else None for e, f in zip(runs_before, frames_b)])
            sum_a = mean([e["sum"] / f if e and f else None for e, f in zip(runs_after, frames_a)])
            d_med = delta(med_b, med_a, digits)
            d_p95 = delta(p95_b, p95_a, digits)
            lines.append("| `%s` | %s | %s | %s (%s) | %s | %s | %s (%s) | %s -> %s |" % (
                label, cell(med_b, digits), cell(med_a, digits), d_med[0], d_med[1],
                cell(p95_b, digits), cell(p95_a, digits), d_p95[0], d_p95[1], cell(sum_b, 2), cell(sum_a, 2)))
        lines.append("")
    return lines


def read_stats(directory, scene):
    result = []
    for path in sorted(glob.glob(os.path.join(directory, "%s_run*.stats.md" % scene))):
        text = open(path, encoding="utf-8", errors="replace").read()
        gpu = re.search(r"GPU time: ([0-9.]+) ms", text)
        counts = re.search(r"draws: (\d+), triangles: (\d+), PSO switches: (\d+), texture switches: (\d+), barriers: (\d+)", text)
        if gpu and counts:
            result.append({"gpu": float(gpu.group(1)), "draws": int(counts.group(1)), "triangles": int(counts.group(2)),
                           "pso": int(counts.group(3)), "textures": int(counts.group(4)), "barriers": int(counts.group(5))})
    return result


def stats_table(before_dir, after_dir):
    lines = ["| Сцена | Метрика | До (3 прогона) | После (3 прогона) |", "| --- | --- | --- | --- |"]
    for scene in ("benchmark", "rocket_league"):
        before = read_stats(before_dir, scene)
        after = read_stats(after_dir, scene)
        for key, label in (("draws", "draws"), ("triangles", "треугольники"), ("pso", "смены PSO"), ("textures", "смены текстур"),
                           ("barriers", "барьеры"), ("gpu", "GPU, мс (кадр 900)")):
            def join(runs):
                return " / ".join(("%.3f" % r[key]) if key == "gpu" else str(r[key]) for r in runs)
            lines.append("| %s | %s | %s | %s |" % (scene, label, join(before), join(after)))
    lines.append("")
    return lines


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    before = load(sys.argv[1])
    after = load(sys.argv[2])
    lines = zone_tables(before, after)
    if len(sys.argv) >= 5:
        lines.append("#### --frame-stats 900\n")
        lines.extend(stats_table(sys.argv[3], sys.argv[4]))
    print("\n".join(lines))


if __name__ == "__main__":
    main()
