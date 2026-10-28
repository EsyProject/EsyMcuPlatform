# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Run adapter contracts with explicit BTstack headers and native ATT regressions."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


BTSTACK_PATH = os.environ.get("ESY_BTSTACK_PATH")
SANITIZE = False


class BTstackRuntimeTest(unittest.TestCase):
    def test_provider_runtime_contract(self):
        self.run_runtime_contract(native_att=False)

    def test_native_att_runtime_contract(self):
        self.run_runtime_contract(native_att=True)

    def run_runtime_contract(self, *, native_att):
        if not BTSTACK_PATH:
            self.skipTest("Set ESY_BTSTACK_PATH or --btstack-path to test the adapter")

        module = Path(__file__).resolve().parents[1]
        platform = module.parents[1]
        btstack = Path(BTSTACK_PATH).resolve()
        self.assertTrue((btstack / "src/btstack.h").is_file(), "BTstack source path is invalid")
        with tempfile.TemporaryDirectory(prefix="esy-ble-btstack-runtime-") as directory:
            executable = Path(directory) / "test_btstack_runtime"
            command = [
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            ]
            if native_att:
                command.extend([
                    "-DESY_BLE_TEST_NATIVE_ATT=1",
                    str(btstack / "src/ble/att_db.c"),
                ])
            if SANITIZE:
                command.extend([
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
                ])
            for path in (
                module / "tests/btstack", module / "include",
                platform / "esy_common/defs/include", btstack / "src",
                btstack / "src/ble", btstack / "platform/embedded",
            ):
                command.extend(["-I", str(path)])
            command.extend([
                str(module / "src/esy_ble.c"),
                str(module / "src/esy_ble_btstack.c"),
                str(module / "tests/btstack/test_btstack_runtime.c"),
                str(btstack / "src/btstack_util.c"),
                "-o", str(executable),
            ])
            compiled = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            completed = subprocess.run([str(executable)], capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            print(completed.stdout, end="")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--btstack-path", default=BTSTACK_PATH)
    parser.add_argument("--sanitize", action="store_true")
    options, remaining = parser.parse_known_args()
    BTSTACK_PATH = options.btstack_path
    SANITIZE = options.sanitize
    unittest.main(argv=[__file__, *remaining])
