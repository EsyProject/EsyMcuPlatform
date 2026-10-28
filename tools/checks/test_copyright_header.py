#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Exercise accepted SPDX notices and reject invalid ownership or licenses."""

from pathlib import Path
import subprocess
import tempfile
import unittest


CHECK = Path(__file__).with_name("check_copyright_header.sh")
HEADER = """/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
"""


class CopyrightHeaderTests(unittest.TestCase):
    def inspect(self, files, scan_path="."):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, contents in files.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(contents.encode("utf-8"))
            return subprocess.run(
                [str(CHECK), str(root / scan_path)],
                capture_output=True,
                text=True,
                check=False,
            )

    def assertAccepted(self, files, scan_path="."):
        result = self.inspect(files, scan_path)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def assertRejected(self, files, scan_path="."):
        result = self.inspect(files, scan_path)
        self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("[copyright] error:", result.stderr)

    def test_source_and_header_accept_spdx_notice_at_first_line(self):
        self.assertAccepted({
            "src/sample.c": HEADER + "int sample(void) { return 0; }\n",
            "include/sample.h": HEADER + "int sample(void);\n",
        })

    def test_original_years_and_increasing_ranges_are_preserved(self):
        for years in ("2019", "2019-2026", "2026-2026"):
            with self.subTest(years=years):
                self.assertAccepted({"sample.c": HEADER.replace("2026", years)})

    def test_windows_line_endings_are_accepted(self):
        self.assertAccepted({"sample.c": HEADER.replace("\n", "\r\n")})

    def test_independent_authors_can_follow_the_project_notice(self):
        self.assertAccepted({"sample.c": HEADER.replace(
            " *\n", " * SPDX-FileCopyrightText: 2018-2026 Original Author\n"
            " * SPDX-FileCopyrightText: 2024 Another Author <author@example.com>\n *\n",
        )})

    def test_additional_copyright_lines_require_a_year_and_author(self):
        for notice in ("", "2026", "2026 ", "2026    ", "Original Author", "2026-2018 Author"):
            with self.subTest(notice=notice):
                self.assertRejected({"sample.c": HEADER.replace(
                    " *\n", f" * SPDX-FileCopyrightText: {notice}\n *\n",
                )})

    def test_missing_or_invalid_years_are_rejected(self):
        for years in ("", "26", "0000", "2026-2019", "2019-26", "2019/2026"):
            with self.subTest(years=years):
                self.assertRejected({"sample.c": HEADER.replace("2026", years)})

    def test_wrong_owner_or_license_is_rejected(self):
        for notice in (
            HEADER.replace("EsyProject contributors", "AnotherOwner"),
            HEADER.replace("Apache-2.0", "MIT"),
            HEADER.replace("SPDX-License-Identifier:", "License:"),
        ):
            with self.subTest(notice=notice):
                self.assertRejected({"sample.h": notice})

    def test_old_proprietary_notice_is_rejected(self):
        self.assertRejected({"sample.c": """/*
 * Copyright (c) 2026 AnotherOwner. All rights reserved.
 *
 * Proprietary and confidential.
 * Unauthorized copying is strictly prohibited.
 */
"""})

    def test_missing_or_partial_notice_is_rejected(self):
        for contents in ("", "int sample(void);\n", "/*\n", HEADER.rsplit(" */", 1)[0]):
            with self.subTest(contents=contents):
                self.assertRejected({"sample.c": contents})

    def test_an_independent_author_does_not_replace_the_project_notice(self):
        self.assertRejected({"sample.c": HEADER.replace("EsyProject contributors", "Original Author")})

    def test_notice_must_start_at_first_line(self):
        for prefix in ("\n", "\ufeff", " ", "#include <stdint.h>\n"):
            with self.subTest(prefix=prefix):
                self.assertRejected({"sample.c": prefix + HEADER})

    def test_nested_headers_and_paths_with_spaces_are_checked(self):
        self.assertRejected({
            "valid.c": HEADER,
            "nested path/sample.h": HEADER.replace("Apache-2.0", "MIT"),
        })

    def test_generated_build_sources_and_other_file_types_are_not_scanned(self):
        self.assertAccepted({
            "src/sample.c": HEADER,
            "nested/build/generated.c": "/* Generated source. */\n",
            "notes.txt": "License notes.\n",
        })

    def test_explicit_source_file_is_supported(self):
        self.assertAccepted({"sample.c": HEADER}, scan_path="sample.c")

    def test_missing_scan_path_is_rejected(self):
        self.assertRejected({}, scan_path="missing")


if __name__ == "__main__":
    unittest.main()
