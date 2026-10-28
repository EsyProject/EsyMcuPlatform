# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Reject machine-readable OS coverage that omits production or counts fixtures."""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'esy_test/utils'))
from esy_coverage import coverage_entries

TESTS = Path(__file__).resolve().parent
SOURCE = (TESTS / '../src/freertos').resolve()


def check(report):
    expected = set(SOURCE.glob('*.c')) | {SOURCE / 'port/esp32/esy_os_context.c'}
    # Relative paths use platform, module or tests roots. Real internal headers
    # may appear in gcovr output, but foreign and duplicate header rows may not.
    coverage_entries(json.loads(report.read_text())['files'], expected,
                     (ROOT, TESTS.parent, TESTS), optional=SOURCE.rglob('*.h'))
    print(f'OS coverage population verified: {len(expected)} production C files; no fixtures.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', nargs='?', type=Path,
                        default=TESTS / 'build/artifacts/gcov/gcovr/coverage.json')
    check(parser.parse_args().report)
