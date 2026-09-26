#!/usr/bin/env python3
"""Summarize Sentinel benchmark result directories.

Percentiles use linear interpolation on the sorted sample. A timing row is late
when delay_ms is greater than late_threshold times that row's target interval.
Missing timing files stay blank instead of being treated as zero.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from pathlib import Path


def percentile(values: list[float], percent: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (percent / 100.0) * (len(ordered) - 1)
    low = math.floor(rank)
    high = math.ceil(rank)
    if low == high:
        return ordered[low]
    weight = rank - low
    return ordered[low] * (1.0 - weight) + ordered[high] * weight


def mean(values: list[float]) -> float:
    return sum(values) / len(values)


def load_json(path: Path) -> dict:
    text = path.read_text(encoding="utf-8-sig")
    payload = json.loads(text)
    if not isinstance(payload, dict):
        raise ValueError(f"{path} is not a JSON object")
    return payload


def read_resource_samples(path: Path) -> tuple[list[float], list[float]]:
    cpu: list[float] = []
    private_mb: list[float] = []
    with path.open(newline="", encoding="utf-8-sig") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None or "cpu_percent" not in reader.fieldnames:
            raise ValueError(f"{path} is missing cpu_percent")
        if "private_bytes" not in reader.fieldnames:
            raise ValueError(f"{path} is missing private_bytes")
        for row in reader:
            private_text = (row.get("private_bytes") or "").strip()
            if private_text:
                private_mb.append(float(private_text) / (1024.0 * 1024.0))
            cpu_text = (row.get("cpu_percent") or "").strip()
            if not cpu_text:
                continue
            cpu_value = float(cpu_text)
            if math.isfinite(cpu_value) and cpu_value >= 0.0:
                cpu.append(cpu_value)
    return cpu, private_mb


def read_delays(path: Path) -> list[tuple[float, float]]:
    rows: list[tuple[float, float]] = []
    with path.open(newline="", encoding="utf-8-sig") as handle:
        reader = csv.DictReader(handle)
        required = {"sample_index", "target_interval_ms", "delay_ms"}
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise ValueError(f"{path} is missing timing columns")
        for row in reader:
            rows.append((float(row["target_interval_ms"]), float(row["delay_ms"])))
    return rows


def expected_sample_count(duration_seconds: int, interval_ms: int) -> int:
    duration_ms = duration_seconds * 1000
    if duration_ms % interval_ms == 0:
        return duration_ms // interval_ms
    return duration_ms // interval_ms + 1


def find_runs(path: Path) -> list[Path]:
    if (path / "metadata.json").is_file():
        return [path]
    if not path.is_dir():
        raise FileNotFoundError(path)
    return sorted(child for child in path.iterdir() if (child / "metadata.json").is_file())


def blank() -> dict[str, float | int | None]:
    return {
        "avg_cpu": None,
        "p95_cpu": None,
        "peak_cpu": None,
        "avg_ram": None,
        "peak_ram": None,
        "expected": None,
        "actual": None,
        "completion": None,
        "late": None,
        "late_percent": None,
        "missed": None,
        "avg_delay": None,
        "p95_delay": None,
        "p99_delay": None,
        "max_delay": None,
    }


def summarize_group(runs: list[dict], late_threshold: float) -> dict[str, float | int | None]:
    summary = blank()
    cpu: list[float] = []
    memory: list[float] = []
    delays: list[float] = []
    late = 0
    actual = 0
    expected_total = 0
    expected_known = True
    timing_known = True

    for run in runs:
        cpu.extend(run["cpu"])
        memory.extend(run["memory"])
        if run["delays"] is None:
            timing_known = False
            expected_known = False
            continue
        actual += len(run["delays"])
        for interval_ms, delay_ms in run["delays"]:
            delays.append(delay_ms)
            if delay_ms > late_threshold * interval_ms:
                late += 1
        if run["expected"] is None:
            expected_known = False
        else:
            expected_total += run["expected"]

    if cpu:
        summary["avg_cpu"] = mean(cpu)
        summary["p95_cpu"] = percentile(cpu, 95)
        summary["peak_cpu"] = max(cpu)
    if memory:
        summary["avg_ram"] = mean(memory)
        summary["peak_ram"] = max(memory)
    if timing_known:
        summary["actual"] = actual
        summary["late"] = late
        summary["late_percent"] = (100.0 * late / actual) if actual else 0.0
        if delays:
            summary["avg_delay"] = mean(delays)
            summary["p95_delay"] = percentile(delays, 95)
            summary["p99_delay"] = percentile(delays, 99)
            summary["max_delay"] = max(delays)
        if expected_known:
            summary["expected"] = expected_total
            summary["missed"] = max(0, expected_total - actual)
            summary["completion"] = (100.0 * actual / expected_total) if expected_total else None
    return summary


def interval_sort_key(label: str) -> tuple[int, int]:
    if label == "built-in":
        return (1, 0)
    return (0, -int(label))


def format_rate(interval_label: str) -> str:
    if interval_label == "built-in":
        return "n/a"
    rate = 1000.0 / int(interval_label)
    if rate.is_integer():
        return f"{int(rate)} Hz"
    return f"{rate:.2f} Hz"


def format_optional(value: float | int | None, pattern: str) -> str:
    if value is None:
        return "n/a"
    return format(value, pattern)


def print_table(rows: list[tuple[str, dict]]) -> None:
    headers = (
        "Interval",
        "Rate",
        "Avg CPU",
        "Peak CPU",
        "Avg RAM",
        "Peak RAM",
        "Late %",
        "p95 delay",
        "p99 delay",
    )
    rendered = [headers]
    for label, summary in rows:
        interval = "built-in" if label == "built-in" else f"{label} ms"
        rendered.append((
            interval,
            format_rate(label),
            format_optional(summary["avg_cpu"], ".2f") + ("%" if summary["avg_cpu"] is not None else ""),
            format_optional(summary["peak_cpu"], ".2f") + ("%" if summary["peak_cpu"] is not None else ""),
            format_optional(summary["avg_ram"], ".1f") + (" MB" if summary["avg_ram"] is not None else ""),
            format_optional(summary["peak_ram"], ".1f") + (" MB" if summary["peak_ram"] is not None else ""),
            format_optional(summary["late_percent"], ".1f") + ("%" if summary["late_percent"] is not None else ""),
            format_optional(summary["p95_delay"], ".2f") + (" ms" if summary["p95_delay"] is not None else ""),
            format_optional(summary["p99_delay"], ".2f") + (" ms" if summary["p99_delay"] is not None else ""),
        ))
    widths = [max(len(row[index]) for row in rendered) for index in range(len(headers))]
    for row in rendered:
        print(" | ".join(cell.ljust(widths[index]) for index, cell in enumerate(row)))


def write_summary(path: Path, rows: list[tuple[str, dict]]) -> None:
    fieldnames = [
        "interval_ms",
        "rate_hz",
        "avg_cpu_percent",
        "p95_cpu_percent",
        "peak_cpu_percent",
        "avg_private_mb",
        "peak_private_mb",
        "expected_samples",
        "actual_samples",
        "completion_percent",
        "late_samples",
        "late_percent",
        "missed_samples",
        "avg_delay_ms",
        "p95_delay_ms",
        "p99_delay_ms",
        "max_delay_ms",
        "runs",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for label, summary in rows:
            rate = "" if label == "built-in" else f"{1000.0 / int(label):.6g}"
            writer.writerow({
                "interval_ms": "" if label == "built-in" else label,
                "rate_hz": rate,
                "avg_cpu_percent": number_or_blank(summary["avg_cpu"]),
                "p95_cpu_percent": number_or_blank(summary["p95_cpu"]),
                "peak_cpu_percent": number_or_blank(summary["peak_cpu"]),
                "avg_private_mb": number_or_blank(summary["avg_ram"]),
                "peak_private_mb": number_or_blank(summary["peak_ram"]),
                "expected_samples": number_or_blank(summary["expected"]),
                "actual_samples": number_or_blank(summary["actual"]),
                "completion_percent": number_or_blank(summary["completion"]),
                "late_samples": number_or_blank(summary["late"]),
                "late_percent": number_or_blank(summary["late_percent"]),
                "missed_samples": number_or_blank(summary["missed"]),
                "avg_delay_ms": number_or_blank(summary["avg_delay"]),
                "p95_delay_ms": number_or_blank(summary["p95_delay"]),
                "p99_delay_ms": number_or_blank(summary["p99_delay"]),
                "max_delay_ms": number_or_blank(summary["max_delay"]),
                "runs": summary["run_count"],
            })


def number_or_blank(value: float | int | None) -> str:
    if value is None:
        return ""
    if isinstance(value, float):
        return f"{value:.6g}"
    return str(value)


def load_run(run_dir: Path, late_threshold: float) -> dict:
    del late_threshold
    metadata = load_json(run_dir / "metadata.json")
    resource_path = run_dir / "resource_usage.csv"
    if not resource_path.is_file():
        raise FileNotFoundError(resource_path)
    cpu, memory = read_resource_samples(resource_path)
    timing_path = run_dir / "sample_timing.csv"
    instrumented = bool(metadata.get("timing_instrumentation"))
    delays = None
    if instrumented:
        if not timing_path.is_file():
            raise FileNotFoundError(timing_path)
        delays = read_delays(timing_path)
    elif timing_path.is_file():
        delays = read_delays(timing_path)
        instrumented = True

    interval_control = bool(metadata.get("interval_control"))
    interval = metadata.get("sampling_interval_ms")
    label = "built-in"
    expected = None
    if interval_control and interval not in (None, ""):
        label = str(int(interval))
        duration = metadata.get("benchmark_duration_seconds")
        if instrumented and duration not in (None, ""):
            expected = expected_sample_count(int(duration), int(interval))
    return {
        "label": label,
        "cpu": cpu,
        "memory": memory,
        "delays": delays,
        "expected": expected,
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Summarize Sentinel benchmark runs.")
    parser.add_argument("directories", nargs="+", type=Path, help="A run directory or a directory of runs")
    parser.add_argument(
        "--late-threshold",
        type=float,
        default=0.10,
        help="Fraction of the sampling interval above which a sample is late (default: 0.10)",
    )
    parser.add_argument("--summary", type=Path, help="Where to write summary.csv")
    parser.add_argument("--no-summary", action="store_true", help="Print the table only")
    args = parser.parse_args(argv)
    if args.late_threshold < 0:
        parser.error("--late-threshold must be zero or positive")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    grouped: dict[str, list[dict]] = {}
    for directory in args.directories:
        for run_dir in find_runs(directory):
            loaded = load_run(run_dir, args.late_threshold)
            grouped.setdefault(loaded["label"], []).append(loaded)
    if not grouped:
        print("No benchmark runs found.", file=sys.stderr)
        return 2

    rows: list[tuple[str, dict]] = []
    for label in sorted(grouped, key=interval_sort_key):
        summary = summarize_group(grouped[label], args.late_threshold)
        summary["run_count"] = len(grouped[label])
        rows.append((label, summary))
    print_table(rows)

    if args.no_summary:
        return 0
    if args.summary is not None:
        summary_path = args.summary
    elif len(args.directories) == 1:
        summary_path = args.directories[0] / "summary.csv"
    else:
        print("Pass --summary to write a CSV for multiple input directories.", file=sys.stderr)
        return 0
    write_summary(summary_path, rows)
    print(f"\nWrote {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
