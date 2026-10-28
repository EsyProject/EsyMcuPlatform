# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Exercise provider preparation offline with local repositories and ZIP bytes."""

import contextlib
import errno
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile


SCRIPT = Path(__file__).with_name("prepare_providers.py")
DEFAULTS = ("corejson", "coremqtt", "llhttp", "mbedtls", "littlefs", "fatfs", "lvgl")
VARIABLES = {
    "corejson": "ESY_COREJSON_PATH",
    "coremqtt": "ESY_COREMQTT_PATH",
    "llhttp": "ESY_LLHTTP_PATH",
    "mbedtls": "ESY_MBEDTLS_PATH",
    "littlefs": "ESY_LITTLEFS_PATH",
    "fatfs": "ESY_FATFS_PATH",
    "lvgl": "LVGL_PATH",
}


class ProviderPreparationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="provider preparation ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.platform = self.root / "platform checkout"
        self.script = self.platform / "tools/providers/prepare_providers.py"
        self.script.parent.mkdir(parents=True)
        shutil.copyfile(SCRIPT, self.script)
        shutil.copyfile(SCRIPT.with_name("provider_cache.py"),
                        self.script.with_name("provider_cache.py"))
        self.lock = self.script.with_name("providers.lock.json")
        self.cwd = self.root / "unrelated invocation directory"
        self.cwd.mkdir()
        self.cache = self.root / "managed provider directory"
        self.environ = {
            key: value for key, value in os.environ.items()
            if key not in VARIABLES.values() and not key.startswith("GIT_")
        }
        self.environ["ESY_PROVIDER_CACHE_DIR"] = str(self.cache)
        self.environ.update(GIT_ALLOW_PROTOCOL="file", GIT_TERMINAL_PROMPT="0",
                            GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
        self.providers = {
            name: {
                "version": "fixture-1", "kind": "git",
                "url": str(self.root / "unavailable local remote"),
                "revision": "1" * 40, "directory": f"{name}-fixture",
                "environment": variable,
                "required_files": ["source/library.c", "include/library.h"],
            }
            for name, variable in VARIABLES.items()
        }
        self.providers["fatfs"].update(
            kind="zip", url="https://invalid.example/fixture.zip",
            sha256="0" * 64, source_subdirectory="source",
            required_files=["ff.c", "ff.h"])
        self.providers["fatfs"].pop("revision")
        self.write_lock()
        spec = importlib.util.spec_from_file_location(
            "cache_fixture", self.script.with_name("provider_cache.py"))
        self.lookup = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.lookup)
        spec = importlib.util.spec_from_file_location("provider_fixture", self.script)
        self.tool = importlib.util.module_from_spec(spec)
        with mock.patch.dict(sys.modules, {"provider_cache": self.lookup}):
            spec.loader.exec_module(self.tool)

    def write_lock(self, defaults=DEFAULTS):
        self.lock.write_text(json.dumps({
            "schema_version": 1, "default_providers": list(defaults),
            "providers": self.providers,
        }), encoding="utf-8")

    def run_cli(self, *arguments, environment=None, cwd=None):
        return subprocess.run(
            [sys.executable, "-B", str(self.script), *(str(arg) for arg in arguments)],
            env=self.environ if environment is None else environment,
            cwd=self.cwd if cwd is None else cwd, capture_output=True, text=True,
            timeout=30)

    def run_cache_cli(self, *arguments, environment=None):
        return subprocess.run(
            [sys.executable, "-B", str(self.script.with_name("provider_cache.py")),
             *(str(arg) for arg in arguments)],
            env=self.environ if environment is None else environment, cwd=self.cwd,
            capture_output=True, text=True, timeout=30)

    def cached_layout(self, provider):
        source = self.lookup.managed_provider_directory(self.cache, provider)
        source /= provider.get("source_subdirectory", "")
        for relative in provider["required_files"]:
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("cached fixture input\n", encoding="utf-8")
        return source

    def run_main(self, *arguments, download=None):
        stdout, stderr = io.StringIO(), io.StringIO()
        blocked_download = AssertionError("Unexpected network access in offline test")
        with mock.patch.dict(os.environ, self.environ, clear=True), \
                mock.patch.object(self.tool, "urlopen",
                                  side_effect=download or blocked_download) as opened, \
                contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            environment_before = dict(os.environ)
            status = self.tool.main([str(arg) for arg in arguments])
            self.assertEqual(environment_before, dict(os.environ),
                             "Provider preparation must not modify the calling environment")
        return status, stdout.getvalue(), stderr.getvalue(), opened

    def override(self, name):
        source = self.root / f"user {name} sources"
        for relative in self.providers[name]["required_files"]:
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("user-owned provider input\n", encoding="utf-8")
        self.environ[VARIABLES[name]] = str(source)
        return source

    def child_command(self, capture, exit_code=0):
        child = self.root / "child command with spaces.py"
        child.write_text(
            "import json, os, sys\nfrom pathlib import Path\n"
            "Path(sys.argv[1]).write_text(json.dumps({"
            "'cwd': os.getcwd(), 'argv': sys.argv[2:], "
            f"'providers': {{key: os.environ.get(key) for key in {list(VARIABLES.values())!r}}}, "
            "'sentinel': os.environ.get('PROVIDER_TEST_SENTINEL')}))\n"
            f"sys.exit({exit_code})\n", encoding="utf-8")
        return [sys.executable, str(child), str(capture)]

    def git(self, directory, *arguments):
        return subprocess.run(
            ["git", "-c", "user.name=Provider fixture", "-c",
             "user.email=provider@example.invalid", "-c", "commit.gpgsign=false",
             "-c", "protocol.file.allow=always", "-C", str(directory), *arguments],
            env=self.environ, capture_output=True, text=True, check=True).stdout.strip()

    def local_git_provider(self, name="corejson"):
        remote = self.root / f"local {name} remote"
        remote.mkdir()
        self.git(remote, "init", "--quiet")
        for relative in self.providers[name]["required_files"]:
            path = remote / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("pinned upstream input\n", encoding="utf-8")
        self.git(remote, "add", ".")
        self.git(remote, "commit", "--quiet", "-m", "Create provider fixture")
        self.providers[name].update(url=str(remote), revision=self.git(remote, "rev-parse", "HEAD"))
        self.write_lock()
        return remote, self.tool.managed_provider_directory(self.cache, self.providers[name])

    def zip_bytes(self, extra=()):
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, "w") as archive:
            archive.writestr("source/ff.c", "pinned source\n")
            archive.writestr("source/ff.h", "pinned header\n")
            archive.writestr("docs/license.txt", "upstream attribution\n")
            for name, data in extra:
                archive.writestr(name, data)
        payload = stream.getvalue()
        self.providers["fatfs"]["sha256"] = hashlib.sha256(payload).hexdigest()
        self.write_lock()
        return payload

    def prepare_archive(self, payload):
        result = self.run_main("--directory", self.cache, "--provider", "fatfs",
                               download=lambda *args, **kwargs: io.BytesIO(payload))
        self.assertEqual(0, result[0], result[2])
        return self.tool.managed_provider_directory(self.cache, self.providers["fatfs"])

    def test_default_selection_exports_seven_overrides_without_fetching_or_creating_cache(self):
        sources = {name: self.override(name) for name in DEFAULTS}
        result = self.run_cli("--offline")
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual({f"{VARIABLES[name]}={source}" for name, source in sources.items()},
                         set(result.stdout.splitlines()))
        self.assertIn(f"LVGL_PATH={sources['lvgl']}", result.stdout)
        self.assertFalse((self.platform / ".third-party").exists())
        self.assertFalse((self.cwd / ".third-party").exists())

    def test_repeated_selection_and_command_arguments_preserve_cwd_and_exit(self):
        corejson, lvgl = self.override("corejson"), self.override("lvgl")
        self.environ["PROVIDER_TEST_SENTINEL"] = "caller value"
        capture = self.root / "child capture.json"
        result = self.run_cli(
            "--provider", "corejson", "--provider", "lvgl", "--provider", "corejson",
            "--offline", "--exec", *self.child_command(capture, exit_code=23),
            "one argument with spaces", "--child-option")
        self.assertEqual(23, result.returncode, result.stderr)
        observed = json.loads(capture.read_text())
        self.assertEqual(str(self.cwd), observed["cwd"])
        self.assertEqual(["one argument with spaces", "--child-option"], observed["argv"])
        self.assertEqual(str(corejson), observed["providers"]["ESY_COREJSON_PATH"])
        self.assertEqual(str(lvgl), observed["providers"]["LVGL_PATH"])
        self.assertIsNone(observed["providers"]["ESY_COREMQTT_PATH"])
        self.assertEqual("caller value", observed["sentinel"])

    def test_explicit_override_wins_over_invalid_managed_cache_without_mutating_either(self):
        source = self.override("corejson")
        managed = self.tool.managed_provider_directory(self.cache, self.providers["corejson"])
        managed.mkdir(parents=True)
        marker = managed / "user-owned.txt"
        marker.write_text("leave this alone\n")
        result = self.run_cli("--directory", self.cache, "--provider", "corejson", "--offline")
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(f"ESY_COREJSON_PATH={source}\n", result.stdout)
        self.assertEqual("leave this alone\n", marker.read_text())
        self.assertEqual("user-owned provider input\n", (source / "source/library.c").read_text())
        self.assertFalse((source / ".git").exists())

    def test_empty_missing_or_incomplete_override_fails_before_command_without_fallback(self):
        capture = self.root / "must not execute.json"
        incomplete = self.root / "incomplete user source"
        incomplete.mkdir()
        for value in ("", " \t", str(self.root / "missing override"), str(incomplete)):
            with self.subTest(value=value):
                environment = dict(self.environ, ESY_COREJSON_PATH=value)
                result = self.run_cli(
                    "--directory", self.cache, "--provider", "corejson", "--exec",
                    *self.child_command(capture), environment=environment)
                self.assertEqual(1, result.returncode, result.stderr)
                self.assertIn("error:", result.stderr)
                self.assertFalse(capture.exists())
                self.assertFalse(self.cache.exists())

    def test_fatfs_override_already_names_the_public_source_directory(self):
        source = self.override("fatfs")
        result = self.run_cli("--provider", "fatfs", "--offline")
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(f"ESY_FATFS_PATH={source}\n", result.stdout)
        self.assertFalse((source / "source").exists())

    def test_offline_missing_provider_fails_before_command_and_never_creates_cache(self):
        capture = self.root / "must not execute.json"
        result = self.run_cli("--directory", self.cache, "--provider", "corejson",
                              "--offline", "--exec", *self.child_command(capture))
        self.assertEqual(1, result.returncode, result.stderr)
        self.assertIn("offline", result.stderr)
        self.assertFalse(capture.exists())
        self.assertFalse(self.cache.exists())

    def test_invalid_selection_or_empty_exec_is_a_cli_error(self):
        for arguments in (("--provider", "unknown"), ("--exec",)):
            with self.subTest(arguments=arguments):
                result = self.run_cli(*arguments)
                self.assertEqual(2, result.returncode, result.stderr)
                self.assertFalse(self.cache.exists())

    def test_default_managed_directory_is_shared_user_cache_not_invocation_cwd(self):
        _, unused = self.local_git_provider()
        result = self.run_cli("--provider", "corejson")
        expected = self.tool.managed_provider_directory(self.cache, self.providers["corejson"])
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(f"ESY_COREJSON_PATH={expected}\n", result.stdout)
        self.assertTrue((expected / "source/library.c").is_file())
        self.assertFalse((self.cwd / ".third-party").exists())

    def test_all_selection_includes_every_locked_provider(self):
        for name in VARIABLES:
            self.override(name)
        result = self.run_cli("--all", "--offline")
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(7, len(result.stdout.splitlines()))
        self.assertIn("LVGL_PATH=", result.stdout)

    def test_setup_without_arguments_prepares_all_seven_host_providers(self):
        baseline = json.loads(SCRIPT.with_name("providers.lock.json").read_text(encoding="utf-8"))
        self.assertEqual(list(DEFAULTS), baseline["default_providers"])
        for name in DEFAULTS:
            if name == "fatfs":
                archive = self.root / "local FatFs archive.zip"
                archive.write_bytes(self.zip_bytes())
                self.providers[name]["url"] = archive.as_uri()
            else:
                self.local_git_provider(name)
        self.write_lock()
        setup = self.platform / "setup.sh"
        shutil.copyfile(SCRIPT.parents[2] / "setup.sh", setup)
        environment = dict(self.environ, **{variable: "/missing old provider override"
                                            for variable in VARIABLES.values()})
        self.assertFalse(self.cache.exists())
        result = subprocess.run(
            ["bash", str(setup)], env=environment, cwd=self.cwd,
            capture_output=True, text=True, timeout=30)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(set(VARIABLES[name] for name in DEFAULTS),
                         {line.split("=", 1)[0] for line in result.stdout.splitlines()})
        lvgl = self.lookup.managed_provider_directory(self.cache, self.providers["lvgl"])
        self.assertTrue(lvgl.is_dir())
        expected = {self.lookup.managed_provider_directory(self.cache, provider).name
                    for provider in self.providers.values()}
        self.assertEqual(expected, {entry.name for entry in self.cache.iterdir()})

    def test_changed_library_pin_uses_a_separate_cache_without_changing_previous_checkout(self):
        remote, baseline = self.local_git_provider()
        result = self.run_cli("--provider", "corejson")
        self.assertEqual(0, result.returncode, result.stderr)
        baseline_pin = self.git(baseline, "rev-parse", "HEAD")
        (remote / "source/library.c").write_text("new pinned upstream input\n")
        self.git(remote, "commit", "--quiet", "-am", "Update provider pin")
        provider = dict(self.providers["corejson"], revision=self.git(remote, "rev-parse", "HEAD"))
        self.providers["corejson"] = provider
        self.write_lock()
        result = self.run_cli("--provider", "corejson", "--offline")
        self.assertEqual(1, result.returncode, result.stderr)
        self.assertIn("offline", result.stderr)
        updated = self.lookup.managed_provider_directory(self.cache, provider)
        self.assertFalse(updated.exists())
        result = self.run_cli("--provider", "corejson")
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(f"ESY_COREJSON_PATH={updated}\n", result.stdout)
        self.assertEqual(provider["revision"], self.git(updated, "rev-parse", "HEAD"))
        self.assertEqual(baseline_pin, self.git(baseline, "rev-parse", "HEAD"))
        self.assertEqual("pinned upstream input\n", (baseline / "source/library.c").read_text())
        cached = self.run_cache_cli("--json")
        self.assertEqual(0, cached.returncode, cached.stderr)
        self.assertEqual({"ESY_COREJSON_PATH": str(updated)}, json.loads(cached.stdout))

    def test_cache_lookup_uses_the_supplied_environment_without_modifying_it(self):
        baseline = self.cached_layout(self.providers["corejson"])
        environment = dict(self.environ)
        before = dict(environment)
        with mock.patch.dict(os.environ, dict(self.environ, ESY_COREJSON_PATH="/different global path"),
                             clear=True):
            self.assertEqual(str(baseline), self.lookup.cached_provider("corejson", environment))
            self.assertEqual({"ESY_COREJSON_PATH": str(baseline)},
                             self.lookup.load_cached_environment(environment=environment))
        self.assertEqual(before, environment)

    def test_cache_identity_changes_when_locked_revision_changes(self):
        provider = self.providers["corejson"]
        first = self.tool.managed_provider_directory(self.cache, provider)
        changed = dict(provider, revision="2" * 40)
        second = self.tool.managed_provider_directory(self.cache, changed)
        self.assertNotEqual(first, second)
        self.assertEqual("corejson-fixture-" + "1" * 12, first.name)

    def test_setup_populates_shared_cache_despite_old_shell_override_and_new_shell_finds_it(self):
        _, managed = self.local_git_provider()
        shutil.copyfile(SCRIPT.parents[2] / "setup.sh", self.platform / "setup.sh")
        environment = dict(self.environ, ESY_COREJSON_PATH="/missing/old/provider")
        result = subprocess.run(
            ["bash", str(self.platform / "setup.sh"), "--provider", "corejson"],
            env=environment, cwd=self.cwd, capture_output=True, text=True, timeout=30)
        self.assertEqual(0, result.returncode, result.stderr)
        fresh = subprocess.run(
            [sys.executable, "-B", str(self.script.with_name("provider_cache.py")), "--json"],
            env=self.environ, cwd=self.cwd, capture_output=True, text=True, timeout=30)
        self.assertEqual(0, fresh.returncode, fresh.stderr)
        self.assertEqual(str(managed), json.loads(fresh.stdout)["ESY_COREJSON_PATH"])
        self.assertFalse((self.platform / ".third-party").exists())

    def test_xdg_cache_is_discovered_from_another_checkout(self):
        _, unused = self.local_git_provider()
        environment = dict(self.environ, XDG_CACHE_HOME=str(self.root / "user cache"))
        environment.pop("ESY_PROVIDER_CACHE_DIR")
        result = self.run_cli("--provider", "corejson", environment=environment)
        self.assertEqual(0, result.returncode, result.stderr)
        second = self.root / "another checkout/tools/providers"
        second.mkdir(parents=True)
        for name in ("provider_cache.py", "providers.lock.json"):
            shutil.copyfile(self.script.with_name(name), second / name)
        result = subprocess.run(
            [sys.executable, "-B", str(second / "provider_cache.py"), "--json"],
            env=environment, cwd=self.cwd, capture_output=True, text=True, timeout=30)
        self.assertEqual(0, result.returncode, result.stderr)
        value = json.loads(result.stdout)["ESY_COREJSON_PATH"]
        self.assertTrue(value.startswith(str(self.root / "user cache/esymcuplatform/providers")))

    def test_read_only_cache_lookup_preserves_empty_override_without_downloading(self):
        environment = dict(self.environ, ESY_COREJSON_PATH="")
        result = subprocess.run(
            [sys.executable, "-B", str(self.script.with_name("provider_cache.py")), "--json"],
            env=environment, cwd=self.cwd, capture_output=True, text=True, timeout=30)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual("", json.loads(result.stdout)["ESY_COREJSON_PATH"])
        self.assertFalse(self.cache.exists())

    def test_require_default_without_setup_fails_before_command_with_setup_hint(self):
        capture = self.root / "must not execute.json"
        result = subprocess.run(
            [sys.executable, "-B", str(self.script.with_name("provider_cache.py")),
             "--require-default", "--exec", *self.child_command(capture)],
            env=self.environ, cwd=self.cwd, capture_output=True, text=True, timeout=30)
        self.assertEqual(1, result.returncode, result.stderr)
        self.assertIn("./setup.sh", result.stderr)
        self.assertFalse(capture.exists())
        self.assertFalse(self.cache.exists())

    def test_cache_root_precedence_and_relative_roots_are_rejected(self):
        environment = dict(self.environ, XDG_CACHE_HOME=str(self.root / "xdg"))
        self.assertEqual(self.cache, self.tool.default_cache_directory(environment))
        environment.pop("ESY_PROVIDER_CACHE_DIR")
        self.assertEqual(self.root / "xdg/esymcuplatform/providers",
                         self.tool.default_cache_directory(environment))
        for variable in ("ESY_PROVIDER_CACHE_DIR", "XDG_CACHE_HOME"):
            with self.subTest(variable=variable), self.assertRaisesRegex(ValueError, variable):
                self.tool.default_cache_directory({variable: "relative/cache"})

    def test_local_git_bootstrap_offline_reuse_and_child_only_environment(self):
        remote, managed = self.local_git_provider()
        capture = self.root / "managed child.json"
        status, stdout, stderr, opened = self.run_main(
            "--directory", self.cache, "--provider", "corejson", "--exec",
            *self.child_command(capture))
        self.assertEqual(0, status, stderr)
        opened.assert_not_called()
        self.assertEqual(str(managed), json.loads(capture.read_text())["providers"]["ESY_COREJSON_PATH"])
        self.assertEqual(self.providers["corejson"]["revision"], self.git(managed, "rev-parse", "HEAD"))
        self.assertEqual("", self.git(managed, "status", "--porcelain"))
        original_mtime = (managed / "source/library.c").stat().st_mtime_ns
        remote.rename(remote.with_name("unavailable upstream"))
        result = self.run_cli("--directory", self.cache, "--provider", "corejson", "--offline")
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(original_mtime, (managed / "source/library.c").stat().st_mtime_ns)

    def test_dirty_or_untracked_git_cache_is_rejected_without_reset_or_child_execution(self):
        _, managed = self.local_git_provider()
        result = self.run_cli("--directory", self.cache, "--provider", "corejson")
        self.assertEqual(0, result.returncode, result.stderr)
        original_head = self.git(managed, "rev-parse", "HEAD")
        capture = self.root / "must not execute.json"
        for relative in ("source/library.c", "local-note.txt"):
            with self.subTest(relative=relative):
                changed = managed / relative
                previous = changed.read_bytes() if changed.exists() else None
                changed.write_text("uncommitted caller work\n")
                result = self.run_cli("--directory", self.cache, "--provider", "corejson",
                    "--exec", *self.child_command(capture))
                self.assertEqual(1, result.returncode, result.stderr)
                self.assertIn("local changes", result.stderr)
                self.assertEqual("uncommitted caller work\n", changed.read_text())
                self.assertEqual(original_head, self.git(managed, "rev-parse", "HEAD"))
                self.assertFalse(capture.exists())
                if previous is None:
                    changed.unlink()
                else:
                    changed.write_bytes(previous)

    def test_changed_git_lock_pin_uses_new_cache_without_altering_old_checkout(self):
        _, managed = self.local_git_provider()
        result = self.run_cli("--directory", self.cache, "--provider", "corejson")
        self.assertEqual(0, result.returncode, result.stderr)
        original_head = self.git(managed, "rev-parse", "HEAD")
        self.providers["corejson"]["revision"] = "f" * 40
        self.write_lock()
        result = self.run_cli("--directory", self.cache, "--provider", "corejson", "--offline")
        self.assertEqual(1, result.returncode, result.stderr)
        self.assertIn("missing in offline mode", result.stderr)
        self.assertEqual(original_head, self.git(managed, "rev-parse", "HEAD"))
        self.assertEqual("", self.git(managed, "status", "--porcelain"))

    def test_git_submodules_are_initialized_and_uninitialized_reuse_is_rejected(self):
        nested, _ = self.local_git_provider("coremqtt")
        remote, managed = self.local_git_provider("corejson")
        self.git(remote, "submodule", "add", "--quiet", str(nested), "framework")
        self.git(remote, "commit", "--quiet", "-am", "Pin local framework")
        self.providers["corejson"].update(submodules=True, revision=self.git(remote, "rev-parse", "HEAD"))
        managed = self.tool.managed_provider_directory(self.cache, self.providers["corejson"])
        self.write_lock()
        result = self.run_cli("--directory", self.cache, "--provider", "corejson")
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertTrue((managed / "framework/source/library.c").is_file())
        self.git(managed, "submodule", "deinit", "--force", "--", "framework")
        result = self.run_cli("--directory", self.cache, "--provider", "corejson", "--offline")
        self.assertEqual(1, result.returncode, result.stderr)
        self.assertIn("submodules", result.stderr)
        self.assertFalse((managed / "framework/source/library.c").exists())

    def test_existing_non_repository_and_symlink_cache_are_never_replaced(self):
        _, managed = self.local_git_provider()
        managed.mkdir(parents=True)
        marker = managed / "owner.txt"
        marker.write_text("preserve existing directory\n")
        result = self.run_cli("--directory", self.cache, "--provider", "corejson")
        self.assertEqual(1, result.returncode, result.stderr)
        self.assertEqual("preserve existing directory\n", marker.read_text())
        marker.unlink()
        managed.rmdir()
        destination = self.override("corejson")
        self.environ.pop("ESY_COREJSON_PATH")
        managed.symlink_to(destination, target_is_directory=True)
        result = self.run_cli("--directory", self.cache, "--provider", "corejson")
        self.assertEqual(1, result.returncode, result.stderr)
        self.assertTrue(managed.is_symlink())
        self.assertEqual("user-owned provider input\n", (destination / "source/library.c").read_text())

    def test_zip_bootstrap_checks_digest_exports_source_subdirectory_and_reuses_offline(self):
        payload = self.zip_bytes()
        managed = self.prepare_archive(payload)
        self.assertEqual("pinned source\n", (managed / "source/ff.c").read_text())
        self.assertEqual("upstream attribution\n", (managed / "docs/license.txt").read_text())
        stamp = json.loads((managed / ".esy-provider.json").read_text())
        self.assertEqual({"source/ff.c", "source/ff.h", "docs/license.txt"}, set(stamp["files"]))
        status, stdout, stderr, opened = self.run_main(
            "--directory", self.cache, "--provider", "fatfs", "--offline")
        self.assertEqual(0, status, stderr)
        self.assertEqual(f"ESY_FATFS_PATH={managed / 'source'}\n", stdout)
        opened.assert_not_called()

    def test_zip_checksum_failure_does_not_publish_cache_or_run_command(self):
        payload = self.zip_bytes()
        self.providers["fatfs"]["sha256"] = "0" * 64
        self.write_lock()
        capture = self.root / "must not execute.json"
        status, stdout, stderr, opened = self.run_main(
            "--directory", self.cache, "--provider", "fatfs", "--exec",
            *self.child_command(capture), download=lambda *args, **kwargs: io.BytesIO(payload))
        self.assertEqual(1, status, stderr)
        self.assertIn("SHA256", stderr)
        self.assertFalse((self.tool.managed_provider_directory(self.cache, self.providers["fatfs"])).exists())
        self.assertFalse(capture.exists())
        self.assertEqual(1, opened.call_count)

    def test_zip_tampering_nonrequired_files_or_added_files_is_rejected_without_repair(self):
        managed = self.prepare_archive(self.zip_bytes())
        for relative in ("source/ff.c", "docs/license.txt", "local-note.txt"):
            with self.subTest(relative=relative):
                changed = managed / relative
                previous = changed.read_bytes() if changed.exists() else None
                changed.write_text("local archive edit\n")
                status, stdout, stderr, opened = self.run_main(
                    "--directory", self.cache, "--provider", "fatfs", "--offline")
                self.assertEqual(1, status, stderr)
                self.assertEqual("local archive edit\n", changed.read_text())
                opened.assert_not_called()
                if previous is None:
                    changed.unlink()
                else:
                    changed.write_bytes(previous)

    def test_zip_changed_lock_or_missing_stamp_never_replaces_existing_tree(self):
        managed = self.prepare_archive(self.zip_bytes())
        original = (managed / "source/ff.c").read_bytes()
        self.providers["fatfs"]["version"] = "changed-lock"
        self.write_lock()
        status, stdout, stderr, opened = self.run_main(
            "--directory", self.cache, "--provider", "fatfs")
        self.assertEqual(1, status, stderr)
        self.assertEqual(original, (managed / "source/ff.c").read_bytes())
        opened.assert_not_called()
        (managed / ".esy-provider.json").unlink()
        status, stdout, stderr, opened = self.run_main(
            "--directory", self.cache, "--provider", "fatfs", "--offline")
        self.assertEqual(1, status, stderr)
        self.assertIn("verification record", stderr)
        self.assertEqual(original, (managed / "source/ff.c").read_bytes())

    def test_concurrent_archive_publication_reuses_verified_winner_for_linux_conflicts(self):
        payload = self.zip_bytes()
        for error_number in (errno.EEXIST, errno.ENOTEMPTY):
            with self.subTest(error_number=error_number):
                directory = self.cache / str(error_number)
                managed = self.tool.managed_provider_directory(directory, self.providers["fatfs"])

                def another_preparer_wins(staged, destination):
                    shutil.copytree(staged, destination)
                    raise OSError(error_number, "Concurrent provider publication")

                with mock.patch.object(Path, "rename", autospec=True,
                                       side_effect=another_preparer_wins) as rename:
                    status, stdout, stderr, opened = self.run_main(
                        "--directory", directory, "--provider", "fatfs",
                        download=lambda *args, **kwargs: io.BytesIO(payload))
                self.assertEqual(0, status, stderr)
                self.assertEqual(f"ESY_FATFS_PATH={managed / 'source'}\n", stdout)
                self.assertEqual("pinned source\n", (managed / "source/ff.c").read_text())
                self.assertEqual(1, rename.call_count)
                self.assertEqual(1, opened.call_count)
                self.assertEqual([managed], list(directory.iterdir()))

    def test_concurrent_publication_still_rejects_invalid_winner_without_repairing_it(self):
        payload = self.zip_bytes()
        managed = self.tool.managed_provider_directory(self.cache, self.providers["fatfs"])

        def invalid_preparer_wins(staged, destination):
            shutil.copytree(staged, destination)
            (destination / "source/ff.c").write_text("other preparer's local edit\n")
            raise OSError(errno.ENOTEMPTY, "Concurrent provider publication")

        with mock.patch.object(Path, "rename", autospec=True,
                               side_effect=invalid_preparer_wins):
            status, stdout, stderr, opened = self.run_main(
                "--directory", self.cache, "--provider", "fatfs",
                download=lambda *args, **kwargs: io.BytesIO(payload))
        self.assertEqual(1, status, stderr)
        self.assertIn("do not match the lock", stderr)
        self.assertEqual("other preparer's local edit\n", (managed / "source/ff.c").read_text())

    def test_publication_permission_errors_are_not_treated_as_concurrent_success(self):
        payload = self.zip_bytes()
        with mock.patch.object(Path, "rename", autospec=True,
                               side_effect=PermissionError(errno.EACCES, "Publication denied")):
            status, stdout, stderr, opened = self.run_main(
                "--directory", self.cache, "--provider", "fatfs",
                download=lambda *args, **kwargs: io.BytesIO(payload))
        self.assertEqual(1, status, stderr)
        self.assertIn("Publication denied", stderr)
        self.assertFalse((self.tool.managed_provider_directory(self.cache, self.providers["fatfs"])).exists())

    def test_unsafe_zip_paths_and_symlinks_are_rejected_before_publication(self):
        symlink = zipfile.ZipInfo("source/provider-link")
        symlink.create_system = 3
        symlink.external_attr = (stat.S_IFLNK | 0o777) << 16
        for entry in ("../escaped-by-zip", str(self.root / "escaped-by-zip"),
                      "C:/escaped-by-zip", "source\\escaped-by-zip", symlink):
            with self.subTest(entry=entry):
                payload = self.zip_bytes(((entry, "escaped-by-zip"),))
                status, stdout, stderr, opened = self.run_main(
                    "--directory", self.cache, "--provider", "fatfs",
                    download=lambda *args, **kwargs: io.BytesIO(payload))
                self.assertEqual(1, status, stderr)
                self.assertIn("Unsafe archive path", stderr)
                self.assertFalse((self.tool.managed_provider_directory(self.cache, self.providers["fatfs"])).exists())
                self.assertFalse(any(self.root.rglob("escaped-by-zip")))


if __name__ == "__main__":
    unittest.main()
