#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0
"""Reject missing, duplicate, foreign and individually under-covered sources."""
import contextlib
import importlib.util
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location("gate", Path(__file__).with_name("check_coverage.py"))
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)

class CoverageGateTests(unittest.TestCase):
    def exercise(self, module, mutate=None, error=None):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "coverage.txt"
            sources = sorted((GATE.ROOT / module / "src").rglob("*.c"))
            self.assertTrue(sources, f"No production sources for {module}")
            rows = [f"{path} 100 100 100%" for path in sources]
            if mutate is not None:
                rows = mutate(rows, sources)
            report.write_text("\n".join(rows) + "\nTOTAL 100 100 100%\n")
            if error is None:
                with contextlib.redirect_stdout(io.StringIO()):
                    GATE.check(module, report)
            else:
                with self.assertRaisesRegex(ValueError, error):
                    GATE.check(module, report)

    def test_absolute_relative_and_wrapped_paths_are_supported(self):
        for module in GATE.MODULES:
            with self.subTest(module=module):
                self.exercise(module)
                self.exercise(module, lambda rows, sources: [
                    f"{path.relative_to(GATE.ROOT)}\n   100 100 100%" for path in sources])
                self.exercise(module, lambda rows, sources: [
                    f"../src/{path.relative_to(GATE.ROOT / module / 'src')} 100 100 100%"
                    for path in sources])

    def test_missing_and_low_sources_fail_despite_high_aggregate(self):
        for module in GATE.MODULES:
            with self.subTest(module=module):
                self.exercise(module, lambda rows, sources: rows[1:], "Missing")
                self.exercise(module, lambda rows, sources: [
                    rows[0].replace("100 100 100%", "100 84 100%")] + rows[1:], "requires 85%")
                self.exercise(module, lambda rows, sources: [
                    rows[0].replace("100 100 100%", "0 0 100%")] + rows[1:], "requires 85%")

    def test_duplicate_alias_foreign_and_basename_rows_are_rejected(self):
        for module in GATE.MODULES:
            for mutation, error in (
                (lambda rows, sources: rows + [rows[0]], "Duplicate"),
                (lambda rows, sources: rows + [
                    f"src/../src/{sources[0].relative_to(GATE.ROOT / module / 'src')} 100 100 100%"], "Duplicate"),
                (lambda rows, sources: [f"/tmp/foreign/{sources[0].name} 100 100 100%"] + rows[1:], "Foreign"),
                (lambda rows, sources: rows + [f"{GATE.ROOT}/esy_test/foreign.c 100 100 100%"], "Foreign"),
                (lambda rows, sources: [f"{sources[0].name} 100 100 100%"] + rows[1:], "Foreign"),
                (lambda rows, sources: rows + ["TOTAL 100 100 100%"], "Duplicate"),
                (lambda rows, sources: [rows[0].replace("100 100 100%", "100 101 100%")] + rows[1:], "Invalid"),
            ):
                with self.subTest(module=module, error=error, mutation=mutation):
                    self.exercise(module, mutation, error)

    def test_every_nested_family_source_is_required(self):
        module = "esy_middleware/protocol"
        source_dir = GATE.ROOT / module / "src"
        nested = sorted(path for path in source_dir.rglob("*.c")
                        if path.parent != source_dir)
        self.assertTrue(nested)
        for source in nested:
            with self.subTest(source=source.relative_to(source_dir)):
                self.exercise(module, lambda rows, sources: [
                    row for row, path in zip(rows, sources) if path != source], "Missing")

    def test_real_suite_filters_partition_every_production_source_once(self):
        projects = [GATE.ROOT / module / relative
                    for module in GATE.MODULES
                    for relative in ("tests/project.yml", "tests/json/project.yml")]
        configuration = subprocess.run(
            ["ruby", "-ryaml", "-rjson", "-e",
             "puts JSON.generate(ARGV.map { |path| YAML.unsafe_load_file(path)"
             ".fetch(:gcov).fetch(:gcovr).fetch(:report_include) })",
             *(str(project) for project in projects)],
            capture_output=True, text=True, check=True)
        filters = json.loads(configuration.stdout)
        for index, module in enumerate(GATE.MODULES):
            module_projects = projects[index * 2:index * 2 + 2]
            expressions = [re.compile(pattern) for pattern in filters[index * 2:index * 2 + 2]]
            populations = [set(), set()]
            sources = set((GATE.ROOT / module / "src").rglob("*.c"))
            for source in sources:
                owners = [position for position, (project, expression) in enumerate(
                    zip(module_projects, expressions)) if expression.match(
                        os.path.relpath(source, project.parent).replace(os.sep, "/"))]
                with self.subTest(module=module, source=source.name):
                    self.assertEqual(1, len(owners),
                                     f"Expected one report owner for {source}, got {owners}")
                for position in owners:
                    populations[position].add(source)
            self.assertTrue(all(populations), f"Empty report population for {module}")
            self.assertEqual(sources, populations[0] | populations[1])
            self.assertFalse(populations[0] & populations[1])

    def test_disjoint_codec_and_json_reports_cover_the_complete_common_population(self):
        module = "esy_common/protocol_codec"
        source_dir = GATE.ROOT / module / "src"
        sources = sorted(source_dir.rglob("*.c"))
        with tempfile.TemporaryDirectory() as directory:
            reports = [Path(directory) / name for name in ("codec.txt", "json.txt")]
            populations = (sources[::2], sources[1::2])
            self.assertTrue(all(populations))
            for report, population in zip(reports, populations):
                report.write_text("\n".join(
                    f"{path} 100 100 100%" for path in population) +
                    "\nTOTAL 100 100 100%\n")
            with contextlib.redirect_stdout(io.StringIO()):
                GATE.check(module, reports)
            with reports[1].open("a") as stream:
                stream.write(f"{sources[0]} 100 100 100%\n")
            with self.assertRaisesRegex(ValueError, "Duplicate"):
                GATE.check(module, reports)

    def test_split_reports_still_require_each_source_and_each_valid_total(self):
        module = "esy_common/protocol_codec"
        sources = sorted((GATE.ROOT / module / "src").rglob("*.c"))
        with tempfile.TemporaryDirectory() as directory:
            first, second = [Path(directory) / name for name in ("first.txt", "second.txt")]
            first.write_text("\n".join(f"{path} 100 100 100%" for path in sources[:-1]) +
                             "\nTOTAL 100 100 100%\n")
            second.write_text("TOTAL 100 100 100%\n")
            with self.assertRaisesRegex(ValueError, "Missing"):
                GATE.check(module, [first, second])
            second.write_text(f"{sources[-1]} 100 84 100%\nTOTAL 100 100 100%\n")
            with self.assertRaisesRegex(ValueError, "requires 85%"):
                GATE.check(module, [first, second])
            second.write_text(f"{sources[-1]} 100 100 100%\n")
            with self.assertRaisesRegex(ValueError, "invalid aggregate"):
                GATE.check(module, [first, second])

    def test_cli_joins_both_json_codec_and_channel_reports_without_omissions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reports = [root / name for name in ("common.txt", "middleware.txt",
                                                "json.txt", "json_tcp.txt")]
            for module, ordinary, json_report in (
                (GATE.MODULES[0], reports[0], reports[2]),
                (GATE.MODULES[1], reports[1], reports[3]),
            ):
                sources = sorted((GATE.ROOT / module / "src").rglob("*.c"))
                populations = ([path for path in sources if "json" not in path.name],
                               [path for path in sources if "json" in path.name])
                self.assertTrue(all(populations))
                for report, population in zip((ordinary, json_report), populations):
                    report.write_text("\n".join(f"{path} 100 100 100%" for path in population) +
                                      "\nTOTAL 100 100 100%\n")
            command = [sys.executable, "-B", str(Path(__file__).with_name("check_coverage.py")),
                       *(str(report) for report in reports)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            reports[3].write_text("TOTAL 100 100 100%\n")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("Missing esy_middleware/protocol coverage", result.stderr)

if __name__ == "__main__":
    unittest.main()
