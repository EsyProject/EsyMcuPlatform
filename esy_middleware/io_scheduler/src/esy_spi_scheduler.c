/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/io_scheduler/esy_spi_scheduler.h"
#include <string.h>
#include "esy_spi_scheduler_internal.h"
#include "esy_io_scheduler_task_resource_internal.h"
#include "esy_hal/esy_hal_system.h"
#define ESY_SPI_SCHEDULER_DEFAULT_TASK_STACK_SIZE 512u

static bool is_worker(void *task) {
  return esy_os_task_is_current(task);
}

static esy_err_t wake(void *task) {
  return esy_os_task_notify_give(task);
}

static esy_err_t stop_worker(void *task) {
  return esy_io_scheduler_delete_parked(task);
}

static void spi_scheduler_task(void *arg) {
  esy_spi_scheduler_t *gw = arg;
  for (;;) {
    (void)esy_os_task_notify_take(true,
        esy_io_scheduler_atomic_flag_get(&gw->busy) ||
        esy_io_scheduler_atomic_flag_get(&gw->stopping) ||
        !esy_io_scheduler_atomic_flag_get(&gw->ready) ? 1u : UINT32_MAX);
    esy_spi_scheduler_process_context(gw);
    if (esy_io_scheduler_atomic_flag_get(&gw->stopping) && !gw->current &&
        !esy_hal_spi_is_busy(gw->spi)) {
      /* The owner may reclaim gw after parking; never access it on resume. */
      for (;;) (void)esy_os_task_park();
    }
  }
}

void esy_spi_scheduler_default_cfg(esy_spi_scheduler_config_t *cfg) {
  if (!cfg) return;
  memset(cfg, 0, sizeof(*cfg));
  cfg->block_count = 4u;
  cfg->task_stack_size = ESY_SPI_SCHEDULER_DEFAULT_TASK_STACK_SIZE;
  cfg->task_priority = 1u;
  cfg->task_core_affinity = ESY_CORE_ALL;
}

esy_err_t esy_spi_scheduler_create(esy_spi_t *spi,
    const esy_spi_scheduler_config_t *cfg, esy_spi_scheduler_storage_t *storage,
    esy_spi_scheduler_t **out_gw) {
  if (!spi || !cfg || !storage || !out_gw || esy_hal_system_is_in_isr())
    return ESY_ERR_INVALID_ARG;
  *out_gw = NULL;
  uint32_t stack_size = cfg->task_stack_size ? cfg->task_stack_size
      : ESY_SPI_SCHEDULER_DEFAULT_TASK_STACK_SIZE;
  if (!cfg->static_resources.task_control_storage ||
      !esy_io_scheduler_task_stack_resource_is_valid(cfg->static_resources.task_stack_buffer,
          cfg->static_resources.task_stack_buffer_size, stack_size)) return ESY_ERR_INVALID_ARG;
  if (!esy_os_task_park_supported()) return ESY_ERR_NOT_SUPPORTED;
  esy_spi_scheduler_core_config_t core_cfg = {
    .tx_alloc_size = cfg->tx_alloc_size, .rx_alloc_size = cfg->rx_alloc_size,
    .block_count = cfg->block_count,
    .static_resources = {
      .request_storage = cfg->static_resources.request_storage,
      .request_storage_count = cfg->static_resources.request_storage_count,
      .tx_buffer_storage = cfg->static_resources.tx_buffer_storage,
      .tx_buffer_storage_size = cfg->static_resources.tx_buffer_storage_size,
      .rx_buffer_storage = cfg->static_resources.rx_buffer_storage,
      .rx_buffer_storage_size = cfg->static_resources.rx_buffer_storage_size,
    },
  };
  esy_spi_scheduler_t *gw = NULL;
  esy_err_t err = esy_spi_scheduler_core_create(spi, &core_cfg, storage, &gw);
  if (err != ESY_OK) return err;
  /* Publish scheduling hooks before the worker can run; producers start only
   * after create returns. ready gates worker access until its handle is stored. */
  esy_io_scheduler_atomic_flag_set(&gw->ready, false);
  gw->is_worker = is_worker;
  gw->wake = wake;
  gw->stop_worker = stop_worker;
  esy_os_task_t *task = NULL;
  err = esy_os_task_create_static(cfg->task_name, spi_scheduler_task, gw,
      stack_size, cfg->task_priority, cfg->static_resources.task_stack_buffer,
      cfg->static_resources.task_control_storage, &task);
  if (err != ESY_OK) {
    gw->wake = NULL;
    gw->stop_worker = NULL;
    (void)esy_spi_scheduler_destroy(gw);
    return err;
  }
  gw->task = task;
  esy_os_task_set_core_affinity(task, cfg->task_core_affinity);
  esy_io_scheduler_atomic_flag_set(&gw->ready, true);
  *out_gw = gw;
  return ESY_OK;
}

#ifdef TEST
void esy_spi_scheduler_test_iteration(esy_spi_scheduler_t *gw) {
  esy_spi_scheduler_process_context(gw);
  if (gw && esy_io_scheduler_atomic_flag_get(&gw->stopping) && !gw->current &&
      !esy_hal_spi_is_busy(gw->spi)) (void)esy_os_task_park();
}
#endif
