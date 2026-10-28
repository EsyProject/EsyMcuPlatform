# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Regression tests for complete, nonempty module coverage evidence."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("check_gcov_thresholds.sh")


class CoverageThresholdTests(unittest.TestCase):
    def check_reports(self, reports, expected, common_reports=None,
                      json_report="TOTAL 100 100 100%\n"):
        with tempfile.TemporaryDirectory(prefix="coverage evidence ") as d:
            root = Path(d)
            tool = root / "tools/checks/check_gcov_thresholds.sh"
            tool.parent.mkdir(parents=True)
            shutil.copyfile(SCRIPT, tool)
            for directory in ("esy_common", "esy_hal", "esy_middleware"):
                (root / directory).mkdir(parents=True)
            all_reports = {"esy_middleware/" + module: report for module, report in reports.items()}
            for module in ("ds", "utils"):
                all_reports["esy_common/" + module] = (
                    common_reports.get(module) if common_reports is not None
                    else "TOTAL 100 100 100%\n")
            all_reports["esy_common/protocol_codec/json"] = json_report
            projects = [(root / module / "tests/project.yml", module) for module in
                        ("esy_middleware/first", "esy_middleware/second", "esy_common/ds", "esy_common/utils")]
            projects.append((root / "esy_common/protocol_codec/tests/json/project.yml",
                             "esy_common/protocol_codec/json"))
            for project, module in projects:
                project.parent.mkdir(parents=True)
                project.write_text(":project: {}\n")
                if all_reports.get(module) is not None:
                    report = root / "reports" / module / "artifacts/gcov/gcovr/coverage.txt"
                    report.parent.mkdir(parents=True)
                    report.write_text(all_reports[module])
            generated_project = root / "esy_common/ds/tests/build/generated/tests/project.yml"
            generated_project.parent.mkdir(parents=True)
            generated_project.write_text(":project: {}\n")
            env = {**os.environ, "ESY_GCOV_REPORT_ROOT": str(root / "reports"), "ESY_GCOV_LINE_MIN": "85"}
            result = subprocess.run(["bash", str(tool)], env=env, capture_output=True, text=True)
            self.assertEqual(expected, result.returncode, result.stdout + result.stderr)
            return result.stdout + result.stderr

    def test_partial_report_set_fails(self):
        text = self.check_reports({"first": "TOTAL 100 100 100%\n"}, 1)
        self.assertIn("missing coverage report", text)

    def test_empty_report_fails(self):
        text = self.check_reports({"first": "TOTAL 0 0 100%\n", "second": "TOTAL 100 100 100%\n"}, 1)
        self.assertIn("empty coverage report", text)

    def test_complete_reports_pass(self):
        self.check_reports({"first": "TOTAL 100 85 85%\n", "second": "TOTAL 100 100 100%\n"}, 0)

    def test_below_threshold_fails(self):
        text = self.check_reports({"first": "TOTAL 100 84 84%\n", "second": "TOTAL 100 100 100%\n"}, 1)
        self.assertIn("below 85%", text)

    def test_missing_total_fails(self):
        self.check_reports({"first": "broken report\n", "second": "TOTAL 100 100 100%\n"}, 1)

    def test_common_data_structures_and_utils_reports_are_required(self):
        complete = {"first": "TOTAL 100 100 100%\n", "second": "TOTAL 100 100 100%\n"}
        for missing in ("ds", "utils"):
            with self.subTest(module=missing):
                common = {"ds": "TOTAL 100 100 100%\n", "utils": "TOTAL 100 100 100%\n"}
                common[missing] = None
                text = self.check_reports(complete, 1, common)
                self.assertIn(f"esy_common/{missing}/tests/project.yml: missing coverage report", text)

    def test_common_data_structures_and_utils_cannot_bypass_threshold(self):
        complete = {"first": "TOTAL 100 100 100%\n", "second": "TOTAL 100 100 100%\n"}
        for low in ("ds", "utils"):
            with self.subTest(module=low):
                common = {"ds": "TOTAL 100 100 100%\n", "utils": "TOTAL 100 100 100%\n"}
                common[low] = "TOTAL 100 84 84%\n"
                text = self.check_reports(complete, 1, common)
                self.assertIn(f"reports/esy_common/{low}/artifacts/gcov/gcovr/coverage.txt: line coverage 84% is below 85%", text)

    def test_nested_json_suite_requires_its_own_complete_coverage_report(self):
        complete = {"first": "TOTAL 100 100 100%\n", "second": "TOTAL 100 100 100%\n"}
        text = self.check_reports(complete, 1, json_report=None)
        self.assertIn("esy_common/protocol_codec/tests/json/project.yml: missing coverage report", text)
        text = self.check_reports(complete, 1, json_report="TOTAL 100 84 84%\n")
        self.assertIn("reports/esy_common/protocol_codec/json/artifacts/gcov/gcovr/coverage.txt: line coverage 84% is below 85%", text)


if __name__ == "__main__":
    unittest.main()
