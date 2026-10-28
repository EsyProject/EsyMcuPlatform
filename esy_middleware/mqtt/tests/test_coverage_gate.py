# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Verify MQTT coverage cannot pass through absent, foreign or aliased sources."""

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
        return [{"file": str(SOURCE / "esy_mqtt.c"),
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

    def test_explicit_path_roots_are_supported(self):
        for prefix in (str(SOURCE), "esy_middleware/mqtt/src", "src", "../src"):
            entries = self.entries()
            entries[0]["file"] = f"{prefix}/esy_mqtt.c"
            with self.subTest(prefix=prefix):
                self.run_check(entries)
            entries[0]["file"] = entries[0]["file"].replace("/", "\\")
            self.run_check(entries)

    def test_absent_empty_foreign_and_duplicate_sources_fail(self):
        entries = self.entries()
        for population, error in (
            ([], "Missing"),
            (entries + entries, "Duplicate"),
            ([{"file": "esy_mqtt.c", "lines": []}], "Foreign"),
            ([{"file": "/tmp/foreign/esy_middleware/mqtt/src/esy_mqtt.c", "lines": []}], "Foreign"),
            ([{"file": str(SOURCE / "esy_mqtt.c"), "lines": []}], "below"),
            (entries + [{"file": "tests/fixture.h", "lines": []}], "Foreign"),
        ):
            with self.subTest(error=error), self.assertRaisesRegex(ValueError, error):
                self.run_check(population)

    def test_normalized_alias_cannot_duplicate_the_core(self):
        entries = self.entries()
        for path in ("src/esy_mqtt.c", "src/../src/esy_mqtt.c", "../src/esy_mqtt.c"):
            duplicate = copy.deepcopy(entries[0])
            duplicate["file"] = path
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, "Duplicate"):
                self.run_check(entries + [duplicate])


if __name__ == "__main__":
    unittest.main()
