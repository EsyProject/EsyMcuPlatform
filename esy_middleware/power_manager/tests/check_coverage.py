"""Reject incomplete power-manager coverage and enforce line/branch floors."""

import argparse
import json
from pathlib import Path

SOURCE = Path(__file__).resolve().parent.parent / "src"
PREFIX = "esy_middleware/power_manager/src/"


def check(report, minimum_line=85.0, minimum_branch=70.0):
    files = json.loads(report.read_text())["files"]
    expected = {PREFIX + path.name for path in SOURCE.glob("*.c")}
    actual = {entry["file"] for entry in files}
    if actual != expected or len(files) != len(expected):
        raise ValueError(f"Unexpected source population: {actual}; expected {expected}")
    branches = []
    for entry in files:
        lines = [line for line in entry["lines"]
                 if not line.get("gcovr/noncode", False)]
        covered = sum(line["count"] > 0 for line in lines)
        percent = 100.0 * covered / len(lines) if lines else 0.0
        if not lines or percent < minimum_line:
            raise ValueError(f"{entry['file']}: {percent:.1f}% below {minimum_line}%")
        branches.extend(branch for line in lines for branch in line.get("branches", []))
        print(f"{entry['file']}: lines {percent:.1f}% >= {minimum_line}%")
    covered = sum(branch["count"] > 0 for branch in branches)
    percent = 100.0 * covered / len(branches) if branches else 0.0
    if not branches or percent < minimum_branch:
        raise ValueError(f"Branches: {percent:.1f}% below {minimum_branch}%")
    print(f"Branches: {percent:.1f}% >= {minimum_branch}%")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    check(args.report)
