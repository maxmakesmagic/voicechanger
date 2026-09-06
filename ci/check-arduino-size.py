#!/usr/bin/env python3

"""Check Teensy memory use against per-board, per-sketch byte budgets."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


SECTIONS = ("FLASH", "RAM1", "RAM2")


class SizeCheckError(Exception):
    """A size report or budget is missing or malformed."""


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--budgets", required=True, type=Path)
    parser.add_argument("--board", required=True)
    parser.add_argument("--sketch", required=True)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--teensy-size", required=True, type=Path)
    return parser.parse_args()


def validate_fqbn(fqbn):
    parts = fqbn.split(":")
    if len(parts) < 3 or not all(parts[:3]):
        raise SizeCheckError("board must be a fully qualified board name")
    return fqbn


def load_budgets(path, board, sketch, measured_sizes):
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise SizeCheckError(f"cannot read budget file {path}: {error}") from error
    except json.JSONDecodeError as error:
        raise SizeCheckError(f"invalid JSON in budget file {path}: {error}") from error

    try:
        budgets = document[board][sketch]
    except (KeyError, TypeError) as error:
        baseline = ", ".join(
            f"{section}={measured_sizes[section]}" for section in SECTIONS
        )
        raise SizeCheckError(
            f"no size budget for sketch {sketch!r} on {board!r}; "
            f"add its measured baseline to {path}: {baseline}"
        ) from error

    if not isinstance(budgets, dict) or set(budgets) != set(SECTIONS):
        raise SizeCheckError(
            f"budget for {sketch!r} on {board!r} must define exactly "
            + ", ".join(SECTIONS)
        )
    for section, limit in budgets.items():
        if isinstance(limit, bool) or not isinstance(limit, int) or limit < 0:
            raise SizeCheckError(f"{section} budget must be a non-negative integer")
    return budgets


def measure(teensy_size, elf):
    if not teensy_size.is_file():
        raise SizeCheckError(f"Teensy size tool does not exist: {teensy_size}")
    if not elf.is_file():
        raise SizeCheckError(f"compiled ELF does not exist: {elf}")

    try:
        result = subprocess.run(
            [str(teensy_size), "--json", str(elf)],
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError as error:
        raise SizeCheckError(f"cannot run {teensy_size}: {error}") from error
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "no diagnostic"
        raise SizeCheckError(f"Teensy size tool failed: {detail}")

    try:
        report = json.loads(result.stdout)
        raw_sections = report["sections"]
    except (json.JSONDecodeError, KeyError, TypeError) as error:
        raise SizeCheckError("Teensy size tool returned malformed JSON") from error
    if not isinstance(raw_sections, list):
        raise SizeCheckError("Teensy size report has no section list")

    sections = {}
    capacities = {}
    for entry in raw_sections:
        try:
            name = entry["name"]
            size = entry["size"]
            maximum = entry["max_size"]
        except (KeyError, TypeError) as error:
            raise SizeCheckError("malformed section in Teensy size report") from error
        if (
            not isinstance(name, str)
            or isinstance(size, bool)
            or not isinstance(size, int)
            or isinstance(maximum, bool)
            or not isinstance(maximum, int)
            or size < 0
            or maximum <= 0
            or size > maximum
        ):
            raise SizeCheckError("invalid values in Teensy size report")
        if name in sections:
            raise SizeCheckError(f"duplicate {name!r} section in Teensy size report")
        sections[name] = size
        capacities[name] = maximum

    missing = [section for section in SECTIONS if section not in sections]
    if missing:
        raise SizeCheckError(
            "Teensy size report is missing: " + ", ".join(missing)
        )
    return sections, capacities


def markdown_report(board, sketch, sizes, capacities, budgets):
    lines = [
        f"### Arduino size: `{sketch}` on `{board}`",
        "",
        "| Section | Used | Budget | Device capacity | Budget headroom |",
        "|---|---:|---:|---:|---:|",
    ]
    for section in SECTIONS:
        remaining = budgets[section] - sizes[section]
        lines.append(
            f"| {section} | {sizes[section]:,} B | {budgets[section]:,} B | "
            f"{capacities[section]:,} B | {remaining:+,} B |"
        )
    return "\n".join(lines) + "\n"


def publish(report):
    print(report, end="", flush=True)
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        try:
            with open(summary_path, "a", encoding="utf-8") as summary:
                summary.write(report)
                summary.write("\n")
        except OSError as error:
            raise SizeCheckError(f"cannot write GitHub job summary: {error}") from error


def main():
    args = parse_args()
    try:
        board = validate_fqbn(args.board)
        sizes, capacities = measure(args.teensy_size, args.elf)
        budgets = load_budgets(args.budgets, board, args.sketch, sizes)
        impossible_budgets = [
            section
            for section in SECTIONS
            if budgets[section] > capacities[section]
        ]
        if impossible_budgets:
            raise SizeCheckError(
                "budget exceeds device capacity for: "
                + ", ".join(impossible_budgets)
            )
        publish(
            markdown_report(board, args.sketch, sizes, capacities, budgets)
        )
        failures = []
        for section in SECTIONS:
            excess = sizes[section] - budgets[section]
            if excess > 0:
                unit = "byte" if excess == 1 else "bytes"
                failures.append(
                    f"{section} exceeds its budget by {excess:,} {unit}"
                )
        if failures:
            raise SizeCheckError("; ".join(failures))
    except SizeCheckError as error:
        print(f"Arduino size check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
