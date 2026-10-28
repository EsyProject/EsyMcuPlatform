# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Verify network coverage inventories every raw lwIP provider source."""

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
                for path in sorted(SOURCE.rglob("*.c"))]

    def run_check(self, entries):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "coverage.json"
            report.write_text(json.dumps({"files": entries}))
            with contextlib.redirect_stdout(io.StringIO()):
                check(report)

    def test_threshold_and_noncode_lines(self):
        entries = self.entries()
        entries[0]["lines"][:3] = [{"count": 0} for _ in range(3)]
        entries[0]["lines"].append({"count": 0, "gcovr/noncode": True})
        self.run_check(entries)
        entries[0]["lines"][3] = {"count": 0}
        with self.assertRaisesRegex(ValueError, "below"):
            self.run_check(entries)

    def test_explicit_path_roots_are_supported(self):
        for root in (SOURCE, Path("esy_middleware/network/src"), Path("src"),
                     Path("../../src")):
            entries = self.entries()
            for entry, path in zip(entries, sorted(SOURCE.rglob("*.c"))):
                entry["file"] = str(root / path.relative_to(SOURCE))
            with self.subTest(root=root):
                self.run_check(entries)
            for entry in entries:
                entry["file"] = entry["file"].replace("/", "\\")
            self.run_check(entries)

    def test_absent_empty_foreign_and_duplicate_sources_fail(self):
        entries = self.entries()
        for population, error in (
            ([], "Missing"),
            (entries + entries, "Duplicate"),
            ([{"file": "esy_network_lwip.c", "lines": []}], "Foreign"),
            ([{"file": "/tmp/foreign/esy_middleware/network/src/esy_network_lwip.c", "lines": []}], "Foreign"),
            ([{"file": str(SOURCE / "esy_network_lwip.c"), "lines": []}] + entries[1:], "below"),
            (entries + [{"file": "tests/fixture.h", "lines": []}], "Foreign"),
        ):
            with self.subTest(error=error), self.assertRaisesRegex(ValueError, error):
                self.run_check(population)

    def test_normalized_alias_cannot_duplicate_the_core(self):
        entries = self.entries()
        for path in ("src/esy_network_lwip.c", "src/../src/esy_network_lwip.c", "../../src/esy_network_lwip.c"):
            duplicate = copy.deepcopy(entries[0])
            duplicate["file"] = path
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, "Duplicate"):
                self.run_check(entries + [duplicate])


if __name__ == "__main__":
    unittest.main()
