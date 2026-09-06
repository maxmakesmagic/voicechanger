#!/usr/bin/env python3

"""Validate the CI benchmark output and render it as a job-summary table."""

import argparse
import csv
import math
import os
from pathlib import Path
import re
import sys


EXPECTED_HEADER = [
    "ratio",
    "median_trial_mean_us_per_frame",
    "mad_trial_mean_us_per_frame",
    "min_trial_mean_us_per_frame",
    "p95_trial_mean_us_per_frame",
    "ns_per_sample",
    "average_throughput_factor",
    "output_checksum",
]
EXPECTED_RATIOS = (0.8, 1.0, 1.25)
WORKLOAD_PATTERN = re.compile(
    r"^blocks=(\d+) repetitions=(\d+) warmups=(\d+)$"
)
CHECKSUM_PATTERN = re.compile(r"^0x[0-9a-fA-F]{1,16}$")


class BenchmarkReportError(Exception):
    """The benchmark output does not satisfy the CI reporting contract."""


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("result", type=Path)
    parser.add_argument("--blocks", type=int, required=True)
    parser.add_argument("--repetitions", type=int, required=True)
    parser.add_argument("--warmups", type=int, required=True)
    return parser.parse_args()


def parse_result(path, expected_workload):
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise BenchmarkReportError(f"cannot read {path}: {error}") from error
    if len(lines) < 6 or not lines[0].startswith("compiler="):
        raise BenchmarkReportError("missing compiler or benchmark rows")

    workload_match = WORKLOAD_PATTERN.fullmatch(lines[1])
    if not workload_match:
        raise BenchmarkReportError("malformed workload line")
    actual_workload = tuple(int(value) for value in workload_match.groups())
    if actual_workload != expected_workload:
        raise BenchmarkReportError(
            f"expected workload {expected_workload}, got {actual_workload}"
        )

    rows = list(csv.reader(lines[2:]))
    if not rows or rows[0] != EXPECTED_HEADER:
        raise BenchmarkReportError("unexpected CSV header")
    data_rows = rows[1:]
    if len(data_rows) != len(EXPECTED_RATIOS):
        raise BenchmarkReportError(
            f"expected {len(EXPECTED_RATIOS)} ratios, got {len(data_rows)}"
        )

    parsed_rows = []
    for expected_ratio, row in zip(EXPECTED_RATIOS, data_rows):
        if len(row) != len(EXPECTED_HEADER):
            raise BenchmarkReportError("benchmark row does not have eight columns")
        try:
            ratio = float(row[0])
            metrics = [float(value) for value in row[1:7]]
        except ValueError as error:
            raise BenchmarkReportError("benchmark row contains a non-number") from error
        if not math.isclose(ratio, expected_ratio, rel_tol=0.0, abs_tol=1e-6):
            raise BenchmarkReportError(
                f"expected ratio {expected_ratio:g}, got {ratio:g}"
            )
        if not all(math.isfinite(value) for value in metrics):
            raise BenchmarkReportError("benchmark row contains a non-finite metric")

        median, mad, minimum, p95, nanoseconds, throughput = metrics
        if (
            median <= 0.0
            or mad < 0.0
            or minimum <= 0.0
            or p95 <= 0.0
            or nanoseconds <= 0.0
            or throughput <= 0.0
            or not minimum <= median <= p95
        ):
            raise BenchmarkReportError(
                f"invalid timing relationship for ratio {ratio:g}"
            )
        if not CHECKSUM_PATTERN.fullmatch(row[7]):
            raise BenchmarkReportError(f"invalid checksum for ratio {ratio:g}")
        parsed_rows.append((ratio, median, mad, p95, throughput, row[7]))
    return lines[0].removeprefix("compiler="), actual_workload, parsed_rows


def markdown_report(compiler, workload, rows):
    blocks, repetitions, warmups = workload
    lines = [
        "### Host pitch-shift benchmark",
        "",
        f"Compiler: `{compiler}`. Workload: {blocks:,} blocks, "
        f"{repetitions} measured trials, {warmups} warm-ups.",
        "",
        "| Ratio | Median us/frame | MAD | p95 us/frame | Throughput | Checksum |",
        "|---:|---:|---:|---:|---:|---|",
    ]
    for ratio, median, mad, p95, throughput, checksum in rows:
        lines.append(
            f"| {ratio:g} | {median:.3f} | {mad:.3f} | {p95:.3f} | "
            f"{throughput:.3f}x | `{checksum}` |"
        )
    lines.extend(
        [
            "",
            "Host timing is a trend report, not a blocking threshold; shared "
            "runner load is not deterministic.",
        ]
    )
    return "\n".join(lines) + "\n"


def publish(report):
    print(report, end="", flush=True)
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        try:
            with open(summary_path, "a", encoding="utf-8") as summary:
                summary.write(report)
        except OSError as error:
            raise BenchmarkReportError(
                f"cannot write GitHub job summary: {error}"
            ) from error


def main():
    args = parse_args()
    try:
        compiler, workload, rows = parse_result(
            args.result, (args.blocks, args.repetitions, args.warmups)
        )
        publish(markdown_report(compiler, workload, rows))
    except BenchmarkReportError as error:
        print(f"Benchmark report failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
