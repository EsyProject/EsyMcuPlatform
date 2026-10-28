#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Prepare this library's locked host-test providers in the shared user cache."""

import argparse
import errno
import hashlib
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import shutil
import stat
import subprocess
import sys
import tempfile
from urllib.request import urlopen
import zipfile

from provider_cache import default_cache_directory, load_lock, managed_provider_directory

STAMP = ".esy-provider.json"


class ProviderError(RuntimeError):
    """A provider could not be prepared without altering existing input."""


def run_checked(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise ProviderError(f"{command[0]} failed: {result.stderr.strip()}")
    return result.stdout


def hash_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_path(path, provider):
    return path / provider.get("source_subdirectory", "")


def validate_layout(path, provider):
    source = source_path(path, provider)
    for relative in provider["required_files"]:
        if not (source / relative).is_file():
            raise ProviderError(f"{provider['environment']} is missing {source / relative}")
    return source


def lock_digest(provider):
    encoded = json.dumps(provider, sort_keys=True).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def archive_files(path):
    files = {}
    for item in sorted(path.rglob("*")):
        if item.is_symlink():
            raise ProviderError(f"Unexpected symlink in managed provider: {item}")
        if item.is_file() and item != path / STAMP:
            files[item.relative_to(path).as_posix()] = hash_file(item)
    return files


def validate_managed(path, provider):
    if path.is_symlink() or not path.is_dir():
        raise ProviderError(f"Managed provider must be a real directory: {path}")
    source = validate_layout(path, provider)
    if provider["kind"] == "git":
        git = ["git", "-C", str(path)]
        top = Path(run_checked(git + ["rev-parse", "--show-toplevel"]).strip()).resolve()
        head = run_checked(git + ["rev-parse", "HEAD"]).strip()
        if top != path.resolve() or head != provider["revision"]:
            raise ProviderError(f"Provider revision does not match the lock: {path}")
        dirty = run_checked(git + ["status", "--porcelain", "--untracked-files=normal"])
        if dirty:
            raise ProviderError(f"Managed provider has local changes; leaving it untouched: {path}")
        if provider.get("submodules"):
            status = run_checked(git + ["submodule", "status", "--recursive"])
            if any(line and not line.startswith(" ") for line in status.splitlines()):
                raise ProviderError(f"Provider submodules do not match the lock: {path}")
    elif provider["kind"] == "zip":
        stamp = path / STAMP
        if not stamp.is_file():
            raise ProviderError(f"Managed archive has no verification record: {path}")
        saved = json.loads(stamp.read_text(encoding="utf-8"))
        if (saved.get("lock_sha256") != lock_digest(provider) or
                saved.get("files") != archive_files(path)):
            raise ProviderError(f"Managed archive contents do not match the lock: {path}")
    else:
        raise ProviderError(f"Unsupported provider kind: {provider['kind']}")
    return source


def prepare_git(path, provider):
    git = ["git", "-C", str(path)]
    run_checked(["git", "init", "--quiet", str(path)])
    run_checked(git + ["remote", "add", "origin", provider["url"]])
    run_checked(git + ["fetch", "--quiet", "--depth=1", "origin", provider["revision"]])
    run_checked(git + ["checkout", "--quiet", "--detach", "FETCH_HEAD"])
    if provider.get("submodules"):
        run_checked(git + ["submodule", "update", "--init", "--recursive", "--depth=1"])


def download_archive(url, destination):
    with urlopen(url, timeout=60) as response, destination.open("wb") as output:
        shutil.copyfileobj(response, output)


def prepare_zip(path, provider):
    archive_path = path.parent / "download.zip"
    download_archive(provider["url"], archive_path)
    if hash_file(archive_path) != provider["sha256"]:
        raise ProviderError(f"Archive SHA256 mismatch: {provider['url']}")
    with zipfile.ZipFile(archive_path) as archive:
        for item in archive.infolist():
            relative = PurePosixPath(item.filename)
            if (relative.is_absolute() or ".." in relative.parts or
                    PureWindowsPath(item.filename).drive or "\\" in item.filename or
                    stat.S_ISLNK(item.external_attr >> 16)):
                raise ProviderError(f"Unsafe archive path: {item.filename}")
        archive.extractall(path)
    validate_layout(path, provider)
    stamp = {"lock_sha256": lock_digest(provider), "files": archive_files(path)}
    (path / STAMP).write_text(json.dumps(stamp, sort_keys=True) + "\n", encoding="utf-8")


def prepare_provider(provider, directory, environment, offline=False):
    variable = provider["environment"]
    if variable in environment:
        value = environment[variable]
        if not value.strip():
            raise ProviderError(f"{variable} is explicitly empty; unset it or supply a provider")
        source = Path(value).expanduser().resolve()
        # Overrides already point to the public source directory, including FatFs.
        validate_layout(source, dict(provider, source_subdirectory=""))
        print(f"[providers] using {variable}={source}", file=sys.stderr)
        return source

    path = managed_provider_directory(directory, provider)
    if path.exists() or path.is_symlink():
        return validate_managed(path, provider)
    if offline:
        raise ProviderError(f"Provider is missing in offline mode: {path}")

    print(f"[providers] preparing {variable} {provider['version']}", file=sys.stderr)
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".prepare-", dir=directory) as temporary:
        staged = Path(temporary) / "content"
        staged.mkdir()
        if provider["kind"] == "git":
            prepare_git(staged, provider)
        elif provider["kind"] == "zip":
            prepare_zip(staged, provider)
        else:
            raise ProviderError(f"Unsupported provider kind: {provider['kind']}")
        validate_managed(staged, provider)
        try:
            staged.rename(path)
        except OSError as error:
            if error.errno not in (errno.EEXIST, errno.ENOTEMPTY):
                raise
            # Another preparer may have completed the same immutable input.
            validate_managed(path, provider)
    return source_path(path, provider)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path,
                        help="Override the shared user cache directory")
    parser.add_argument("--all", action="store_true", help="Prepare all locked software providers")
    parser.add_argument("--ignore-overrides", action="store_true",
                        help="Populate the managed cache even when explicit provider paths are set")
    parser.add_argument("--provider", action="append", help="Provider name; repeat to select several")
    parser.add_argument("--offline", action="store_true", help="Validate existing providers without downloads")
    parser.add_argument("--exec", dest="command", nargs=argparse.REMAINDER,
                        help="Run a command with the selected provider environment, keeping the current directory")
    args = parser.parse_args(argv)
    if args.command == []:
        parser.error("--exec requires a command")
    try:
        defaults, providers = load_lock()
        selected = list(dict.fromkeys(args.provider if args.provider is not None else
                                     providers if args.all else defaults))
        unknown = set(selected) - providers.keys()
        if unknown:
            parser.error("Unknown providers: " + ", ".join(sorted(unknown)))
        environment = dict(os.environ)
        directory = (args.directory.expanduser().resolve() if args.directory is not None
                     else default_cache_directory())
        for name in selected:
            provider = providers[name]
            inputs = {} if args.ignore_overrides else environment
            source = prepare_provider(provider, directory, inputs, args.offline)
            environment[provider["environment"]] = str(source)
        if args.command is not None:
            result = subprocess.run(args.command, env=environment)
            return result.returncode if result.returncode >= 0 else 128 - result.returncode
        for name in selected:
            variable = providers[name]["environment"]
            print(f"{variable}={environment[variable]}")
        print("Host-test providers are ready. Library test tools discover the user cache automatically.",
              file=sys.stderr)
        return 0
    except (ProviderError, OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
