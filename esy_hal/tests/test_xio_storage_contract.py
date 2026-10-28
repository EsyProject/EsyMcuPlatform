# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Compile public XIO storage overrides and compare C/C++ handle layouts."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = r'''
#include <stddef.h>
#include <stdio.h>
#include "esy_hal_backend/esy_hb_xio.h"
#include "esy_hal/xio/esy_hal_xio_uart.h"
#include "esy_hal/xio/esy_hal_xio_spi.h"
#ifdef __cplusplus
#define CHECK static_assert
#define ALIGNOF alignof
#else
#define CHECK _Static_assert
#define ALIGNOF _Alignof
#endif
CHECK(ESY_XIO_PROG_PRIV_BYTES == EXPECTED_PROGRAM_BYTES,
      "Program storage override was discarded");
CHECK(ESY_XIO_SM_PRIV_BYTES == EXPECTED_SM_BYTES,
      "SM storage override was discarded");
CHECK(sizeof(((esy_hb_xio_program_t *)0)->backend.bytes) == EXPECTED_PROGRAM_BYTES,
      "Program backing storage has the wrong capacity");
CHECK(sizeof(((esy_hb_xio_sm_t *)0)->backend.bytes) == EXPECTED_SM_BYTES,
      "SM backing storage has the wrong capacity");
CHECK(sizeof(((esy_uart_t *)0)->backend.bytes) == EXPECTED_UART_BYTES,
      "Enclosing UART storage override was discarded");
CHECK(sizeof(((esy_spi_t *)0)->backend.bytes) == EXPECTED_SPI_BYTES,
      "Enclosing SPI storage override was discarded");
int main(void) {
  printf("%zu %zu %zu %zu %zu %zu %zu %zu %zu %zu\n",
      sizeof(esy_hb_xio_program_t), ALIGNOF(esy_hb_xio_program_t),
      offsetof(esy_hb_xio_program_t, initialized),
      sizeof(esy_hb_xio_sm_t), ALIGNOF(esy_hb_xio_sm_t),
      offsetof(esy_hb_xio_sm_t, claimed),
      sizeof(esy_uart_t), ALIGNOF(esy_uart_t),
      sizeof(esy_spi_t), ALIGNOF(esy_spi_t));
  return 0;
}
'''


class XioStorageContractTests(unittest.TestCase):
    def layouts(self, program_bytes=64, sm_bytes=64, uart_bytes=448,
                spi_bytes=256, overrides=()):
        layouts = []
        with tempfile.TemporaryDirectory(prefix="xio storage contract ") as tmp:
            directory = Path(tmp)
            for language, environment, fallback, standard in (
                    ("c", "CC", "cc", "c11"),
                    ("cpp", "CXX", "c++", "c++17")):
                source = directory / f"consumer.{language}"
                source.write_text(SOURCE)
                executable = directory / f"consumer_{language}"
                command = shlex.split(os.environ.get(environment, fallback))
                command += [f"-std={standard}", "-Wall", "-Wextra", "-Werror",
                            f"-I{ROOT / 'esy_hal_backend/include'}",
                            f"-I{ROOT / 'esy_hal/include'}",
                            f"-I{ROOT / 'esy_common/defs/include'}",
                            f"-DEXPECTED_PROGRAM_BYTES={program_bytes}",
                            f"-DEXPECTED_SM_BYTES={sm_bytes}",
                            f"-DEXPECTED_UART_BYTES={uart_bytes}",
                            f"-DEXPECTED_SPI_BYTES={spi_bytes}", *overrides,
                            str(source), "-o", str(executable)]
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                result = subprocess.run([str(executable)], capture_output=True, text=True)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                layouts.append(result.stdout)
        self.assertEqual(layouts[0], layouts[1], "C and C++ public layout differs")
        return layouts[0]

    def test_default_storage_has_matching_c_and_cpp_layout(self):
        self.layouts()

    def test_each_storage_override_is_preserved_independently(self):
        self.layouts(program_bytes=128, overrides=("-DESY_XIO_PROG_PRIV_BYTES=128u",))
        self.layouts(sm_bytes=96, overrides=("-DESY_XIO_SM_PRIV_BYTES=96u",))
        self.layouts(uart_bytes=1024, overrides=("-DESY_UART_PRIV_BYTES=1024u",))
        self.layouts(spi_bytes=1024, overrides=("-DESY_SPI_PRIV_BYTES=1024u",))

    def test_global_overrides_change_both_embedded_capacities_and_layout(self):
        defaults = self.layouts()
        enlarged = self.layouts(program_bytes=128, sm_bytes=96,
            uart_bytes=1024, spi_bytes=1024, overrides=(
                "-DESY_XIO_PROG_PRIV_BYTES=128u", "-DESY_XIO_SM_PRIV_BYTES=96u",
                "-DESY_UART_PRIV_BYTES=1024u", "-DESY_SPI_PRIV_BYTES=1024u"))
        self.assertNotEqual(defaults, enlarged)


if __name__ == "__main__":
    unittest.main()
