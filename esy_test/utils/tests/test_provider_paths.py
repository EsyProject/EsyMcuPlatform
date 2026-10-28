#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Exercise prepared-cache lookup through directly executed host test entries."""

import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


PLATFORM = Path(__file__).resolve().parents[3]
LOOKUP = runpy.run_path(str(PLATFORM / "tools/providers/provider_cache.py"))


class ProviderPathsTests(unittest.TestCase):
    def cache_fixture(self, cache):
        _, providers = LOOKUP["load_lock"]()
        paths = {}
        for name, provider in providers.items():
            source = LOOKUP["managed_provider_directory"](cache, provider)
            source /= provider.get("source_subdirectory", "")
            for relative in provider["required_files"]:
                path = source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("", encoding="utf-8")
            paths[name] = str(source)
        environment = dict(os.environ, ESY_PROVIDER_CACHE_DIR=str(cache))
        for provider in providers.values():
            environment.pop(provider["environment"], None)
        return environment, paths

    def test_shared_lookup_preserves_explicit_input_without_global_side_effects(self):
        with tempfile.TemporaryDirectory(prefix="host provider cache ") as directory:
            environment, paths = self.cache_fixture(Path(directory))
            with mock.patch.dict(os.environ, environment, clear=True):
                before_environment = dict(os.environ)
                before_path = list(sys.path)
                helper = runpy.run_path(str(PLATFORM / "esy_test/utils/esy_provider_paths.py"))
                self.assertEqual(paths["coremqtt"], helper["cached_provider"]("coremqtt"))
                self.assertEqual(paths["fatfs"], helper["cached_provider"]("fatfs"))
                self.assertEqual(before_environment, dict(os.environ))
                self.assertEqual(before_path, sys.path)
                for explicit in ("", "/invalid explicit provider"):
                    self.assertEqual(explicit, helper["cached_provider"](
                        "coremqtt", dict(environment, ESY_COREMQTT_PATH=explicit)))

    def test_smoke_scripts_resolve_the_cache_without_sourcing_environment(self):
        with tempfile.TemporaryDirectory(prefix="smoke provider cache ") as directory:
            environment, paths = self.cache_fixture(Path(directory))
            with mock.patch.dict(os.environ, environment, clear=True):
                tls = runpy.run_path(str(PLATFORM / "esy_middleware/tls/tests/test_tls_smoke.py"))
                http = runpy.run_path(str(PLATFORM / "esy_middleware/http/tests/test_http_smoke.py"))
                mqtt = runpy.run_path(str(PLATFORM / "esy_middleware/mqtt/tests/test_broker_smoke.py"))
                self.assertEqual(paths["mbedtls"], tls["MBEDTLS"])
                self.assertEqual(paths["mbedtls"], http["MBEDTLS"])
                self.assertEqual(paths["llhttp"], http["LLHTTP"])
                self.assertEqual(paths["coremqtt"], mqtt["cached_provider"]("coremqtt"))
                self.assertNotIn("ESY_MBEDTLS_PATH", os.environ)

    def test_filesystem_runner_uses_cache_and_preserves_ceedling_status(self):
        with tempfile.TemporaryDirectory(prefix="filesystem runner cache ") as directory:
            root = Path(directory)
            environment, paths = self.cache_fixture(root / "cache with spaces")
            for relative in (
                    "tools/providers/provider_cache.py", "tools/providers/providers.lock.json",
                    "esy_test/utils/esy_provider_paths.py",
                    "esy_middleware/fs/tests/run_backend_tests.py",
                    "esy_middleware/fs/config/fatfs/ffconf.h"):
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(PLATFORM / relative, destination)
            tools = root / "bin"
            tools.mkdir()
            executable = tools / "ceedling"
            executable.write_text(
                f"#!{sys.executable}\n"
                "import json, os\n"
                "from pathlib import Path\n"
                "Path(os.environ['CAPTURE']).write_text(json.dumps({"
                "'cwd': os.getcwd(), 'littlefs': os.environ['ESY_LITTLEFS_PATH'],"
                "'fatfs': os.environ['ESY_TEST_FATFS_PATH']}))\nraise SystemExit(7)\n",
                encoding="utf-8")
            executable.chmod(0o755)
            capture = root / "capture.json"
            environment.update(PATH=str(tools) + os.pathsep + os.environ["PATH"],
                               CAPTURE=str(capture))
            runner = root / "esy_middleware/fs/tests/run_backend_tests.py"
            result = subprocess.run([sys.executable, "-B", str(runner)], cwd=root,
                                    env=environment, text=True, capture_output=True)
            self.assertEqual(7, result.returncode, result.stdout + result.stderr)
            captured = json.loads(capture.read_text())
            self.assertEqual(str(runner.parent), captured["cwd"])
            self.assertEqual(paths["littlefs"], captured["littlefs"])
            self.assertEqual((runner.parent / "build/providers/fatfs/ffconf.h").read_bytes(),
                             (root / "esy_middleware/fs/config/fatfs/ffconf.h").read_bytes())
            self.assertFalse((Path(paths["fatfs"]) / "ffconf.h").exists())
            self.assertEqual((Path(paths["fatfs"]) / "ff.c").read_bytes(),
                             (Path(captured["fatfs"]) / "ff.c").read_bytes())


if __name__ == "__main__":
    unittest.main()
