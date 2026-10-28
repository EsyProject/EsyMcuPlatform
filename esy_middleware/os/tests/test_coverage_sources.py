# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Regression tests for complete JSON OS coverage population."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    'os_coverage', Path(__file__).with_name('check_coverage_sources.py'))
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class OSCoverageSources(unittest.TestCase):
    def sources(self):
        return [str(p) for p in sorted(GATE.SOURCE.glob('*.c'))] + [
            str(GATE.SOURCE / 'port/esp32/esy_os_context.c')]

    def check(self, sources, fail=False):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'coverage.json'
            path.write_text(json.dumps({'files': [{'file': p} for p in sources]}))
            if fail:
                with self.assertRaises(ValueError):
                    GATE.check(path)
            else:
                with contextlib.redirect_stdout(io.StringIO()):
                    GATE.check(path)

    def test_all_wrapper_and_context_sources_are_required(self):
        self.check(self.sources())
        self.check(self.sources()[:-1], fail=True)
        self.check([], fail=True)

    def test_explicit_path_roots_are_supported(self):
        sources = self.sources()
        for root in (GATE.ROOT, GATE.TESTS.parent):
            self.check([str(Path(path).relative_to(root)) for path in sources])
        self.check(['../' + str(Path(path).relative_to(GATE.TESTS.parent))
                    for path in sources])
        self.check([path.replace('/', '\\') for path in sources])

    def test_foreign_namesakes_and_basenames_cannot_replace_local_sources(self):
        name = Path(self.sources()[0]).name
        for path in (f'/tmp/foreign/esy_middleware/os/src/freertos/{name}',
                     f'foreign/src/freertos/{name}', name):
            with self.subTest(path=path):
                self.check([path] + self.sources()[1:], fail=True)

    def test_internal_headers_must_also_be_local_and_unique(self):
        header = next(GATE.SOURCE.rglob('*.h'))
        self.check(self.sources() + [str(header)])
        for extra in (f'/tmp/foreign/src/freertos/{header.name}',
                      'src/freertos/fixture.h'):
            with self.subTest(extra=extra):
                self.check(self.sources() + [extra], fail=True)
        self.check(self.sources() + [str(header),
                   f'src/freertos/../freertos/{header.name}'], fail=True)

    def test_duplicate_and_fixture_sources_are_rejected(self):
        self.check(self.sources() + [self.sources()[0]], fail=True)
        self.check(self.sources() + [f'src/freertos/../freertos/{Path(self.sources()[0]).name}'],
                   fail=True)
        self.check(self.sources() + [f'../src/freertos/{Path(self.sources()[0]).name}'], fail=True)
        self.check(self.sources() + ['tests/native_test_support.c'], fail=True)


if __name__ == '__main__':
    unittest.main()
