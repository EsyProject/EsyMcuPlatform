# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Require numerics production sources and minimum line/branch coverage."""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "esy_test/utils"))
from esy_coverage import coverage_entries

TESTS = Path(__file__).resolve().parent
SOURCE = TESTS.parent / "src"


def check(report, line_min=85.0, branch_min=85.0):
    entries = json.loads(report.read_text())["files"]
    # Relative report paths are rooted at the platform, module or test directory.
    files = coverage_entries(entries, SOURCE.glob("*.c"), (ROOT, SOURCE.parent, TESTS))
    for entry in files.values():
        lines = [line for line in entry["lines"] if not line.get("gcovr/noncode", False)]
        branches = [branch for line in lines for branch in line.get("branches", [])]
        for kind, items, minimum in (("line", lines, line_min),
                                     ("branch", branches, branch_min)):
            percent = 100.0 * sum(item["count"] > 0 for item in items) / len(items) if items else 0.0
            if percent < minimum:
                raise ValueError(f"{entry['file']}: {kind} {percent:.1f}% below {minimum}%")
            print(f"{entry['file']}: {kind} {percent:.1f}% >= {minimum}%")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    check(args.report)
