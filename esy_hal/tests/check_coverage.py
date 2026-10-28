# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Require the complete platform source population and per-file coverage floors."""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'esy_test/utils'))
from esy_coverage import coverage_entries

SOURCE = Path(__file__).resolve().parent.parent / 'src'
# Wi-Fi has a separate real-target consumer suite. Empty translation units have
# no executable code; disabled-direction implementations are tested by CMake.
EXCLUDED = {'esy_hal_wifi.c', 'esy_hal_empty.c',
            'xio/esy_hal_xio_uart_tx_disabled.c',
            'xio/esy_hal_xio_uart_rx_disabled.c'}


def expected_sources():
    return {str(p.relative_to(SOURCE)) for p in SOURCE.rglob('*.c')} - EXCLUDED


def check(report):
    entries = json.loads(report.read_text())['files']
    # Relative report paths are rooted at the platform, module or test directory.
    files = coverage_entries(entries, (SOURCE / name for name in expected_sources()),
                             (ROOT, SOURCE.parent, SOURCE.parent / 'tests'))
    total = covered_total = 0
    for path, entry in files.items():
        lines = [line for line in entry['lines'] if not line.get('gcovr/noncode', False)]
        covered = sum(line['count'] > 0 for line in lines)
        percent = 100 * covered / len(lines) if lines else 0
        if percent < 65:
            raise ValueError(f'{path}: {percent:.2f}% below the 65% per-file floor')
        total += len(lines)
        covered_total += covered
    percent = 100 * covered_total / total if total else 0
    if percent < 85:
        raise ValueError(f'Aggregate lines: {percent:.2f}% below 85%')
    print(f'Platform coverage: {len(files)} sources, {covered_total}/{total} lines ({percent:.2f}%)')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    check(parser.parse_args().report)
