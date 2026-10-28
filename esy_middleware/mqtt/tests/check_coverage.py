# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Require MQTT core coverage; optional transport adapters have their own suites."""

import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "esy_test/utils"))
from esy_coverage import coverage_entries

SOURCE = Path(__file__).resolve().parent.parent / "src"


def check(report, minimum=85.0):
    entries = json.loads(report.read_text())["files"]
    files = coverage_entries(entries, [SOURCE / "esy_mqtt.c"],
                             (ROOT, SOURCE.parent, SOURCE.parent / "tests"))
    for entry in files.values():
        lines = [line for line in entry["lines"] if not line.get("gcovr/noncode", False)]
        covered = sum(line["count"] > 0 for line in lines)
        percent = 100.0 * covered / len(lines) if lines else 0.0
        if percent < minimum:
            raise ValueError(f"{entry['file']}: {percent:.1f}% below {minimum}%")
        print(f"{entry['file']}: {percent:.1f}% >= {minimum}%")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--minimum", type=float, default=85.0)
    arguments = parser.parse_args()
    check(arguments.report, arguments.minimum)
