/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_hal/esy_hal_boot.h"
#include "esy_hal/esy_hal_fault.h"
#include "esy_middleware/bootloader/esy_bootloader.h"
#include "esy_middleware/ota/esy_ota.h"
#include "esy_middleware/ota/esy_ota_flash.h"
#include "esy_middleware/coredump/esy_coredump.h"
#include "esy_middleware/os/esy_os.h"
#include "esy_middleware/os/esy_os_fatal.h"
#include "esy_middleware/io_scheduler/esy_spi_scheduler.h"
#include "esy_middleware/io_scheduler/esy_uart_tx_scheduler_os.h"

void esy_native_probe_managed(void) {
  (void)esy_spi_scheduler_create(NULL, NULL, NULL, NULL);
  (void)esy_uart_tx_scheduler_os_create(NULL, NULL, NULL);
  (void)esy_uart_tx_scheduler_os_submit(NULL, NULL);
  (void)esy_uart_tx_scheduler_os_request_stop(NULL, false);
  (void)esy_uart_tx_scheduler_os_request_abort(NULL);
  (void)esy_uart_tx_scheduler_os_destroy(NULL);
  esy_bootloader_layout_t layout;
  esy_boot_runtime_t runtime;
  (void)esy_hal_boot_get_layout(&layout);
  (void)esy_hal_boot_get_runtime(&runtime);
  (void)esy_hal_boot_confirm_running_image();
  (void)esy_bootloader_init(NULL);
  (void)esy_ota_init(NULL, NULL);
  (void)esy_ota_flash_init(NULL, NULL);
  (void)esy_hal_fault_init(NULL);
  (void)esy_os_set_fatal_handler(esy_hal_fault_raise);
  (void)esy_coredump_init(NULL, NULL);
}
