# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Compile the real STM32F4 handoff and check its native stack-switch assembly.

Set ESY_TEST_ARM_TOOLCHAIN_ROOT to the Arm GNU toolchain root (containing bin)
and ESY_TEST_STM32_SUPPORT_ROOT to native CMSIS/STM32F4xx_HAL_Driver providers.
The provider's HAL configuration template supplies the test configuration; no
hardware API or CMSIS implementation is replaced with a host stub.
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class Stm32HandoffNativeTests(unittest.TestCase):
    @unittest.skipUnless(
        os.environ.get("ESY_TEST_ARM_TOOLCHAIN_ROOT") and
        os.environ.get("ESY_TEST_STM32_SUPPORT_ROOT"),
        "Set ESY_TEST_ARM_TOOLCHAIN_ROOT and ESY_TEST_STM32_SUPPORT_ROOT "
        "for native STM32F4 handoff verification")
    def test_handoff_is_stack_free_at_each_supported_optimization(self):
        toolchain = Path(os.environ["ESY_TEST_ARM_TOOLCHAIN_ROOT"]).resolve()
        support = Path(os.environ["ESY_TEST_STM32_SUPPORT_ROOT"]).resolve()
        compiler = toolchain / "bin/arm-none-eabi-gcc"
        objdump = toolchain / "bin/arm-none-eabi-objdump"
        template = support / "STM32F4xx_HAL_Driver/Inc/stm32f4xx_hal_conf_template.h"
        for required in (compiler, objdump, template,
                         support / "CMSIS/Include/core_cm4.h",
                         support / "CMSIS/Device/ST/STM32F4xx/Include/stm32f407xx.h"):
            self.assertTrue(required.is_file(), f"Missing native provider: {required}")

        with tempfile.TemporaryDirectory(prefix="stm32 native handoff ") as tmp:
            directory = Path(tmp)
            shutil.copyfile(template, directory / "stm32f4xx_hal_conf.h")
            command = [str(compiler), "-std=c11", "-mcpu=cortex-m4", "-mthumb",
                       "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard", "-DSTM32F407xx",
                       "-Wall", "-Wextra", "-Werror"]
            for include in (ROOT / "esy_hal_backend/include",
                            ROOT / "esy_common/defs/include", directory,
                            support / "STM32F4xx_HAL_Driver/Inc",
                            support / "CMSIS/Device/ST/STM32F4xx/Include",
                            support / "CMSIS/Include"):
                command.extend(["-I", str(include)])
            source = ROOT / "esy_hal_backend/src/stm32f4/esy_hb_boot.c"
            assemblies = []
            for name, flags in (("O0-omit-fp", ("-O0", "-fomit-frame-pointer")),
                                ("Og", ("-Og",)), ("O2", ("-O2",))):
                with self.subTest(optimization=name):
                    assembly = directory / f"{name}.s"
                    obj = directory / f"{name}.o"
                    for mode, output in (("-S", assembly), ("-c", obj)):
                        self.run_native(command + list(flags) +
                                        [mode, str(source), "-o", str(output)])
                    disassembly = self.run_native([str(objdump), "-dr", str(obj)])
                    self.assertIn("<enter_application>:", disassembly)
                    (directory / f"{name}.disassembly.txt").write_text(disassembly)
                    assemblies.append(str(assembly))
            self.run_native([sys.executable, "-B",
                             str(ROOT / "esy_hal/tests/check_stm32_handoff_assembly.py"),
                             *assemblies])

    def run_native(self, command):
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        self.assertEqual(0, result.returncode,
                         f"Native command failed: {command}\n"
                         f"{result.stdout}{result.stderr}")
        return result.stdout


if __name__ == "__main__":
    unittest.main()
