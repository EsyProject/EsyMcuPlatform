/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_system.h"
#include "esy_middleware/os/esy_os.h"
#include "esy_middleware/os/esy_os_task.h"
#include "esy_middleware/os/esy_os_fatal.h"

void esy_native_probe_core(void) {
  uint8_t random_byte;
  esy_hal_system_init();
  (void)esy_hal_system_random_bytes(&random_byte, sizeof(random_byte));
  esy_os_init();
  (void)esy_os_task_get_tick();
  esy_os_task_set_core_affinity(NULL, 1u);
  esy_os_task_yield_core(1u);
  (void)esy_os_set_fatal_handler(NULL);
}
