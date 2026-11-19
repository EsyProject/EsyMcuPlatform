# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Check the PIO provider's ownership/native seams without backend emulation."""

from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "esy_hal_backend/src/pico/xio/program/esy_hb_xio_program.c"


def body(source, name):
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^)]*\)\s*\{", source)
    if match is None:
        raise AssertionError(f"Missing function: {name}")
    start = match.end()
    depth = 1
    for index in range(start, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if depth == 0:
            return source[start:index]
    raise AssertionError(f"Unterminated function: {name}")


class XioProgramContractTests(unittest.TestCase):
    def test_public_storage_and_config_have_matching_c_and_cpp_layout(self):
        source = '''#include <stdio.h>
#include "esy_hal/xio/esy_hal_xio_program.h"
int main(void) {
  esy_xio_program_t p = {0};
  esy_xio_program_sm_t sm = {0};
  printf("%zu %zu %zu %zu %zu", sizeof(p),sizeof(sm),
    sizeof(esy_xio_program_config_t),sizeof(esy_xio_program_sm_config_t),
    sizeof(esy_xio_program_dma_config_t));
  return 0;
}
'''
        layouts = []
        with tempfile.TemporaryDirectory(prefix="xio program headers ") as temp:
            directory = Path(temp)
            for extension, compiler, fallback, standard in (
                    ("c", "CC", "cc", "c11"),
                    ("cpp", "CXX", "c++", "c++17")):
                path = directory / f"header.{extension}"
                path.write_text(source)
                executable = directory / extension
                command = shlex.split(os.environ.get(compiler, fallback))
                command += [f"-std={standard}", "-Wall", "-Wextra", "-Werror",
                            "-Wno-missing-field-initializers",
                            f"-I{ROOT / 'esy_hal/include'}",
                            f"-I{ROOT / 'esy_common/defs/include'}",
                            str(path), "-o", str(executable)]
                result = subprocess.run(command, text=True, capture_output=True)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                layouts.append(subprocess.check_output([str(executable)], text=True))
        self.assertEqual(layouts[0], layouts[1])

    def test_group_has_one_native_synchronized_start_after_validation(self):
        source = SOURCE.read_text()
        group = body(source, "esy_hb_xio_program_exec_start_group")
        self.assertEqual(1, group.count("pio_enable_sm_mask_in_sync("))
        validation = group[:group.index("pio_enable_sm_mask_in_sync(")]
        self.assertIn("checked_sm(", validation)
        self.assertIn("native_enabled(", validation)
        self.assertIn("ESY_ERR_INVALID_ARG", validation)
        self.assertNotIn("pio_sm_clear_fifos", group)
        self.assertNotIn("pio_sm_restart", group)

    def test_reset_suppresses_injected_side_set_and_restores_mapping(self):
        reset = body(SOURCE.read_text(), "reset_sm")
        disable = reset.index("~PIO_SM0_PINCTRL_SIDESET_COUNT_BITS")
        self.assertLess(disable, reset.index("pio_sm_exec("))
        self.assertGreater(reset.index("pinctrl = saved_pinctrl;"),
                           reset.rindex("pio_sm_exec("))
        self.assertIn("pio_encode_mov(pio_isr, pio_null)", reset)
        self.assertIn("pio_encode_mov(pio_osr, pio_null)", reset)

    def test_init_and_reset_leave_osr_empty_before_first_autopull(self):
        source = SOURCE.read_text()
        reset = body(source, "reset_sm")
        # MOV OSR clears its shift count to zero (full). A final SM restart
        # restores the hardware empty count of 32 before any user OUT executes.
        restart = reset.rindex("pio_sm_restart(")
        self.assertGreater(
            restart, reset.rindex("pio_sm_exec("),
            "Reset must restore empty OSR state after all injected instructions",
        )
        for entry in ("esy_hb_xio_program_exec_sm_init",
                      "esy_hb_xio_program_exec_sm_reset"):
            with self.subTest(entry=entry):
                self.assertIn("reset_sm(runtime)", body(source, entry))

    def test_input_sync_is_established_and_dma_endpoints_stay_private(self):
        source = SOURCE.read_text()
        init = body(source, "esy_hb_xio_program_exec_sm_init")
        self.assertIn("hw_clear_bits(&pio->input_sync_bypass", init)
        dma = body(source, "start_dma")
        self.assertIn("pio_get_dreq(", dma)
        self.assertIn("ESY_DMA_TRANSFER_SIZE_32", dma)
        self.assertIn("pio->rxf[", dma)
        self.assertIn("pio->txf[", dma)
        abort = body(source, "esy_hb_xio_program_exec_dma_abort")
        self.assertRegex(abort, r"if\s*\(result\s*==\s*ESY_OK\)\s*memset\(dma,")
        for name in ("dma_complete", "dma_error"):
            callback = body(source, name)
            self.assertNotIn("memset", callback)
            self.assertNotIn("esy_hb_dma_abort", callback)
            self.assertNotIn("esy_hb_dma_release", callback)


if __name__ == "__main__":
    unittest.main()
