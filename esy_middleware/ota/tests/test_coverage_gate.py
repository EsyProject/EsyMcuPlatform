# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Exercise OTA coverage population and per-source threshold failures."""
import contextlib
import copy
import io
import json
from pathlib import Path
import tempfile
import unittest

from check_coverage import SOURCE, check


class CoverageGateTests(unittest.TestCase):
    def entries(self):
        return [{"file": str(path), "lines": [{"count": 1} for _ in range(20)]}
                for path in sorted(SOURCE.glob("*.c"))]

    def run_check(self, entries):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "coverage.json"
            report.write_text(json.dumps({"files": entries}))
            with contextlib.redirect_stdout(io.StringIO()):
                check(report, 85.0)

    def test_threshold_and_noncode_lines(self):
        entries = self.entries()
        entries[0]["lines"][:3] = [{"count": 0} for _ in range(3)]
        entries[0]["lines"].append({"count": 0, "gcovr/noncode": True})
        self.run_check(entries)

    def test_explicit_path_roots_are_supported(self):
        for prefix in (str(SOURCE), "esy_middleware/ota/src", "src", "../src"):
            entries = self.entries()
            for entry in entries:
                entry["file"] = f"{prefix}/{Path(entry['file']).name}"
            with self.subTest(prefix=prefix):
                self.run_check(entries)
        for entry in entries:
            entry["file"] = entry["file"].replace("/", "\\")
        self.run_check(entries)

    def test_foreign_namesakes_and_basenames_cannot_replace_local_sources(self):
        name = Path(self.entries()[0]["file"]).name
        for path in (f"/tmp/foreign/esy_middleware/ota/src/{name}",
                     f"foreign/esy_middleware/ota/src/{name}", name):
            entries = self.entries()
            entries[0]["file"] = path
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, "Foreign"):
                self.run_check(entries)

    def test_normalized_and_mixed_path_duplicate_aliases_fail(self):
        entries = self.entries()
        name = Path(entries[0]["file"]).name
        for path in (f"src/{name}", f"src/../src/{name}", f"../src/{name}"):
            duplicate = copy.deepcopy(entries[0])
            duplicate["file"] = path
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, "Duplicate"):
                self.run_check(entries + [duplicate])

    def test_one_low_source_fails_despite_high_aggregate(self):
        entries = self.entries()
        entries[0]["lines"][:4] = [{"count": 0} for _ in range(4)]
        with self.assertRaisesRegex(ValueError, "below"):
            self.run_check(entries)

    def test_missing_duplicate_and_foreign_sources_fail(self):
        entries = self.entries()
        for population, error in (([], "Missing"), (entries[:-1], "Missing"),
                                  (entries + entries[:1], "Duplicate"),
                                  (entries + [{"file": "foreign.c", "lines": []}], "Foreign"),
                                  (entries + [{"file": "tests/fixture.h", "lines": []}], "Foreign")):
            with self.subTest(population=population):
                with self.assertRaisesRegex(ValueError, error):
                    self.run_check(population)

    def test_empty_source_fails(self):
        for lines in ([], [{"count": 1, "gcovr/noncode": True}]):
            entries = self.entries()
            entries[0]["lines"] = lines
            with self.subTest(lines=lines), self.assertRaisesRegex(ValueError, "below"):
                self.run_check(entries)


if __name__ == "__main__":
    unittest.main()
