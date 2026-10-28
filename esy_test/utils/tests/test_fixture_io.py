# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Compile the fixture helper as strict C11 and exercise complete input files."""

from pathlib import Path
import subprocess
import tempfile
import unittest


class FixtureIoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="esy-fixture-test-")
        cls.directory = Path(cls.temporary.name)
        source = Path(__file__).resolve().parents[1]
        driver = cls.directory / "driver.c"
        driver.write_text('''#include "esy_fixture_io.h"
#include <stdio.h>
int main(int argc, char **argv) {
    uint16_t values[4] = {0};
    size_t count = 0u;
    if (argc != 2 || !esy_fixture_read_u16_csv(argv[1], values, 4u, &count)) return 1;
    printf("%zu", count);
    for (size_t i = 0u; i < count; ++i) printf(" %u", (unsigned)values[i]);
    return 0;
}
''')
        cls.executable = cls.directory / "reader"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-pedantic", "-I", str(source), str(driver),
                        str(source / "esy_fixture_io.c"), "-o", str(cls.executable)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def read(self, text):
        path = self.directory / "input.csv"
        path.write_text(text)
        return subprocess.run([str(self.executable), str(path)], check=True,
                              capture_output=True, text=True).stdout

    def test_spaces_comments_and_eof_do_not_lose_or_repeat_samples(self):
        self.assertEqual("3 1 2 3", self.read("1 2 3\n"))
        self.assertEqual("2 1 2", self.read("1,2\n# trailing comment\n"))
        self.assertEqual("3 1 2 3", self.read("# title\n1,# note\n2\t3"))
        self.assertEqual("0", self.read("# only comments\n" * 10000))
        self.assertEqual("0", self.read(""))

    def test_headers_invalid_tokens_clamping_and_capacity_are_explicit(self):
        self.assertEqual("4 0 65535 7 8", self.read(
            "samples,value\n-1,9999999999999999999999999999999,7,8,9"))
        self.assertEqual("2 12 3", self.read("12oops,12,+3,word"))


if __name__ == "__main__":
    unittest.main()
