# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Exercise source/commit/release policy and isolated test configuration semantics."""
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


class MaintenanceChecks(unittest.TestCase):
    def test_backend_api_closure_reaches_nested_xio_sources(self):
        with tempfile.TemporaryDirectory(prefix='nested backend closure ') as directory:
            root = Path(directory)
            checks = root / 'tools/checks'
            checks.mkdir(parents=True)
            checker = checks / 'check_esy_hal_backend_contracts.sh'
            shutil.copyfile(ROOT / 'tools/checks' / checker.name, checker)
            shutil.copytree(ROOT / 'esy_hal_backend', root / 'esy_hal_backend',
                            ignore=shutil.ignore_patterns('build'))
            source = root / 'esy_hal_backend/src/pico/xio/uart_rx/esy_hb_xio_uart_rx.c'
            source.write_text(source.read_text().replace(
                'esy_hb_xio_uart_rx_get_pin(', 'removed_uart_rx_get_pin('))
            result = subprocess.run(['bash', str(checker)], capture_output=True, text=True)
            self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertIn('pico/xio/uart_rx/esy_hb_xio_uart_rx.c misses public API',
                          result.stderr)
            self.assertIn('esy_hb_xio_uart_rx_get_pin', result.stderr)

    def allocation_fixture(self, root):
        for directory in ('esy_common', 'esy_hal', 'esy_hal_backend', 'esy_middleware'):
            (root / directory).mkdir()
        tool = root / 'tools/checks/check_no_dynamic_alloc.sh'
        tool.parent.mkdir(parents=True)
        for name in ('check_no_dynamic_alloc.sh', 'check_no_dynamic_alloc.py'):
            shutil.copyfile(ROOT / 'tools/checks' / name, tool.parent / name)
        return tool

    def run_allocation_check(self, tool, *paths, cwd=None):
        return subprocess.run(['bash', str(tool), *(str(path) for path in paths)],
                              cwd=cwd or tool.parents[2], capture_output=True, text=True)

    def test_allocation_defaults_cover_every_production_root(self):
        with tempfile.TemporaryDirectory(prefix='allocation roots ') as directory:
            root = Path(directory)
            tool = self.allocation_fixture(root)
            for relative in ('esy_common/defs/src/owned.c', 'esy_hal/src/owned.c',
                             'esy_hal_backend/src/pico/owned.c',
                             'esy_middleware/wifi/src/owned.h'):
                with self.subTest(path=relative):
                    source = root / relative
                    source.parent.mkdir(parents=True, exist_ok=True)
                    source.write_text('void *buffer = malloc(8);\n')
                    result = self.run_allocation_check(tool)
                    self.assertEqual(1, result.returncode, result.stderr)
                    self.assertIn(f'{relative}:1:', result.stdout)
                    source.write_text('int fixed_storage;\n')

    def test_allocation_references_include_aliases_addresses_and_spliced_tokens(self):
        with tempfile.TemporaryDirectory(prefix='allocation references ') as directory:
            root = Path(directory)
            tool = self.allocation_fixture(root)
            source = root / 'esy_common/owned.c'
            cases = ('void *buffer = malloc(8);\n', '#define ALLOC malloc\n',
                     'allocator = &calloc;\n', 'allocator = (realloc);\n',
                     'release = free;\n', 'allocator = aligned_alloc;\n',
                     '#define COPY strdup\n', '#define COPY strndup\n',
                     '#define ALLOC reallocarray\n', '#define ALLOC posix_memalign\n',
                     '#define PRINT asprintf\n', '#define PRINT vasprintf\n',
                     '#include <stdlib.h>\n', '#include "malloc.h"\n',
                     '#/**/include/**/<stdlib.h>\n')
            for content in cases:
                with self.subTest(content=content):
                    source.write_text(content)
                    result = self.run_allocation_check(tool)
                    self.assertEqual(1, result.returncode, result.stderr)
                    self.assertIn('esy_common/owned.c:1:', result.stdout)
            source.write_text('/* banner */\n#define ALLOC mal\\\nloc\n')
            result = self.run_allocation_check(tool)
            self.assertEqual(1, result.returncode, result.stderr)
            self.assertIn('esy_common/owned.c:2: allocator reference malloc', result.stdout)
            source.write_text('log("/*"); malloc(8);\n')
            self.assertEqual(1, self.run_allocation_check(tool).returncode)

    def test_allocation_ignores_comments_literals_and_only_existing_exemptions(self):
        with tempfile.TemporaryDirectory(prefix='allocation exclusions ') as directory:
            root = Path(directory)
            tool = self.allocation_fixture(root)
            source = root / 'esy_common/owned.c'
            source.write_text(
                '/* malloc(8); free(pointer); #include <stdlib.h> */\n'
                '// allocator = calloc;\\\nrealloc(0, 4);\n'
                'log("malloc(8) /* free */ \\"calloc\\" // strdup");\n'
                'const char *text = "#include <stdlib.h>";\n'
                "char quote = '\\''; char slash = '/';\n"
                'int not_malloc;\n')
            for relative in ('esy_common/tests/owned.c', 'esy_hal/build/owned.c',
                             'esy_middleware/esy_test/owned.h', 'esy_test/owned.c',
                             'esy_middleware/os/src/freertos/owned.c'):
                excluded = root / relative
                excluded.parent.mkdir(parents=True, exist_ok=True)
                # Invalid encoding proves excluded files are not read.
                excluded.write_bytes(b'malloc(8);\xff\n')
            result = self.run_allocation_check(tool)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertIn('1 production C/header files', result.stdout)
            self.assertIn('SDK/library internal allocation is not checked', result.stdout)
            other_os = root / 'esy_middleware/os_extra/owned.c'
            other_os.parent.mkdir()
            other_os.write_text('allocator = malloc;\n')
            self.assertEqual(1, self.run_allocation_check(tool).returncode)

    def test_allocation_custom_paths_preserve_cwd_and_root_resolution(self):
        with tempfile.TemporaryDirectory(prefix='allocation paths ') as directory:
            root = Path(directory)
            tool = self.allocation_fixture(root)
            source = root / 'custom directory/owned.c'
            source.parent.mkdir()
            source.write_text('#define ALLOC malloc\n')
            other = root / 'other cwd'
            other.mkdir()
            self.assertEqual(0, self.run_allocation_check(tool).returncode)
            for argument, cwd in ((source, other), ('owned.c', source.parent),
                                  ('custom directory', other)):
                with self.subTest(argument=argument, cwd=cwd):
                    result = self.run_allocation_check(tool, argument, cwd=cwd)
                    self.assertEqual(1, result.returncode, result.stderr)
                    self.assertIn('custom directory/owned.c:1:', result.stdout)
            result = self.run_allocation_check(tool, 'missing path', cwd=other)
            self.assertEqual(2, result.returncode)
            self.assertIn('does not exist', result.stderr)

    def capture_runner(self, root, tests, configuration, option=None, error=None,
                       ceedling_exit=0):
        (tests / 'project.yml').write_text(configuration)
        executable = root / 'ceedling'
        executable.write_text(
            '#!/usr/bin/env ruby\nrequire "yaml"\nrequire "json"\n'
            'c = YAML.unsafe_load_file(ARGV[1])\n'
            'g = c.dig(:gcov, :gcovr, :config_file)\n'
            'File.write(ENV.fetch("CAPTURE"), JSON.generate('
            '{config: c, cwd: Dir.pwd, gcov: g ? File.read(g) : nil, '
            'providers: ENV.to_h.select { |k, v| k.start_with?("ESY_") }}))\n'
            f'exit {ceedling_exit}\n')
        executable.chmod(0o755)
        capture = root / 'captured.json'
        arguments = ['ruby', str(ROOT / 'tools/quality/run_module_tests.rb'),
                     str(tests), str(root / 'output'), 'gcov:all']
        if option:
            arguments.append(option)
        result = subprocess.run(arguments,
                                env={**os.environ, 'PATH': str(root) + os.pathsep + os.environ['PATH'],
                                     'ESY_PROVIDER_CACHE_DIR': str(root / 'provider cache'),
                                     'CAPTURE': str(capture)},
                                capture_output=True, text=True)
        if error:
            self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertIn(error, result.stderr)
            self.assertFalse(capture.exists())
            return None
        self.assertEqual(1 if ceedling_exit else 0, result.returncode, result.stderr)
        return json.loads(capture.read_text())

    def test_runner_resolves_cached_provider_and_injects_only_the_child_environment(self):
        with tempfile.TemporaryDirectory(prefix='cached runner provider ') as directory:
            root = Path(directory)
            tests = root / 'tests'
            tests.mkdir()
            lookup = runpy.run_path(str(ROOT / 'tools/providers/provider_cache.py'))
            _, providers = lookup['load_lock']()
            provider = providers['coremqtt']
            source = lookup['managed_provider_directory'](root / 'provider cache', provider)
            for relative in provider['required_files']:
                path = source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('')
            configuration = ':project: {}\n:paths:\n  :source: ["@ESY_COREMQTT_PATH@/source"]\n'
            with mock.patch.dict(os.environ):
                os.environ.pop('ESY_COREMQTT_PATH', None)
                captured = self.capture_runner(root, tests, configuration)
                self.assertNotIn('ESY_COREMQTT_PATH', os.environ)
                self.assertEqual(str(source), captured['providers']['ESY_COREMQTT_PATH'])
                self.assertEqual([str(source / 'source')], captured['config']['paths']['source'])
                (root / 'captured.json').unlink()
                os.environ['ESY_COREMQTT_PATH'] = ''
                self.capture_runner(root, tests, configuration, error='must not be empty')

    def test_quality_gate_rejects_bad_task_before_cache_and_missing_cache_before_tests(self):
        with tempfile.TemporaryDirectory(prefix='unprepared quality gate ') as directory:
            environment = dict(os.environ, ESY_PROVIDER_CACHE_DIR=directory)
            lookup = runpy.run_path(str(ROOT / 'tools/providers/provider_cache.py'))
            _, providers = lookup['load_lock']()
            for provider in providers.values():
                environment.pop(provider['environment'], None)
            gate = ROOT / 'tools/quality/ci_quality_gate.sh'
            result = subprocess.run(['bash', str(gate), 'unsupported:task'], env=environment,
                                    capture_output=True, text=True)
            self.assertEqual(2, result.returncode, result.stdout + result.stderr)
            self.assertIn('usage:', result.stderr)
            result = subprocess.run(['bash', str(gate)], env=environment,
                                    capture_output=True, text=True)
            self.assertEqual(1, result.returncode, result.stdout + result.stderr)
            self.assertIn('run ./setup.sh', result.stderr)
            self.assertNotIn('[quality-gate]', result.stdout)

    def test_runner_resolves_only_the_explicit_mqtt_path_without_ruby_evaluation(self):
        with tempfile.TemporaryDirectory(prefix='MQTT provider path ') as directory:
            root = Path(directory)
            tests = root / 'tests'
            tests.mkdir()
            provider = str(root / 'upstream sources with spaces') + r'\1'
            configuration = (
                ':project: {}\n:paths:\n  :source: ["@ESY_COREMQTT_PATH@/source"]\n'
                ':files:\n  :support: ["@ESY_COREMQTT_PATH@/source/core_mqtt.c"]\n'
                ':custom:\n  :nested:\n    - :path: "@ESY_COREMQTT_PATH@/source/include"\n'
                '      :literal: \'#{raise "unexpected Ruby evaluation"}\'\n'
                '      :other: "@OTHER_PATH@/untouched"\n')
            with mock.patch.dict(os.environ, {'ESY_COREMQTT_PATH': provider}):
                captured = self.capture_runner(root, tests, configuration)['config']
            self.assertEqual([provider + '/source'], captured['paths']['source'])
            self.assertEqual([provider + '/source/core_mqtt.c'], captured['files']['support'])
            nested = captured['custom']['nested'][0]
            self.assertEqual(provider + '/source/include', nested['path'])
            self.assertEqual('#{raise "unexpected Ruby evaluation"}', nested['literal'])
            self.assertEqual('@OTHER_PATH@/untouched', nested['other'])

    def test_runner_rejects_missing_or_empty_mqtt_provider_before_launch(self):
        for value in (None, ''):
            with self.subTest(value=value), tempfile.TemporaryDirectory(prefix='missing MQTT provider ') as directory:
                root = Path(directory)
                tests = root / 'tests'
                tests.mkdir()
                with mock.patch.dict(os.environ):
                    os.environ.pop('ESY_COREMQTT_PATH', None)
                    if value is not None:
                        os.environ['ESY_COREMQTT_PATH'] = value
                    self.capture_runner(root, tests,
                        ':project: {}\n:paths:\n  :source: ["@ESY_COREMQTT_PATH@/source"]\n',
                        error='ESY_COREMQTT_PATH')

    def test_runner_resolves_json_provider_paths_as_literal_data(self):
        with tempfile.TemporaryDirectory(prefix='JSON provider path ') as directory:
            root = Path(directory)
            tests = root / 'tests'
            tests.mkdir()
            provider = str(root / 'coreJSON source with spaces') + r'\1'
            configuration = (
                ':project: {}\n:paths:\n'
                '  :include: ["@ESY_COREJSON_PATH@/source/include"]\n'
                ':files:\n  :support: ["@ESY_COREJSON_PATH@/source/core_json.c"]\n')
            with mock.patch.dict(os.environ, {'ESY_COREJSON_PATH': provider}):
                captured = self.capture_runner(root, tests, configuration)['config']
            self.assertEqual([provider + '/source/include'], captured['paths']['include'])
            self.assertEqual([provider + '/source/core_json.c'], captured['files']['support'])
            (root / 'captured.json').unlink()
            for value in (None, ''):
                with self.subTest(value=value), mock.patch.dict(os.environ):
                    os.environ.pop('ESY_COREJSON_PATH', None)
                    if value is not None:
                        os.environ['ESY_COREJSON_PATH'] = value
                    self.capture_runner(root, tests, configuration, error='ESY_COREJSON_PATH')

    def test_runner_resolves_mbedtls_path_without_interpreting_special_characters(self):
        with tempfile.TemporaryDirectory(prefix='Mbed TLS provider path ') as directory:
            root = Path(directory)
            tests = root / 'tests'
            tests.mkdir()
            provider = str(root / 'native sources with spaces') + r'\1'
            configuration = (
                ':project: {}\n:paths:\n'
                '  :include: ["@ESY_MBEDTLS_PATH@/include"]\n'
                ':files:\n  :support: ["@ESY_MBEDTLS_PATH@/library/aes.c"]\n')
            with mock.patch.dict(os.environ, {'ESY_MBEDTLS_PATH': provider}):
                captured = self.capture_runner(root, tests, configuration)['config']
            self.assertEqual([provider + '/include'], captured['paths']['include'])
            self.assertEqual([provider + '/library/aes.c'], captured['files']['support'])

    def test_runner_rejects_missing_or_empty_mbedtls_provider_before_launch(self):
        for value in (None, ''):
            with self.subTest(value=value), tempfile.TemporaryDirectory(prefix='missing TLS provider ') as directory:
                root = Path(directory)
                tests = root / 'tests'
                tests.mkdir()
                with mock.patch.dict(os.environ):
                    os.environ.pop('ESY_MBEDTLS_PATH', None)
                    if value is not None:
                        os.environ['ESY_MBEDTLS_PATH'] = value
                    self.capture_runner(root, tests,
                        ':project: {}\n:paths:\n  :include: ["@ESY_MBEDTLS_PATH@/include"]\n',
                        error='ESY_MBEDTLS_PATH')

    def test_runner_mixin_merges_defines_without_replacing_artifact_directory(self):
        with tempfile.TemporaryDirectory(prefix='isolated mixin ') as directory:
            root = Path(directory)
            tests = root / 'tests'
            tests.mkdir()
            (tests / 'tick_variant.yml').write_text(
                ':project:\n  :build_root: build/unsafe-shared\n'
                ':defines:\n  :test: [TEST, TEST_TICK_TYPE=uint16_t]\n')
            captured = self.capture_runner(root, tests,
                ':project: {}\n:defines:\n  :test: [TEST, BASELINE]\n',
                '--mixin=tick_variant.yml')
            self.assertEqual(str(root / 'output'), captured['config']['project']['build_root'])
            self.assertEqual(['TEST', 'BASELINE', 'TEST_TICK_TYPE=uint16_t'],
                             captured['config']['defines']['test'])

    def test_runner_preserves_modifiers_filters_and_suite_working_directory(self):
        with tempfile.TemporaryDirectory(prefix='isolated runner ') as directory:
            root = Path(directory)
            tests = root / 'nested/tests'
            tests.mkdir(parents=True)
            result = self.capture_runner(root, tests,
                ':project: {}\n:paths:\n  :source: ["../../src"]\n'
                ':files:\n  :source: ["-:../../src/excluded.c", "+:../../src/added.c"]\n'
                ':gcov:\n  :gcovr:\n    :report_root: "../../.."\n'
                '    :report_include: "../../src/owned[.]c$"\n')
            config = result['config']
            self.assertEqual(str(tests), result['cwd'])
            self.assertEqual(['../../src'], config['paths']['source'])
            self.assertEqual(['-:../../src/excluded.c', '+:../../src/added.c'],
                             config['files']['source'])
            self.assertEqual('../../..', config['gcov']['gcovr']['report_root'])
            self.assertEqual('../../src/owned[.]c$', config['gcov']['gcovr']['report_include'])
            self.assertNotIn('config_file', config['gcov']['gcovr'])
            self.assertIsNone(result['gcov'])
            self.assertEqual(str(tests), config['gcov']['gcovr']['object_directory'])
            self.assertNotIn('tools', config)
            self.assertEqual([f'"{root / "output/gcov"}"'],
                             config['tools_gcov_gcovr_report']['arguments'])

    def test_runner_preserves_custom_gcovr_tool_and_propagates_failure(self):
        with tempfile.TemporaryDirectory(prefix='custom coverage tool ') as directory:
            root = Path(directory)
            tests = root / 'tests'
            tests.mkdir()
            result = self.capture_runner(root, tests,
                ':project: {}\n:gcov:\n  :gcovr:\n    :fail_under_line: 91\n'
                ':tools_gcov_gcovr_report:\n  :arguments: ["--sort-percentage"]\n'
                ':tools:\n  :gcov_gcovr_report:\n    :executable: custom-gcovr --provider-flag\n'
                '    :arguments: ["--strict", "${1}"]\n', ceedling_exit=7)
            tool = result['config']['tools']['gcov_gcovr_report']
            self.assertEqual('custom-gcovr --provider-flag', tool['executable'])
            self.assertEqual(['--strict', '${1}'], tool['arguments'])
            self.assertEqual(91, result['config']['gcov']['gcovr']['fail_under_line'])
            self.assertTrue(result['config']['gcov']['gcovr']['exception_on_fail'])
            self.assertEqual(['--sort-percentage', f'"{root / "output/gcov"}"'],
                             result['config']['tools_gcov_gcovr_report']['arguments'])

    @unittest.skipUnless(shutil.which('gcovr') and shutil.which('gcc'),
                         'gcc and gcovr are required for real coverage integration')
    def test_runner_real_ceedling_118_coverage_preserves_filter_and_output_scope(self):
        available = subprocess.run(['ruby', '-e', "gem 'ceedling', '1.1.8'"],
                                   capture_output=True, text=True)
        self.assertEqual(0, available.returncode,
                         'Ceedling 1.1.8 is required for real coverage integration: ' + available.stderr)
        with tempfile.TemporaryDirectory(prefix='real-coverage-runner-') as directory:
            root = Path(directory)
            tests = root / 'tests'
            source = root / 'src'
            binaries = root / 'bin'
            for path in (tests, source, binaries):
                path.mkdir()
            ceedling = binaries / 'ceedling'
            ceedling.write_text(
                '#!/usr/bin/env ruby\n'
                "gem 'ceedling', '1.1.8'\n"
                "load Gem.bin_path('ceedling', 'ceedling', '1.1.8')\n")
            ceedling.chmod(0o755)
            for name in ('owned', 'foreign'):
                (source / f'{name}.h').write_text(f'int {name}(void);\n')
                (source / f'{name}.c').write_text(
                    f'#include "{name}.h"\nint {name}(void) {{ return 42; }}\n')
            (tests / 'test_owned.c').write_text(
                '#include "unity.h"\n#include "owned.h"\n#include "foreign.h"\n'
                'void setUp(void) {}\nvoid tearDown(void) {}\n'
                'void test_sources_run(void) {\n'
                '  TEST_ASSERT_EQUAL(42, owned());\n'
                '  TEST_ASSERT_EQUAL(42, foreign());\n}\n')
            (tests / 'project.yml').write_text(
                ':project:\n  :which_ceedling: gem\n  :ceedling_version: 1.1.8\n'
                '  :use_mocks: FALSE\n  :use_test_preprocessor: :all\n'
                '  :build_root: build\n'
                ':paths:\n  :test: [.]\n  :source: [../src]\n  :include: [../src]\n'
                ':plugins:\n  :enabled: [gcov]\n'
                ':gcov:\n  :reports: [Json]\n  :gcovr:\n'
                f'    :report_root: {json.dumps(str(root))}\n'
                '    :report_include: ".*/owned[.]c$"\n'
                '    :fail_under_line: 100\n'
                '    :json_artifact_filename: owned.json\n')
            output = root / 'isolated-output'
            result = subprocess.run(
                ['ruby', str(ROOT / 'tools/quality/run_module_tests.rb'),
                 str(tests), str(output), 'gcov:all'],
                env={**os.environ, 'PATH': str(binaries) + os.pathsep + os.environ['PATH']},
                capture_output=True, text=True, timeout=60)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            report = output / 'artifacts/gcov/gcovr/owned.json'
            files = json.loads(report.read_text())['files']
            self.assertEqual(['src/owned.c'], [entry['file'] for entry in files])
            self.assertTrue(all(line['count'] > 0 for line in files[0]['lines']))
            self.assertFalse((tests / 'build').exists())

    def test_runner_preserves_config_relative_filters_and_thresholds(self):
        with tempfile.TemporaryDirectory(prefix='gcov config runner ') as directory:
            root = Path(directory)
            tests = root / 'tests'
            original = tests / 'coverage/gcovr.cfg'
            original.parent.mkdir(parents=True)
            contents = ('filter = ../../src/.*\nexclude = .*generated\n'
                        'search-path = obsolete\nfail-under-line = 91\n'
                        'object-directory = ..\n')
            original.write_text(contents)
            result = self.capture_runner(root, tests,
                ':project: {}\n:gcov:\n  :gcovr:\n'
                '    :config_file: coverage/gcovr.cfg\n    :report_root: "../.."\n')
            temporary = Path(result['config']['gcov']['gcovr']['config_file'])
            self.assertEqual(original.parent, temporary.parent)
            self.assertFalse(temporary.exists())
            self.assertEqual(contents, original.read_text())
            self.assertNotIn('search-path = obsolete', result['gcov'])
            for line in contents.splitlines():
                if not line.startswith('search-path'):
                    self.assertIn(line, result['gcov'])
            self.assertIn(f'search-path = {root / "output/gcov"}', result['gcov'])

    def test_commit_subject_exception_and_allowed_types(self):
        cases = [('Initial commit', 0), ('Initial commit: unexpected suffix', 1),
                 ('feature: unsupported type', 1), ('feature(uart): unsupported type', 1),
                 ('fix: ', 1), ('fix(uart): ', 1), ('fix(uart):missing space', 1),
                 ('fix: correct timing\nextra line', 1), ('fix: correct timing\r', 1),
                 ('fix: correct\ttiming', 1), ('fix: \u4fee\u590d', 1)]
        for commit_type in ('feat', 'fix', 'docs', 'style', 'refactor', 'test', 'chore', 'ci'):
            cases.extend([(f'{commit_type}: correct timing', 0),
                          (f'{commit_type}(uart): correct timing', 0)])
        cases.extend((f'fix({scope}): correct timing', 0)
                     for scope in ('os', 'cmake', 'deps', 'uart-rx', '2wire'))
        cases.extend((f'fix({scope}): correct timing', 1)
                     for scope in ('', 'UART', 'uart rx', 'uart_rx', 'hal/uart',
                                   '-uart', '(uart)', 'uart)(os', '\u4e32\u53e3'))
        for subject, expected in cases:
            with self.subTest(subject=subject):
                result = subprocess.run(['bash', str(ROOT / 'tools/checks/check_commit_message.sh'),
                                         '--message', subject], capture_output=True, text=True)
                self.assertEqual(expected, result.returncode, result.stderr)

    def test_release_heading_is_a_literal_complete_version(self):
        with tempfile.TemporaryDirectory(prefix='release heading ') as directory:
            root = Path(directory)
            tool = root / 'tools/checks/check_release_readiness.sh'
            tool.parent.mkdir(parents=True)
            shutil.copyfile(ROOT / 'tools/checks/check_release_readiness.sh', tool)
            checklist = root / 'docs/release/release_tag_checklist.md'
            checklist.parent.mkdir(parents=True)
            checklist.write_text('Release checklist\n')
            for name in ('tools/quality/generate_changelog.sh', 'tools/checks/check_gcov_thresholds.sh'):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#!/bin/sh\nexit 0\n')
                path.chmod(0o755)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            subprocess.run(['git', '-C', str(root), '-c', 'user.name=Test', '-c',
                            'user.email=test@example.invalid', 'commit', '-q', '--allow-empty',
                            '-m', 'Initial commit'], check=True)
            subprocess.run(['git', '-C', str(root), 'tag', 'v9.8.7'], check=True)
            for heading, expected in [('## 9.8.7', 0), ('## [v9.8.7] - 2026-10-18', 0),
                                      ('## 9.8.70', 1), ('## 9x8x7', 1),
                                      ('## 9.8.7-rc1', 1), ('### 9.8.7', 1),
                                      ('    ## 9.8.7', 1)]:
                with self.subTest(heading=heading):
                    (root / 'CHANGELOG.md').write_text(heading + '\n')
                    result = subprocess.run(['bash', str(tool), 'v9.8.7'],
                                            capture_output=True, text=True)
                    self.assertEqual(expected, result.returncode, result.stderr)

    def test_sanitizer_runner_preserves_base_and_mixin_defines(self):
        with tempfile.TemporaryDirectory(prefix='sanitizer runner ') as directory:
            root = Path(directory)
            tests = root / 'tests'
            tests.mkdir()
            (tests / 'project.yml').write_text(
                ':project: {}\n:defines:\n  :test:\n    - BASE_CONTRACT\n')
            (tests / 'sanitizer.yml').write_text(
                ':flags:\n  :test:\n    :compile:\n      :*:\n        - -fsanitize=address\n'
                ':defines:\n  :test:\n    - ESY_PROTOCOL_SANITIZER\n')
            executable = root / 'ceedling'
            executable.write_text('#!/bin/sh\ncp "$2" "$CAPTURE"\n')
            executable.chmod(0o755)
            capture = root / 'captured.yml'
            result = subprocess.run(['ruby', str(ROOT / 'tools/quality/run_module_tests.rb'),
                                     str(tests), str(root / 'output'), 'test:all', '--sanitizer'],
                                    env={**os.environ, 'PATH': str(root) + os.pathsep + os.environ['PATH'],
                                         'CAPTURE': str(capture)}, capture_output=True, text=True)
            self.assertEqual(0, result.returncode, result.stderr)
            config = capture.read_text()
            self.assertIn('BASE_CONTRACT', config)
            self.assertIn('ESY_PROTOCOL_SANITIZER', config)
            self.assertIn('-fsanitize=address', config)


if __name__ == '__main__':
    unittest.main()
