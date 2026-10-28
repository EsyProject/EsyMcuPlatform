# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Verify Wi-Fi coverage gates reject incomplete or weak evidence."""
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

from check_coverage import ROOT, SOURCE, check


class CoverageGateTests(unittest.TestCase):
    def entries(self):
        return [{"file": str(path), "lines": [
            {"count": 1, "branches": [{"count": 1}, {"count": 1}]}
            for _ in range(20)]} for path in sorted(SOURCE.glob("*.c"))]

    def run_check(self, entries):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "coverage.json"
            report.write_text(json.dumps({"files": entries}))
            with contextlib.redirect_stdout(io.StringIO()):
                check(report)

    def test_threshold_boundary_and_noncode(self):
        entries = self.entries()
        for line in entries[0]["lines"][:3]:
            line["count"] = 0
            line["branches"] = [{"count": 0}, {"count": 0}]
        entries[0]["lines"].append({"count": 0, "gcovr/noncode": True})
        self.run_check(entries)

    def test_low_line_or_branch_coverage_fails(self):
        for kind in ("line", "branch"):
            entries = self.entries()
            for line in entries[0]["lines"][:4]:
                if kind == "line":
                    line["count"] = 0
                else:
                    line["branches"] = [{"count": 0}, {"count": 0}]
            with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, "below"):
                self.run_check(entries)

    def test_missing_duplicate_and_foreign_population_fails(self):
        entries = self.entries()
        for population in ([], entries + entries, [dict(entries[0], file="fake.c")]):
            with self.subTest(population=population), self.assertRaisesRegex(ValueError, "population|Foreign"):
                self.run_check(population)

    def test_explicit_local_path_roots_and_windows_separators_pass(self):
        for prefix in (str(SOURCE), str(SOURCE.relative_to(ROOT)), "src", "../src"):
            entries = self.entries()
            for entry in entries:
                entry["file"] = f"{prefix}/{Path(entry['file']).name}"
            with self.subTest(prefix=prefix):
                self.run_check(entries)
            for entry in entries:
                entry["file"] = entry["file"].replace("/", "\\")
            self.run_check(entries)

    def test_foreign_checkout_namesakes_and_basenames_fail(self):
        name = Path(self.entries()[0]["file"]).name
        for path in (f"/tmp/foreign/esy_middleware/wifi/src/{name}",
                     f"foreign/esy_middleware/wifi/src/{name}", name):
            entries = self.entries()
            entries[0]["file"] = path
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, "Foreign"):
                self.run_check(entries)

    def test_canonical_duplicate_aliases_fail(self):
        name = Path(self.entries()[0]["file"]).name
        for alias in (f"src/{name}", f"src/../src/{name}",
                      str(SOURCE / ".." / "src" / name)):
            entries = self.entries()
            extra = dict(entries[0], file=alias)
            with self.subTest(alias=alias), self.assertRaisesRegex(ValueError, "Duplicate"):
                self.run_check(entries + [extra])

    def test_empty_lines_or_branches_fail(self):
        for lines in ([], [{"count": 1}]):
            entries = self.entries()
            entries[0]["lines"] = lines
            with self.subTest(lines=lines), self.assertRaisesRegex(ValueError, "below"):
                self.run_check(entries)


if __name__ == "__main__":
    unittest.main()
