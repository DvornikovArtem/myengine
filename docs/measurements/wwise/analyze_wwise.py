"""Разбор замеров звука XAudio2 против Wwise: python analyze_wwise.py <каталог со сторонами> [--window 25 80] [--tracy <папка с tracy-csvexport.exe>]

В каталоге ожидаются подкаталоги off (XAudio2) и on (Wwise): в каждом traces/<сторона>_tracy_run<N>.tracy (трейсы Tracy) и <сторона>_plain_run<N>.run.json
(загрузка процессора и память без захвата). Скрипт выгружает зоны через tracy-csvexport -u, берёт окно от прогрева до конца сценария и пишет в каталог
results_wwise.md и results_wwise.json: для каждой зоны медиана, p95, p99 времени одного вызова по трём прогонам, сумма зоны за кадр, и таблицу CPU и памяти.
Одна и та же программа служит для обеих сторон: зон другой стороны в трейсе просто нет (Wwise::RenderAudio только у Wwise)."""

import csv
import glob
import json
import os
import statistics
import subprocess
import sys

# точное имя зоны -> подпись
ZONES = [
    ("Frame", "Frame (кадр целиком)"),
    ("Audio::Update", "AudioSystem::Update (за кадр)"),
    ("Audio::BackendUpdate", "XAudio2: backend->Update (внутри Update)"),
    ("Wwise::RenderAudio", "Wwise: RenderAudio (внутри Update)"),
    ("Audio::PlayEvent", "AudioSystem::PlayEvent (за вызов)"),
    ("Physics::Step", "Physics::Step (для сверки машины)"),
    ("Present", "Present (для сверки машины)"),
]


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


def read_trace(csvexport, trace, wanted):
    """Все события нужных зон: {имя: [(начало_нс, длительность_мс), ...]}."""
    events = {name: [] for name in wanted}
    process = subprocess.Popen([csvexport, "-u", trace], stdout=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
    reader = csv.reader(process.stdout)
    next(reader)
    for row in reader:
        if row[0] in wanted:
            events[row[0]].append((float(row[3]), float(row[4]) / 1e6))
    process.wait()
    return events


def stats_of(values):
    if not values:
        return None
    return {"count": len(values), "median": statistics.median(values), "p95": percentile(values, 0.95), "p99": percentile(values, 0.99), "sum": sum(values)}


def mean(values):
    values = [v for v in values if v is not None]
    return sum(values) / len(values) if values else None


def cell(value, digits=3):
    return "-" if value is None else ("%." + str(digits) + "f") % value


def main():
    args = sys.argv[1:]
    if not args or args[0].startswith("--"):
        raise SystemExit(__doc__)
    root = os.path.abspath(args[0])
    window = (25.0, 80.0)
    tracy_dir = None
    if "--window" in args:
        index = args.index("--window")
        window = (float(args[index + 1]), float(args[index + 2]))
    if "--tracy" in args:
        tracy_dir = args[args.index("--tracy") + 1]
    csvexport = find_csvexport(tracy_dir)
    start_ns, end_ns = window[0] * 1e9, window[1] * 1e9
    wanted = {name for name, _ in ZONES}
    result = {"window_s": window, "sides": {}}

    for side in ("off", "on"):
        runs = []
        for trace in sorted(glob.glob(os.path.join(root, side, "traces", "%s_tracy_run*.tracy" % side))):
            events = read_trace(csvexport, trace, wanted)
            per_zone = {}
            for name in wanted:
                values = [d for (b, d) in events[name] if start_ns <= b < end_ns]
                per_zone[name] = stats_of(values)
            frames = len([1 for (b, d) in events["Frame"] if start_ns <= b < end_ns])
            audio_sum = sum(d for (b, d) in events["Audio::Update"] if start_ns <= b < end_ns)
            window_s = (end_ns - start_ns) / 1e9
            runs.append({"trace": os.path.basename(trace), "zones": per_zone, "frames": frames, "audioUpdateMsPerFrame": audio_sum / frames if frames else None,
                         "audioUpdateShareOfFramePercent": 100.0 * audio_sum / sum(d for (b, d) in events["Frame"] if start_ns <= b < end_ns) if frames else None,
                         "framesPerSecond": frames / window_s})
        plain = []
        for path in sorted(glob.glob(os.path.join(root, side, "%s_plain_run*.run.json" % side))):
            with open(path, encoding="utf-8-sig") as handle:
                plain.append(json.load(handle))
        result["sides"][side] = {"tracy_runs": runs, "plain_runs": plain}

    with open(os.path.join(root, "results_wwise.json"), "w", encoding="utf-8") as handle:
        json.dump(result, handle, ensure_ascii=False, indent=1)

    lines = ["# Результаты замеров звука: XAudio2 (off) и Wwise (on)", "", "Окно анализа: %.0f-%.0f с от начала захвата. Медиана, p95 и p99 времени одного вызова зоны, усреднённые по прогонам; "
            "по прогонам - в results_wwise.json." % window, ""]
    lines.append("| Зона | Медиана off, мс | Медиана on, мс | p95 off, мс | p95 on, мс | p99 off, мс | p99 on, мс | Вызовов за прогон off / on |")
    lines.append("| --- | --- | --- | --- | --- | --- | --- | --- |")
    for name, label in ZONES:
        row = []
        counts = []
        for key in ("median", "p95", "p99"):
            for side in ("off", "on"):
                row.append(mean([run["zones"][name][key] if run["zones"][name] else None for run in result["sides"][side]["tracy_runs"]]))
        for side in ("off", "on"):
            counts.append(mean([run["zones"][name]["count"] if run["zones"][name] else None for run in result["sides"][side]["tracy_runs"]]))
        digits = 4 if name == "Audio::PlayEvent" else 3
        lines.append("| %s | %s | %s | %s | %s | %s | %s | %s / %s |" % (label, cell(row[0], digits), cell(row[1], digits), cell(row[2], digits), cell(row[3], digits),
                                                                    cell(row[4], digits), cell(row[5], digits), cell(counts[0], 0), cell(counts[1], 0)))
    lines += ["", "| Что | off (XAudio2) | on (Wwise) |", "| --- | --- | --- |"]
    for label, key, digits in (("Кадров в окне (среднее по прогонам)", "frames", 0), ("Кадров в секунду", "framesPerSecond", 1), ("AudioSystem::Update, мс на кадр (сумма зоны / кадры)", "audioUpdateMsPerFrame", 4),
                               ("Доля AudioSystem::Update во времени кадра, %", "audioUpdateShareOfFramePercent", 3)):
        lines.append("| %s | %s | %s |" % (label, cell(mean([r[key] for r in result["sides"]["off"]["tracy_runs"]]), digits), cell(mean([r[key] for r in result["sides"]["on"]["tracy_runs"]]), digits)))
    lines += ["", "### CPU и память процесса за прогон (без захвата Tracy)", "", "| Что | off (XAudio2) | on (Wwise) |", "| --- | --- | --- |"]
    for label, key, digits in (("CPU процесса за прогон, с (весь процесс, все потоки)", "cpuSecondsTotal", 1), ("CPU в окне матча, % одного ядра", "cpuPercentOfOneCoreInWindow", 1),
                               ("Рабочий набор: максимум, МБ", "workingSetMaxMB", 1), ("Рабочий набор: среднее в окне, МБ", "workingSetWindowMeanMB", 1), ("Private bytes: максимум, МБ", "privateMaxMB", 1)):
        off = [r.get(key) for r in result["sides"]["off"]["plain_runs"]]
        on = [r.get(key) for r in result["sides"]["on"]["plain_runs"]]
        lines.append("| %s | %s (%s) | %s (%s) |" % (label, cell(statistics.median([v for v in off if v is not None]) if [v for v in off if v is not None] else None, digits), ", ".join(cell(v, digits) for v in off),
                                                  cell(statistics.median([v for v in on if v is not None]) if [v for v in on if v is not None] else None, digits), ", ".join(cell(v, digits) for v in on)))
    with open(os.path.join(root, "results_wwise.md"), "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
