# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Keep a queued request alive across the signed 32-bit sequence-age boundary."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]


class PriorityFifoTurnoverTests(unittest.TestCase):
    def test_fifo_after_two_to_the_31_transactions(self):
        with tempfile.TemporaryDirectory(prefix="esy priority FIFO age ") as temporary:
            directory = Path(temporary)
            source = directory / "turnover.c"
            source.write_text(r'''
#include "esy_common/ds/esy_prio_slot_queue.h"
#include <stdint.h>

int main(void) {
  esy_prio_slot_queue_t queue;
  esy_prio_slot_t slots[2];
  const esy_prio_slot_queue_config_t cfg = {slots, 2u};
  if (esy_prio_slot_queue_init(&queue, &cfg) != ESY_OK ||
      esy_prio_slot_queue_enqueue(&queue, 0u, 7u) != ESY_OK) return 1;
  for (uint32_t i = 0; i < UINT32_C(0x80000000); ++i) {
    if (esy_prio_slot_queue_enqueue(&queue, 1u, 8u) != ESY_OK) return 2;
    esy_prio_slot_queue_mark_in_progress(&queue, 1u);
    esy_prio_slot_queue_free_slot(&queue, 1u);
  }
  if (esy_prio_slot_queue_enqueue(&queue, 1u, 7u) != ESY_OK) return 3;
  if (esy_prio_slot_queue_get_depth(&queue) != 2u ||
      esy_prio_slot_queue_pick_next(&queue) != 0) return 4;
  esy_prio_slot_queue_mark_in_progress(&queue, 0u);
  if (esy_prio_slot_queue_requeue(&queue, 0u, 7u) != ESY_OK ||
      esy_prio_slot_queue_pick_next(&queue) != 1) return 5;
  return 0;
}
''')
            executable = directory / "turnover"
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-O3", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "esy_common/ds/include"),
                "-I", str(ROOT / "esy_common/defs/include"), str(source),
                str(ROOT / "esy_common/ds/src/esy_prio_slot_queue.c"),
                "-o", str(executable),
            ], check=True, capture_output=True, text=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=180)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
