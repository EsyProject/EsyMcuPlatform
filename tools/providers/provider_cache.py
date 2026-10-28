#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Locate this library's prepared host-test providers without changing inputs."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

LOCKFILE = Path(__file__).with_name("providers.lock.json")


def load_lock():
    data = json.loads(LOCKFILE.read_text(encoding="utf-8"))
    if data.get("schema_version") != 1:
        raise ValueError("Unsupported provider lock schema")
    return data["default_providers"], data["providers"]


def default_cache_directory(environment=None):
    environment = os.environ if environment is None else environment
    for variable in ("ESY_PROVIDER_CACHE_DIR", "XDG_CACHE_HOME"):
        value = environment.get(variable, "")
        if value:
            path = Path(value)
            if not path.is_absolute():
                raise ValueError(f"{variable} must be an absolute path")
            return path if variable == "ESY_PROVIDER_CACHE_DIR" else path / "esymcuplatform/providers"
    if os.name == "nt" and environment.get("LOCALAPPDATA"):
        return Path(environment["LOCALAPPDATA"]) / "esymcuplatform/providers"
    home = environment.get("HOME") or environment.get("USERPROFILE")
    return (Path(home) if home else Path.home()) / ".cache/esymcuplatform/providers"


def managed_provider_directory(directory, provider):
    identity = provider["revision"] if provider["kind"] == "git" else provider["sha256"]
    return Path(directory) / f"{provider['directory']}-{identity[:12]}"


def cached_provider(name, environment=None):
    environment = os.environ if environment is None else environment
    _, providers = load_lock()
    provider = providers[name]
    variable = provider["environment"]
    if variable in environment:
        # Preserve explicit empty/invalid values for the caller's validation.
        return environment[variable]
    directory = managed_provider_directory(default_cache_directory(environment), provider)
    source = directory / provider.get("source_subdirectory", "")
    if all((source / relative).is_file() for relative in provider["required_files"]):
        return str(source)
    return None


def load_cached_environment(names=None, environment=None):
    environment = os.environ if environment is None else environment
    _, providers = load_lock()
    resolved = {}
    for name in providers if names is None else names:
        value = cached_provider(name, environment)
        if value is not None:
            resolved[providers[name]["environment"]] = value
    return resolved


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="Print available provider inputs as JSON")
    parser.add_argument("--require-default", action="store_true",
                        help="Require the default test providers before executing a command")
    parser.add_argument("--exec", dest="command", nargs=argparse.REMAINDER,
                        help="Run a command using the prepared cache, without network access")
    args = parser.parse_args(argv)
    if args.command == []:
        parser.error("--exec requires a command")
    try:
        values = load_cached_environment()
        if args.require_default:
            defaults, providers = load_lock()
            for name in defaults:
                provider = providers[name]
                value = values.get(provider["environment"])
                if not value or not all((Path(value) / item).is_file()
                                        for item in provider["required_files"]):
                    raise ValueError(f"{provider['environment']} is unavailable or invalid; "
                                     "run ./setup.sh in the EsyMcuPlatform directory")
        if args.command is not None:
            environment = dict(os.environ, **values)
            result = subprocess.run(args.command, env=environment)
            return result.returncode if result.returncode >= 0 else 128 - result.returncode
        print(json.dumps(values, sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
