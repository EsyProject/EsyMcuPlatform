# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

"""Exercise private HAL storage overlays under optimized strict aliasing.

The aggregate assignments below test the private representation only. They are
not examples of resetting public live handles or retiring borrowed resources.
"""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = r'''
#include <stdbool.h>
#include <stdio.h>
#include "esy_hal_adc_internal.h"
#include "esy_hal_gpio_internal.h"
#include "esy_hal_i2c_internal.h"
#include "esy_hal_pwm_internal.h"
#include "esy_hal_spi_internal.h"
#include "esy_hal_timer_capture_internal.h"
#include "esy_hal_uart_internal.h"
#include "esy_hal_usb_dev_internal.h"

/* Keep independently typed pointers visible to the optimizer. These objects
 * have no live device, callback, backend or DMA ownership. */
#define DEFINE_PROBE(name, handle_type, runtime_type, accessor, field) \
  static __attribute__((__noinline__)) bool assign_##name( \
      handle_type *handle, runtime_type *runtime, void *marker) { \
    runtime->field = marker; \
    *handle = (handle_type){.hal = {.bytes = {0}}}; \
    return runtime->field == NULL; \
  } \
  static int check_##name(void) { \
    handle_type handle = {.hal = {.bytes = {0}}}; \
    runtime_type *runtime = accessor(&handle); \
    if (!assign_##name(&handle, runtime, &handle)) { \
      puts(#name " retained a stale private field after aggregate assignment"); \
      return 1; \
    } \
    return 0; \
  }

static esy_uart_dma_runtime_t *uart_tx_runtime(esy_uart_t *handle) {
  return &esy_hal_uart_runtime(handle)->tx_dma;
}
static esy_uart_dma_runtime_t *uart_rx_runtime(esy_uart_t *handle) {
  return &esy_hal_uart_runtime(handle)->rx_dma;
}

DEFINE_PROBE(adc, esy_adc_t, esy_hal_adc_runtime_t,
             esy_hal_adc_runtime, device)
DEFINE_PROBE(gpio, esy_gpio_t, esy_hal_gpio_runtime_t,
             esy_hal_gpio_runtime, pm_owner)
DEFINE_PROBE(i2c, esy_i2c_t, esy_hal_i2c_runtime_t,
             esy_hal_i2c_runtime, device)
DEFINE_PROBE(pwm, esy_pwm_t, esy_hal_pwm_runtime_t,
             esy_hal_pwm_runtime, device)
DEFINE_PROBE(spi, esy_spi_t, esy_hal_spi_runtime_t,
             esy_hal_spi_runtime, device)
DEFINE_PROBE(capture, esy_timer_capture_t, esy_hal_timer_capture_runtime_t,
             esy_hal_timer_capture_runtime, device)
DEFINE_PROBE(uart, esy_uart_t, esy_hal_uart_runtime_t,
             esy_hal_uart_runtime, device)
DEFINE_PROBE(usb, esy_usb_dev_t, esy_hal_usb_runtime_t,
             esy_hal_usb_runtime, device)
DEFINE_PROBE(uart_tx_dma, esy_uart_t, esy_uart_dma_runtime_t,
             uart_tx_runtime, owner)
DEFINE_PROBE(uart_rx_dma, esy_uart_t, esy_uart_dma_runtime_t,
             uart_rx_runtime, owner)

int main(void) {
  int failures = 0;
  failures += check_adc();
  failures += check_gpio();
  failures += check_i2c();
  failures += check_pwm();
  failures += check_spi();
  failures += check_capture();
  failures += check_uart();
  failures += check_usb();
  failures += check_uart_tx_dma();
  failures += check_uart_rx_dma();
  return failures == 0 ? 0 : 1;
}
'''


class HalStorageAliasingTests(unittest.TestCase):
    def compile_and_run(self, compiler, flags):
        with tempfile.TemporaryDirectory(prefix="hal storage aliasing ") as tmp:
            directory = Path(tmp)
            source = directory / "probe.c"
            source.write_text(SOURCE)
            include_paths = [ROOT / "esy_hal/src", ROOT / "esy_hal/include",
                             ROOT / "esy_common/defs/include",
                             ROOT / "esy_common/device/include"]
            executable = directory / "probe"
            command = [*compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-fstrict-aliasing", *flags]
            command += [f"-I{path}" for path in include_paths]
            command += [str(source), "-o", str(executable)]
            compiled = subprocess.run(command, capture_output=True, text=True,
                                      timeout=60)
            self.assertEqual(0, compiled.returncode,
                             compiled.stdout + compiled.stderr)
            return subprocess.run([str(executable)], capture_output=True,
                                  text=True, timeout=10)

    def test_all_private_overlays_survive_optimized_aggregate_assignment(self):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        self.assertTrue(compiler, "CC must identify a C compiler")
        for flags in (("-O2",), ("-O3",), ("-O3", "-flto")):
            with self.subTest(compiler=compiler, flags=flags):
                result = self.compile_and_run(compiler, flags)
                self.assertEqual(0, result.returncode,
                                 result.stdout + result.stderr)

    @unittest.skipUnless(shutil.which("clang"), "Clang is not installed")
    def test_clang_private_overlays_with_strict_aliasing(self):
        for flags in (("-O2",), ("-O3",)):
            with self.subTest(flags=flags):
                result = self.compile_and_run(["clang"], flags)
                self.assertEqual(0, result.returncode,
                                 result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
