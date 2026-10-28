"""Exercise rejection paths in the module's coverage evidence gate."""

import contextlib
import copy
import io
import json
from pathlib import Path
import tempfile
import unittest

from check_coverage import PREFIX, SOURCE, check


class CoverageGateTest(unittest.TestCase):
    def setUp(self):
        self.report = {"files": [
            {"file": PREFIX + path.name, "lines": [
                {"count": 1, "branches": [{"count": 1}, {"count": 1}]}
                for _ in range(20)]}
            for path in sorted(SOURCE.glob("*.c"))]}

    def run_gate(self, report):
        with tempfile.TemporaryDirectory(prefix="power-coverage-") as directory:
            path = Path(directory) / "coverage.json"
            path.write_text(json.dumps(report))
            with contextlib.redirect_stdout(io.StringIO()):
                check(path)

    def test_complete_coverage_passes(self):
        self.run_gate(self.report)

    def test_invalid_source_populations_fail(self):
        for mode in ("missing", "duplicate", "foreign", "empty"):
            with self.subTest(mode=mode):
                report = copy.deepcopy(self.report)
                if mode == "missing":
                    report["files"].pop()
                elif mode == "duplicate":
                    report["files"].append(report["files"][0])
                elif mode == "foreign":
                    report["files"][0]["file"] = "foreign/esy_power_manager.c"
                else:
                    report["files"] = []
                with self.assertRaises(ValueError):
                    self.run_gate(report)

    def test_one_low_or_empty_file_fails_despite_aggregate(self):
        for mode in ("low", "empty", "noncode"):
            with self.subTest(mode=mode):
                report = copy.deepcopy(self.report)
                lines = report["files"][0]["lines"]
                if mode == "low":
                    for line in lines[:4]:
                        line["count"] = 0
                elif mode == "empty":
                    lines.clear()
                else:
                    for line in lines:
                        line["gcovr/noncode"] = True
                with self.assertRaises(ValueError):
                    self.run_gate(report)

    def test_missing_or_low_branch_coverage_fails(self):
        for mode in ("missing", "low"):
            with self.subTest(mode=mode):
                report = copy.deepcopy(self.report)
                for entry in report["files"]:
                    for line in entry["lines"]:
                        if mode == "missing":
                            line["branches"] = []
                        else:
                            line["branches"][0]["count"] = 0
                with self.assertRaises(ValueError):
                    self.run_gate(report)


if __name__ == "__main__":
    unittest.main()
