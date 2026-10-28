# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Verify that HTTP coverage requires the real adapter and its configured threshold."""

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
        return [{"file": str(SOURCE / "esy_http_client.c"),
                 "lines": [{"count": 1} for _ in range(20)]}]

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

    def test_explicit_roots_and_normalized_aliases(self):
        for prefix in (str(SOURCE), "esy_middleware/http/src", "src", "../src"):
            entries = self.entries()
            entries[0]["file"] = f"{prefix}/esy_http_client.c"
            with self.subTest(prefix=prefix):
                self.run_check(entries)
                entries[0]["file"] = entries[0]["file"].replace("/", "\\")
                self.run_check(entries)
        duplicate = copy.deepcopy(entries[0])
        duplicate["file"] = "src/../src/esy_http_client.c"
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            self.run_check(self.entries() + [duplicate])

    def test_missing_foreign_empty_and_duplicate_sources_fail(self):
        entries = self.entries()
        for population, error in (
            ([], "Missing"),
            (entries + entries, "Duplicate"),
            ([{"file": "esy_http_client.c", "lines": []}], "Foreign"),
            ([{"file": "/tmp/foreign/esy_middleware/http/src/esy_http_client.c", "lines": []}], "Foreign"),
            ([{"file": str(SOURCE / "esy_http_client.c"), "lines": []}], "below"),
            (entries + [{"file": "tests/fixture.h", "lines": []}], "Foreign"),
        ):
            with self.subTest(error=error), self.assertRaisesRegex(ValueError, error):
                self.run_check(population)


if __name__ == "__main__":
    unittest.main()
