# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0
"""Reject omitted, aliased, foreign or individually under-tested adapters."""

import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("io_scheduler_coverage", Path(__file__).with_name("check_coverage.py"))
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class CoverageGateTests(unittest.TestCase):
    def test_all_sources_are_required_individually(self):
        sources = sorted(GATE.SOURCE_DIR.glob("*.c"))
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "coverage.txt"
            complete = "\n".join(f"src/{p.name} 100 90 90%" for p in sources)
            report.write_text(complete + "\nTOTAL 300 290 97%\n")
            GATE.check(report)
            report.write_text(complete.replace(".c 100", ".c\n    100"))
            GATE.check(report)
            for source in sources:
                with self.subTest(source=source.name):
                    report.write_text(complete.replace(f"src/{source.name} 100 90 90%", "") + "\nTOTAL 300 300 100%\n")
                    with self.assertRaisesRegex(ValueError, "Missing esy_middleware/io_scheduler coverage"):
                        GATE.check(report)
                    report.write_text(complete.replace(f"{source.name} 100 90 90%", f"{source.name} 100 84 84%") + "\nTOTAL 300 299 99%\n")
                    with self.assertRaisesRegex(ValueError, "requires 85%"):
                        GATE.check(report)

    def test_full_paths_reject_duplicate_aliases_and_foreign_namesakes(self):
        sources = sorted(GATE.SOURCE_DIR.glob("*.c"))
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "coverage.txt"
            rows = [f"{path} 100 90 90%" for path in sources]
            headers = sorted(GATE.SOURCE_DIR.glob("*.h"))
            complete = "\n".join(rows + [f"{path}\n  10 9 90%" for path in headers])
            report.write_text(complete)
            GATE.check(report)
            for extra, error in (
                (rows[0], "Duplicate"),
                (f"src/../src/{sources[0].name} 100 90 90%", "Duplicate"),
                (f"/tmp/foreign/{sources[0].name} 100 90 90%", "Foreign"),
                (f"{GATE.ROOT}/esy_test/foreign.c 100 90 90%", "Foreign"),
                (f"{GATE.ROOT}/esy_test/foreign.h 100 90 90%", "Foreign"),
                (f"{sources[0].name} 100 90 90%", "Foreign"),
            ):
                with self.subTest(extra=extra):
                    report.write_text(complete + "\n" + extra)
                    with self.assertRaisesRegex(ValueError, error):
                        GATE.check(report)

    def test_actual_counts_override_a_misleading_display_percentage(self):
        sources = sorted(GATE.SOURCE_DIR.glob("*.c"))
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "coverage.txt"
            rows = [f"src/{path.name} 100 90 90%" for path in sources]
            report.write_text("\n".join(rows).replace("100 90 90%", "100 84 100%", 1))
            with self.assertRaisesRegex(ValueError, "requires 85%"):
                GATE.check(report)


if __name__ == "__main__":
    unittest.main()
