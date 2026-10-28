# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0
"""Require every I/O scheduler source, unique paths and per-file line coverage."""

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "esy_test/utils"))
from esy_coverage import check_module_lines

SOURCE_DIR = ROOT / "esy_middleware/io_scheduler/src"
LINE_MIN = 85


def check(report):
    return check_module_lines(ROOT, "esy_middleware/io_scheduler", report, minimum=LINE_MIN)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    try:
        check(args.report)
    except (OSError, ValueError) as error:
        print(f"[io-scheduler-coverage] {error}", file=sys.stderr)
        sys.exit(1)
    print("[io-scheduler-coverage] Every source meets the line threshold; no duplicate/foreign rows")
