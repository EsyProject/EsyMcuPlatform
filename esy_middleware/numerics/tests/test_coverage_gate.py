# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Verify that numerics coverage rejects missing sources and insufficient branches."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("gate", Path(__file__).with_name("check_coverage.py"))
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class CoverageTests(unittest.TestCase):
    def entries(self):
        return [{"file": f"src/{source.name}", "lines": [
            {"line_number": 1, "count": 1, "branches": [{"count": 1}, {"count": 1}]}]}
            for source in sorted(GATE.SOURCE.glob("*.c"))]

    def run_gate(self, entries):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "coverage.json"
            report.write_text(json.dumps({"files": entries}))
            with contextlib.redirect_stdout(io.StringIO()):
                GATE.check(report)

    def test_explicit_path_roots_are_supported(self):
        for prefix in (str(GATE.SOURCE), "esy_middleware/numerics/src", "src", "../src"):
            entries = self.entries()
            for entry in entries:
                entry["file"] = f"{prefix}/{Path(entry['file']).name}"
            with self.subTest(prefix=prefix):
                self.run_gate(entries)
        for entry in entries:
            entry["file"] = entry["file"].replace("/", "\\")
        self.run_gate(entries)

    def test_foreign_namesakes_and_basenames_cannot_replace_local_sources(self):
        name = Path(self.entries()[0]["file"]).name
        for path in (f"/tmp/foreign/esy_middleware/numerics/src/{name}",
                     f"foreign/esy_middleware/numerics/src/{name}", name):
            entries = self.entries()
            entries[0]["file"] = path
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, "Foreign"):
                self.run_gate(entries)

    def test_duplicate_aliases_and_foreign_extra_rows_fail(self):
        entries = self.entries()
        name = Path(entries[0]["file"]).name
        for path, error in ((entries[0]["file"], "Duplicate"),
                            (f"src/../src/{name}", "Duplicate"),
                            (str(GATE.SOURCE / name), "Duplicate"),
                            ("tests/fixture.c", "Foreign"),
                            ("tests/fixture.h", "Foreign")):
            extra = copy.deepcopy(entries[0])
            extra["file"] = path
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, error):
                self.run_gate(entries + [extra])

    def test_exact_threshold_and_noncode_lines(self):
        entries = self.entries()
        for entry in entries:
            entry["lines"] = [{"count": int(index >= 3),
                               "branches": [{"count": int(index >= 3)}]}
                              for index in range(20)]
            entry["lines"].append({"count": 0, "gcovr/noncode": True,
                                   "branches": [{"count": 0}]})
        self.run_gate(entries)

    def test_population_and_thresholds(self):
        entries = self.entries()
        # Report paths are rooted at the module, as configured in project.yml.
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "coverage.json"
            report.write_text(json.dumps({"files": entries}))
            with contextlib.redirect_stdout(io.StringIO()):
                GATE.check(report)
            cases = [[], entries[:-1], entries + [entries[0]],
                     [{"file": "fixture.c", "lines": []}]]
            for key in ("line", "branch", "empty", "empty_branches", "noncode_only"):
                invalid = copy.deepcopy(entries)
                if key == "line":
                    invalid[0]["lines"][0]["count"] = 0
                elif key == "branch":
                    invalid[0]["lines"][0]["branches"][0]["count"] = 0
                elif key == "empty":
                    invalid[0]["lines"] = []
                elif key == "empty_branches":
                    invalid[0]["lines"][0]["branches"] = []
                else:
                    invalid[0]["lines"][0]["gcovr/noncode"] = True
                cases.append(invalid)
            for invalid in cases:
                report.write_text(json.dumps({"files": invalid}))
                with self.assertRaises(ValueError), contextlib.redirect_stdout(io.StringIO()):
                    GATE.check(report)


if __name__ == "__main__":
    unittest.main()
