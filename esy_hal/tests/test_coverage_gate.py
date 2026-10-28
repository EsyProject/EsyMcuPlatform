# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Verify that incomplete or uneven platform coverage cannot pass the gate."""
import contextlib
import copy
import io
import json
from pathlib import Path
import tempfile
import unittest
from check_coverage import ROOT, SOURCE, check, expected_sources


class CoverageGate(unittest.TestCase):
    def run_gate(self, entries):
        with tempfile.TemporaryDirectory() as temp:
            report = Path(temp) / 'coverage.json'
            report.write_text(json.dumps({'files': entries}))
            with contextlib.redirect_stdout(io.StringIO()):
                check(report)

    def entries(self):
        return [{'file': 'src/' + name, 'lines': [{'count': 1} for _ in range(100)]}
                for name in sorted(expected_sources())]

    def test_complete_coverage_passes(self):
        self.run_gate(self.entries())

    def test_explicit_path_roots_are_supported(self):
        for root in (ROOT, SOURCE.parent, SOURCE.parent / 'tests'):
            entries = self.entries()
            for entry, name in zip(entries, sorted(expected_sources())):
                if root == SOURCE.parent / 'tests':
                    entry['file'] = '../src/' + name
                else:
                    entry['file'] = str((SOURCE / name).relative_to(root))
            with self.subTest(root=root):
                self.run_gate(entries)
        entries = self.entries()
        for entry, name in zip(entries, sorted(expected_sources())):
            entry['file'] = str(SOURCE / name)
        self.run_gate(entries)
        for entry in entries:
            entry['file'] = entry['file'].replace('/', '\\')
        self.run_gate(entries)

    def test_foreign_namesakes_and_basenames_cannot_replace_local_sources(self):
        name = sorted(expected_sources())[0]
        for path in (f'/tmp/foreign/esy_hal/src/{name}',
                     f'foreign/esy_hal/src/{name}', Path(name).name):
            entries = self.entries()
            entries[0]['file'] = path
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, 'Foreign'):
                self.run_gate(entries)

    def test_duplicate_aliases_and_foreign_extra_rows_fail(self):
        entries = self.entries()
        name = sorted(expected_sources())[0]
        for path, error in ((entries[0]['file'], 'Duplicate'),
                            (f'src/../src/{name}', 'Duplicate'),
                            (str(SOURCE / name), 'Duplicate'),
                            ('tests/fixture.c', 'Foreign'),
                            ('tests/fixture.h', 'Foreign')):
            duplicate = copy.deepcopy(entries[0])
            duplicate['file'] = path
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, error):
                self.run_gate(entries + [duplicate])

    def test_symlink_alias_is_the_same_source(self):
        entries = self.entries()
        with tempfile.TemporaryDirectory() as directory:
            alias = Path(directory) / 'alias.c'
            alias.symlink_to(SOURCE / sorted(expected_sources())[0])
            duplicate = copy.deepcopy(entries[0])
            duplicate['file'] = str(alias)
            with self.assertRaisesRegex(ValueError, 'Duplicate'):
                self.run_gate(entries + [duplicate])

    def test_missing_sources_fail(self):
        with self.assertRaises(ValueError):
            self.run_gate(self.entries()[1:])

    def test_empty_population_and_empty_executable_source_fail(self):
        with self.assertRaisesRegex(ValueError, 'Missing'):
            self.run_gate([])
        for lines in ([], [{'count': 1, 'gcovr/noncode': True}]):
            entries = self.entries()
            entries[0]['lines'] = lines
            with self.subTest(lines=lines), self.assertRaisesRegex(ValueError, '65%'):
                self.run_gate(entries)

    def test_weak_file_cannot_hide_in_aggregate(self):
        entries = self.entries()
        for line in entries[0]['lines'][:36]:
            line['count'] = 0
        with self.assertRaises(ValueError):
            self.run_gate(entries)

    def test_low_aggregate_fails(self):
        entries = self.entries()
        for entry in entries:
            for line in entry['lines'][:16]:
                line['count'] = 0
        with self.assertRaises(ValueError):
            self.run_gate(entries)

    def test_existing_per_file_and_aggregate_boundaries_are_preserved(self):
        entries = self.entries()
        for line in entries[0]['lines'][:35]:
            line['count'] = 0
        self.run_gate(entries)
        entries = self.entries()
        for entry in entries:
            for line in entry['lines'][:15]:
                line['count'] = 0
            entry['lines'].append({'count': 0, 'gcovr/noncode': True})
        self.run_gate(entries)


if __name__ == '__main__':
    unittest.main()
