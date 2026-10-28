# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Exercise explicit provider paths without weakening generic header checks."""

import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class PublicHeaderCompatibilityTests(unittest.TestCase):
    def fixture(self, root):
        tool = root / 'tools/checks/check_public_header_compatibility.sh'
        tool.parent.mkdir(parents=True)
        shutil.copyfile(ROOT / 'tools/checks' / tool.name, tool)
        providers = root / 'tools/providers'
        providers.mkdir()
        for name in ('provider_cache.py', 'providers.lock.json'):
            shutil.copyfile(ROOT / 'tools/providers' / name, providers / name)
        include = root / 'module/include'
        include.mkdir(parents=True)
        return tool, include

    def run_check(self, tool, path, provider=None, providers=None):
        environment = os.environ.copy()
        environment['ESY_PROVIDER_CACHE_DIR'] = str(tool.parents[2] / 'provider cache')
        for name in ('ESY_MBEDTLS_PATH', 'ESY_LITTLEFS_PATH', 'ESY_FATFS_PATH'):
            environment.pop(name, None)
        if provider is not None:
            environment['ESY_MBEDTLS_PATH'] = str(provider)
        environment.update({name: str(value) for name, value in (providers or {}).items()})
        return subprocess.run(['bash', str(tool), str(path)],
                              env=environment, text=True, capture_output=True,
                              check=False)

    def provider_fixture(self, root):
        provider = root / 'explicit provider with spaces'
        version = provider / 'include/mbedtls/version.h'
        version.parent.mkdir(parents=True)
        version.write_text('#define MBEDTLS_VERSION_NUMBER 0x03060700\n')
        return provider

    def filesystem_provider_fixtures(self, root):
        littlefs = root / 'littlefs provider with spaces'
        littlefs.mkdir()
        (littlefs / 'lfs.h').write_text('#include "lfs_util.h"\nstruct littlefs_value { int value; };\n')
        (littlefs / 'lfs_util.h').write_text('#define TEST_LITTLEFS_PROVIDER 1\n')
        fatfs = root / 'fatfs provider with spaces'
        fatfs.mkdir()
        (fatfs / 'ff.h').write_text(
            '#include "ffconf.h"\n#ifndef TEST_PLATFORM_FATFS_CONFIG\n'
            '#error "Platform configuration missing"\n#endif\n')
        (fatfs / 'ffconf.h').write_text('#error "Original provider configuration must not be used"\n')
        config = root / 'esy_middleware/fs/config/fatfs/ffconf.h'
        config.parent.mkdir(parents=True)
        config.write_text('#define TEST_PLATFORM_FATFS_CONFIG 1\n')
        return {'ESY_LITTLEFS_PATH': littlefs, 'ESY_FATFS_PATH': fatfs}

    def test_generic_headers_need_no_provider_and_ignore_unneeded_bad_path(self):
        with tempfile.TemporaryDirectory(prefix='generic headers ') as directory:
            tool, include = self.fixture(Path(directory))
            header = include / 'generic.h'
            header.write_text('#include <stddef.h>\nstruct value { size_t n; };\n')
            for provider in (None, '', '/missing provider'):
                with self.subTest(provider=provider):
                    result = self.run_check(tool, header, provider)
                    self.assertEqual(0, result.returncode, result.stderr)
                    self.assertIn('checking C++', result.stdout)

    def test_adapter_headers_require_an_explicit_nonempty_provider(self):
        with tempfile.TemporaryDirectory(prefix='missing adapter provider ') as directory:
            tool, include = self.fixture(Path(directory))
            header = include / 'adapter_mbedtls.h'
            header.write_text('#include "mbedtls/version.h"\n')
            for provider in (None, ''):
                with self.subTest(provider=provider):
                    result = self.run_check(tool, include, provider)
                    self.assertEqual(2, result.returncode, result.stderr)
                    self.assertIn('ESY_MBEDTLS_PATH is required', result.stderr)
                    self.assertIn('*_mbedtls.h', result.stderr)

    def test_adapter_provider_path_must_contain_the_version_header(self):
        with tempfile.TemporaryDirectory(prefix='invalid adapter provider ') as directory:
            root = Path(directory)
            tool, include = self.fixture(root)
            (include / 'adapter_mbedtls.h').write_text('#include "mbedtls/version.h"\n')
            result = self.run_check(tool, include, root)
            self.assertEqual(2, result.returncode, result.stderr)
            self.assertIn('include/mbedtls/version.h', result.stderr)

    def test_adapter_headers_compile_in_c_and_cpp_with_provider_path_spaces(self):
        with tempfile.TemporaryDirectory(prefix='adapter provider spaces ') as directory:
            root = Path(directory)
            tool, include = self.fixture(root)
            provider = self.provider_fixture(root)
            (include / 'adapter_mbedtls.h').write_text(
                '#include "mbedtls/version.h"\n'
                '#if MBEDTLS_VERSION_NUMBER != 0x03060700\n'
                '#error "Wrong provider"\n#endif\n')
            result = self.run_check(tool, include, provider)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertIn('checking C public', result.stdout)
            self.assertIn('checking C++ public', result.stdout)

    def test_adapter_headers_use_the_prepared_cache_without_environment_setup(self):
        with tempfile.TemporaryDirectory(prefix='cached adapter provider ') as directory:
            root = Path(directory)
            tool, include = self.fixture(root)
            lookup = runpy.run_path(str(ROOT / 'tools/providers/provider_cache.py'))
            _, providers = lookup['load_lock']()
            provider = providers['mbedtls']
            source = lookup['managed_provider_directory'](root / 'provider cache', provider)
            for relative in provider['required_files']:
                path = source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('')
            (source / 'include/mbedtls/version.h').write_text(
                '#define MBEDTLS_VERSION_NUMBER 0x03060700\n')
            (include / 'adapter_mbedtls.h').write_text('#include "mbedtls/version.h"\n')
            result = self.run_check(tool, include)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertIn('checking C++ public', result.stdout)
            result = self.run_check(tool, include, '')
            self.assertEqual(2, result.returncode, result.stdout + result.stderr)
            self.assertIn('ESY_MBEDTLS_PATH is required', result.stderr)

    def test_provider_path_does_not_hide_generic_header_dependency(self):
        with tempfile.TemporaryDirectory(prefix='generic provider isolation ') as directory:
            root = Path(directory)
            tool, include = self.fixture(root)
            provider = self.provider_fixture(root)
            (include / 'adapter_mbedtls.h').write_text('#include "mbedtls/version.h"\n')
            (include / 'generic.h').write_text('#include "mbedtls/version.h"\n')
            result = self.run_check(tool, include, provider)
            self.assertEqual(1, result.returncode, result.stdout + result.stderr)
            self.assertIn('generic.h', result.stderr)

    def test_filesystem_adapter_headers_require_only_their_explicit_provider(self):
        for name, variable in (('esy_fs_littlefs.h', 'ESY_LITTLEFS_PATH'),
                               ('esy_fs_flash.h', 'ESY_LITTLEFS_PATH'),
                               ('esy_fs_fatfs.h', 'ESY_FATFS_PATH')):
            for value in (None, ''):
                with self.subTest(header=name, value=value), tempfile.TemporaryDirectory(prefix='missing fs provider ') as directory:
                    tool, include = self.fixture(Path(directory))
                    header = include / name
                    header.write_text('struct value { int size; };\n')
                    providers = {} if value is None else {variable: value}
                    result = self.run_check(tool, header, providers=providers)
                    self.assertEqual(2, result.returncode, result.stdout + result.stderr)
                    self.assertIn(f'{variable} is required', result.stderr)

    def test_filesystem_provider_paths_validate_required_native_headers(self):
        cases = (('esy_fs_littlefs.h', 'ESY_LITTLEFS_PATH', 'lfs.h'),
                 ('esy_fs_flash.h', 'ESY_LITTLEFS_PATH', 'lfs.h'),
                 ('esy_fs_fatfs.h', 'ESY_FATFS_PATH', 'ff.h'))
        for name, variable, missing in cases:
            with self.subTest(header=name), tempfile.TemporaryDirectory(prefix='invalid fs provider ') as directory:
                root = Path(directory)
                tool, include = self.fixture(root)
                (include / name).write_text('struct value { int size; };\n')
                result = self.run_check(tool, include, providers={variable: root})
                self.assertEqual(2, result.returncode, result.stdout + result.stderr)
                self.assertIn(f'{variable} must contain {missing}', result.stderr)

    def test_filesystem_adapters_support_spaces_and_isolate_fatfs_configuration(self):
        with tempfile.TemporaryDirectory(prefix='filesystem provider spaces ') as directory:
            root = Path(directory)
            tool, include = self.fixture(root)
            providers = self.filesystem_provider_fixtures(root)
            for name in ('esy_fs_littlefs.h', 'esy_fs_flash.h'):
                (include / name).write_text('#include "lfs.h"\n')
            (include / 'esy_fs_fatfs.h').write_text('#include "ff.h"\n')
            before = {item.name: item.read_bytes() for item in providers['ESY_FATFS_PATH'].iterdir()}
            result = self.run_check(tool, include, providers=providers)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertIn('checking C++ public', result.stdout)
            after = {item.name: item.read_bytes() for item in providers['ESY_FATFS_PATH'].iterdir()}
            self.assertEqual(before, after)

    def test_filesystem_core_and_file_binding_ignore_unneeded_provider_paths(self):
        with tempfile.TemporaryDirectory(prefix='filesystem generic headers ') as directory:
            tool, include = self.fixture(Path(directory))
            for name in ('esy_fs.h', 'esy_fs_block.h', 'esy_file_fs.h'):
                (include / name).write_text('#include <stddef.h>\nstruct value { size_t n; };\n')
            result = self.run_check(tool, include, providers={
                'ESY_LITTLEFS_PATH': '/missing littlefs', 'ESY_FATFS_PATH': '/missing fatfs'})
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_filesystem_provider_paths_never_hide_generic_header_dependencies(self):
        for name, native in (('esy_fs_littlefs.h', 'lfs.h'), ('esy_fs_fatfs.h', 'ff.h')):
            with self.subTest(native=native), tempfile.TemporaryDirectory(prefix='filesystem header isolation ') as directory:
                root = Path(directory)
                tool, include = self.fixture(root)
                providers = self.filesystem_provider_fixtures(root)
                (include / name).write_text(f'#include "{native}"\n')
                (include / 'generic.h').write_text(f'#include "{native}"\n')
                result = self.run_check(tool, include, providers=providers)
                self.assertEqual(1, result.returncode, result.stdout + result.stderr)
                self.assertIn('generic.h', result.stderr)

    def test_filesystem_adapters_do_not_receive_each_others_provider(self):
        with tempfile.TemporaryDirectory(prefix='filesystem provider boundaries ') as directory:
            root = Path(directory)
            tool, include = self.fixture(root)
            providers = self.filesystem_provider_fixtures(root)
            (include / 'esy_fs_littlefs.h').write_text('#include "lfs.h"\n#include "ff.h"\n')
            (include / 'esy_fs_fatfs.h').write_text('#include "ff.h"\n#include "lfs.h"\n')
            result = self.run_check(tool, include, providers=providers)
            self.assertEqual(1, result.returncode, result.stdout + result.stderr)
            self.assertIn('esy_fs_littlefs.h', result.stderr)
            self.assertIn('esy_fs_fatfs.h', result.stderr)


if __name__ == '__main__':
    unittest.main()
