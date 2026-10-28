# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0
"""Stage unmodified FatFs beside its required configuration, then run Ceedling."""
import argparse
import os
from pathlib import Path
import runpy
import shutil
import subprocess

PLATFORM = Path(__file__).resolve().parents[3]
cached_provider = runpy.run_path(
    str(PLATFORM / "esy_test/utils/esy_provider_paths.py"))["cached_provider"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--littlefs", default=cached_provider("littlefs"),
                        help="Official littlefs 2.11.2 source directory")
    parser.add_argument("--fatfs", default=cached_provider("fatfs"),
                        help="Official FatFs R0.16 source directory")
    parser.add_argument("--sanitize", action="store_true",
                        help="Enable AddressSanitizer and UndefinedBehaviorSanitizer")
    parser.add_argument("tasks", nargs="*", default=["test:all"])
    args = parser.parse_args()
    if not args.littlefs or not args.fatfs:
        parser.error("Run ./setup.sh, set ESY_LITTLEFS_PATH/ESY_FATFS_PATH, "
                     "or pass --littlefs/--fatfs")
    here = Path(__file__).resolve().parent
    littlefs = Path(args.littlefs).resolve()
    fatfs = Path(args.fatfs).resolve()
    required_littlefs = ("lfs.c", "lfs.h", "lfs_util.c", "lfs_util.h")
    required_fatfs = ("ff.c", "ff.h", "diskio.h", "ffunicode.c")
    for directory, names in ((littlefs, required_littlefs), (fatfs, required_fatfs)):
        for name in names:
            if not (directory / name).is_file():
                parser.error(f"Missing provider source: {directory / name}")
    test_build_root = here / "build"
    if args.sanitize:
        test_build_root /= "sanitizer-backends"
    destination = test_build_root / "providers" / "fatfs"
    destination.mkdir(parents=True, exist_ok=True)
    for name in required_fatfs:
        shutil.copyfile(fatfs / name, destination / name)
    shutil.copyfile(here.parent / "config" / "fatfs" / "ffconf.h", destination / "ffconf.h")
    environment = dict(os.environ, ESY_LITTLEFS_PATH=str(littlefs),
                       ESY_TEST_FATFS_PATH=str(destination))
    command = ["ceedling", "--ruby-replacement", "--mixin=mixins/backends.yml"]
    if args.sanitize:
        environment["ESY_FS_TEST_BUILD_ROOT"] = "build/sanitizer-backends"
        command.append("--mixin=sanitizer.yml")
    command.extend(args.tasks or ["test:all"])
    return subprocess.run(command, cwd=here, env=environment, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
