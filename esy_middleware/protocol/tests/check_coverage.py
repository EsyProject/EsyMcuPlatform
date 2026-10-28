#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0
"""Require complete, unique protocol source coverage and per-file line floors."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "esy_test/utils"))
from esy_coverage import check_module_lines

MODULES = ("esy_common/protocol_codec", "esy_middleware/protocol")


def check(module, report):
    count = check_module_lines(ROOT, module, report, minimum=85, require_total=True,
                              extra_anchors=(ROOT / module / "tests/json",))
    print(f"{module}: all {count} sources meet 85% line coverage; no duplicate/foreign rows")


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4, 5):
        raise SystemExit("usage: check_coverage.py COMMON_REPORT MIDDLEWARE_REPORT [JSON_REPORT [JSON_TCP_REPORT]]")
    common_reports = (sys.argv[1], sys.argv[3]) if len(sys.argv) >= 4 else sys.argv[1]
    middleware_reports = (sys.argv[2], sys.argv[4]) if len(sys.argv) == 5 else sys.argv[2]
    check(MODULES[0], common_reports)
    check(MODULES[1], middleware_reports)
