"""Сравнение «до» и «после» по results_perf.json (analyze_perf.py): python docs/measurements/g8/compare_perf.py <до> <после> [<вид прогона>]
Печатает таблицу Markdown: на каждый вид прогона (offline, online_host, online_client) среднее по прогонам FPS, медиана кадра, p95, p99, p99.9, наибольший кадр
из всех прогонов и число кадров дольше 10 мс на прогон (в среднем и по прогонам)."""

import json
import statistics
import sys


def load(path):
    with open(path + "/results_perf.json", encoding="utf-8") as handle:
        return json.load(handle)["groups"]


def row(label, runs):
    def mean(key):
        return statistics.mean(r["interval_ms"][key] for r in runs)

    over = [r["over_10ms"] for r in runs]
    return "| %s | %d | %.0f | %.2f | %.2f | %.2f | %.2f | %.1f | %.0f (%s) |" % (
        label, len(runs), statistics.mean(r["fps"] for r in runs), mean("median"), mean("p95"), mean("p99"), mean("p999"),
        max(r["interval_ms"]["max"] for r in runs), statistics.mean(over), ", ".join(str(v) for v in over))


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    before = load(sys.argv[1])
    after = load(sys.argv[2])
    kinds = sys.argv[3:] or ["offline", "online_host", "online_client"]
    print("| Прогон | N | FPS | кадр, медиана, мс | p95 | p99 | p99.9 | наибольший, мс | кадров > 10 мс за 40 с |")
    print("| --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for kind in kinds:
        if kind in before:
            print(row("%s, до" % kind, before[kind]))
        if kind in after:
            print(row("%s, после" % kind, after[kind]))


if __name__ == "__main__":
    main()
