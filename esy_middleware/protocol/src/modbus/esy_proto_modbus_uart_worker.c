/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/protocol/esy_proto_modbus_uart_worker.h"

#include <stdatomic.h>
#include <string.h>

#include "esy_hal/esy_hal_system.h"

#define ESY_PROTO_MODBUS_UART_TASK_PERIOD_MS (1u)
#define ESY_PROTO_MODBUS_UART_DEFAULT_TASK_STACK_SIZE (1024u)
#define ESY_PROTO_MODBUS_UART_COMMAND_WAIT_MS (100u)
#define ESY_PROTO_MODBUS_UART_SHUTDOWN_WAIT_MS (100u)

enum {
  MODBUS_COMMAND_IDLE, MODBUS_COMMAND_WRITING, MODBUS_COMMAND_SUBMIT,
  MODBUS_COMMAND_ABORT, MODBUS_COMMAND_RUNNING, MODBUS_COMMAND_DONE
};

struct esy_proto_modbus_uart_master_worker_s {
  uint8_t allocated;
  atomic_bool ready;
  atomic_bool initialized;
  atomic_bool stopping;
  bool processing;
  esy_os_task_t *task;
  esy_proto_modbus_uart_master_channel_storage_t channel_storage;
  esy_proto_modbus_uart_master_channel_t *channel;
  esy_proto_modbus_uart_master_worker_complete_cb on_complete;
  void *user_ctx;
  /* External callers serialize through one command; only this owner uses channel. */
  atomic_int command;
  esy_proto_modbus_request_t command_request;
  bool command_abort_current;
  esy_err_t command_result;
  atomic_uint queued_snapshot;
  atomic_int state_snapshot;
};

struct esy_proto_modbus_uart_slave_worker_s {
  uint8_t allocated;
  uint8_t unit_id;
  atomic_bool ready;
  atomic_bool initialized;
  atomic_bool stopping;
  bool processing;
  esy_os_task_t *task;
  esy_proto_modbus_uart_slave_channel_storage_t channel_storage;
  esy_proto_modbus_uart_slave_channel_t *channel;
  esy_proto_modbus_uart_slave_worker_request_cb on_request;
  void *user_ctx;
};

_Static_assert(sizeof(esy_proto_modbus_uart_master_worker_t) <=
    ESY_PROTO_MODBUS_UART_MASTER_WORKER_STORAGE_BYTES, "Master worker storage too small");
_Static_assert(sizeof(esy_proto_modbus_uart_slave_worker_t) <=
    ESY_PROTO_MODBUS_UART_SLAVE_WORKER_STORAGE_BYTES, "Slave worker storage too small");

static bool stack_is_valid(const void *stack, size_t capacity, uint32_t required) {
  return stack && required != 0u && capacity >= required &&
      (uintptr_t)stack % ESY_OS_TASK_STACK_ALIGN == 0u;
}

/* The lifecycle owner stops API users before requesting cooperative shutdown. */
static esy_err_t delete_parked(esy_os_task_t *task) {
  if (!task) return ESY_OK;
  if (esy_os_task_is_current(task)) return ESY_ERR_STATE;
  (void)esy_os_task_notify_give(task);
  for (uint32_t waited = 0u; waited < ESY_PROTO_MODBUS_UART_SHUTDOWN_WAIT_MS; ++waited) {
    esy_err_t err = esy_os_task_delete_parked(task);
    if (err != ESY_ERR_BUSY) return err;
    esy_os_task_delay_ms(ESY_PROTO_MODBUS_UART_TASK_PERIOD_MS);
  }
  return esy_os_task_delete_parked(task);
}

static void master_snapshot(esy_proto_modbus_uart_master_worker_t *worker) {
  atomic_store(&worker->queued_snapshot,
      esy_proto_modbus_uart_master_channel_get_queue_depth(worker->channel));
  atomic_store(&worker->state_snapshot,
      esy_proto_modbus_uart_master_channel_get_state(worker->channel));
}

static void master_complete(esy_proto_modbus_uart_master_channel_t *channel,
    esy_proto_modbus_req_status_t status, uint8_t unit,
    esy_proto_modbus_func_t function, const uint8_t *response,
    uint16_t response_length, void *arg) {
  (void)channel;
  esy_proto_modbus_uart_master_worker_t *worker = arg;
  if (worker->on_complete)
    worker->on_complete(worker, status, unit, function, response,
        response_length, worker->user_ctx);
}

static esy_err_t slave_request(esy_proto_modbus_uart_slave_channel_t *channel,
    uint8_t unit, esy_proto_modbus_func_t function, const uint8_t *request,
    uint16_t request_length, uint8_t *response, uint16_t *response_length,
    uint16_t response_capacity, void *arg) {
  (void)channel;
  esy_proto_modbus_uart_slave_worker_t *worker = arg;
  return worker->on_request(worker, unit, function, request, request_length,
      response, response_length, response_capacity, worker->user_ctx);
}

static void master_process_command(esy_proto_modbus_uart_master_worker_t *worker) {
  int command = atomic_load_explicit(&worker->command, memory_order_acquire);
  if (command != MODBUS_COMMAND_SUBMIT && command != MODBUS_COMMAND_ABORT) return;
  int expected = command;
  if (!atomic_compare_exchange_strong(&worker->command, &expected, MODBUS_COMMAND_RUNNING)) return;
  worker->command_result = command == MODBUS_COMMAND_SUBMIT
      ? esy_proto_modbus_uart_master_channel_submit(worker->channel, &worker->command_request)
      : esy_proto_modbus_uart_master_channel_abort_all(worker->channel, worker->command_abort_current);
  master_snapshot(worker);
  atomic_store_explicit(&worker->command, MODBUS_COMMAND_DONE, memory_order_release);
}

static esy_err_t master_wait_command(esy_proto_modbus_uart_master_worker_t *worker,
    int command) {
  atomic_store_explicit(&worker->command, command, memory_order_release);
  for (uint32_t waited = 0u; ; ++waited) {
    if (atomic_load_explicit(&worker->command, memory_order_acquire) == MODBUS_COMMAND_DONE) {
      esy_err_t result = worker->command_result;
      atomic_store(&worker->command, MODBUS_COMMAND_IDLE);
      return result;
    }
    if (waited >= ESY_PROTO_MODBUS_UART_COMMAND_WAIT_MS) {
      int expected = command;
      if (atomic_compare_exchange_strong(&worker->command, &expected, MODBUS_COMMAND_IDLE))
        return ESY_ERR_BUSY;
    }
    /* Claimed commands finish on the owner; callbacks must return promptly. */
    esy_os_task_delay_ms(ESY_PROTO_MODBUS_UART_TASK_PERIOD_MS);
  }
}

static esy_err_t master_step(esy_proto_modbus_uart_master_worker_t *worker) {
  if (!worker || !worker->allocated || !atomic_load(&worker->ready) || worker->processing)
    return ESY_ERR_STATE;
  worker->processing = true;
  esy_err_t err;
  if (atomic_load(&worker->stopping)) {
    err = esy_proto_modbus_uart_master_channel_destroy(worker->channel);
    if (err == ESY_OK) {
      worker->channel = NULL;
      atomic_store(&worker->initialized, false);
    }
  } else {
    master_process_command(worker);
    err = esy_proto_modbus_uart_master_channel_poll(worker->channel);
  }
  master_snapshot(worker);
  worker->processing = false;
  return err;
}

static esy_err_t slave_step(esy_proto_modbus_uart_slave_worker_t *worker) {
  if (!worker || !worker->allocated || !atomic_load(&worker->ready) || worker->processing)
    return ESY_ERR_STATE;
  worker->processing = true;
  esy_err_t err;
  if (atomic_load(&worker->stopping)) {
    err = esy_proto_modbus_uart_slave_channel_destroy(worker->channel);
    if (err == ESY_OK) {
      worker->channel = NULL;
      atomic_store(&worker->initialized, false);
    }
  } else {
    err = esy_proto_modbus_uart_slave_channel_poll(worker->channel);
  }
  worker->processing = false;
  return err;
}

static void master_task(void *arg) {
  esy_proto_modbus_uart_master_worker_t *worker = arg;
  for (;;) {
    if (master_step(worker) == ESY_OK && !worker->channel && atomic_load(&worker->stopping)) {
      /* Do not access worker after parking: its owner may reclaim all storage. */
      for (;;) (void)esy_os_task_park();
    }
    esy_os_task_delay_ms(ESY_PROTO_MODBUS_UART_TASK_PERIOD_MS);
  }
}

static void slave_task(void *arg) {
  esy_proto_modbus_uart_slave_worker_t *worker = arg;
  for (;;) {
    if (slave_step(worker) == ESY_OK && !worker->channel && atomic_load(&worker->stopping)) {
      /* Do not access worker after parking: its owner may reclaim all storage. */
      for (;;) (void)esy_os_task_park();
    }
    esy_os_task_delay_ms(ESY_PROTO_MODBUS_UART_TASK_PERIOD_MS);
  }
}

esy_proto_modbus_uart_master_worker_config_t esy_proto_modbus_uart_master_worker_default_cfg(void) {
  esy_proto_modbus_uart_master_worker_config_t cfg = {0};
  cfg.frame_type = ESY_PROTO_MODBUS_FRAME_RTU;
  cfg.timeout_ms = 100u;
  cfg.rtu_frame_timeout_ms = 10u;
  cfg.broadcast_turnaround_ms = 100u;
  cfg.task_name = "esy_proto_modbus_uart_master_worker";
  return cfg;
}

esy_proto_modbus_uart_slave_worker_config_t esy_proto_modbus_uart_slave_worker_default_cfg(void) {
  esy_proto_modbus_uart_slave_worker_config_t cfg = {0};
  cfg.frame_type = ESY_PROTO_MODBUS_FRAME_RTU;
  cfg.unit_id = 1u;
  cfg.rtu_frame_timeout_ms = 10u;
  cfg.task_name = "esy_proto_modbus_uart_slave_worker";
  return cfg;
}

esy_err_t esy_proto_modbus_uart_master_worker_create(
    const esy_proto_modbus_uart_master_worker_config_t *cfg,
    esy_proto_modbus_uart_master_worker_storage_t *storage,
    esy_proto_modbus_uart_master_worker_t **out_worker) {
  if (!cfg || !storage || !out_worker || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  *out_worker = NULL;
  if (storage->bytes[0] != 0u) return ESY_ERR_STATE;
  if (!esy_os_task_park_supported()) return ESY_ERR_NOT_SUPPORTED;
  uint32_t stack_size = cfg->task_stack_size ? cfg->task_stack_size
      : ESY_PROTO_MODBUS_UART_DEFAULT_TASK_STACK_SIZE;
  if (!cfg->static_resources.task_control_storage ||
      !stack_is_valid(cfg->static_resources.task_stack_buffer,
          cfg->static_resources.task_stack_buffer_size, stack_size)) return ESY_ERR_INVALID_ARG;
  esy_proto_modbus_uart_master_worker_t *worker =
      (esy_proto_modbus_uart_master_worker_t *)storage->bytes;
  memset(worker, 0, sizeof(*worker));
  worker->allocated = 1u;
  atomic_init(&worker->ready, false);
  atomic_init(&worker->initialized, false);
  atomic_init(&worker->stopping, false);
  atomic_init(&worker->command, MODBUS_COMMAND_IDLE);
  atomic_init(&worker->queued_snapshot, 0u);
  atomic_init(&worker->state_snapshot, ESY_PROTO_MODBUS_MASTER_STATE_IDLE);
  worker->on_complete = cfg->on_complete;
  worker->user_ctx = cfg->user_ctx;
  const esy_proto_modbus_uart_master_channel_config_t channel_cfg = {
    .frame_type = cfg->frame_type, .uart = cfg->uart,
    .timeout_ms = cfg->timeout_ms, .retry_max = cfg->retry_max,
    .rtu_frame_timeout_ms = cfg->rtu_frame_timeout_ms,
    .broadcast_turnaround_ms = cfg->broadcast_turnaround_ms,
    .queue_depth = cfg->queue_depth,
    .on_complete = master_complete, .user_ctx = worker,
    .static_resources = {
      .slot_storage = cfg->static_resources.slot_storage,
      .slot_storage_count = cfg->static_resources.slot_storage_count,
      .payload_storage = cfg->static_resources.payload_storage,
      .payload_storage_count = cfg->static_resources.payload_storage_count,
    },
  };
  esy_err_t err = esy_proto_modbus_uart_master_channel_create(&channel_cfg,
      &worker->channel_storage, &worker->channel);
  if (err != ESY_OK) {
    if (worker->channel) {
      atomic_store(&worker->stopping, true);
      *out_worker = worker;
    } else {
      worker->allocated = 0u;
    }
    return err;
  }
  err = esy_os_task_create_static(cfg->task_name ? cfg->task_name : "esy_proto_modbus_uart_master_worker",
      master_task, worker, stack_size, cfg->task_priority,
      cfg->static_resources.task_stack_buffer, cfg->static_resources.task_control_storage, &worker->task);
  if (err != ESY_OK) {
    if (esy_proto_modbus_uart_master_worker_destroy(worker) != ESY_OK) *out_worker = worker;
    return ESY_ERR_FAIL;
  }
  atomic_store(&worker->initialized, true);
  atomic_store(&worker->ready, true);
  *out_worker = worker;
  return ESY_OK;
}

esy_err_t esy_proto_modbus_uart_slave_worker_create(
    const esy_proto_modbus_uart_slave_worker_config_t *cfg,
    esy_proto_modbus_uart_slave_worker_storage_t *storage,
    esy_proto_modbus_uart_slave_worker_t **out_worker) {
  if (!cfg || !storage || !out_worker || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  *out_worker = NULL;
  if (storage->bytes[0] != 0u) return ESY_ERR_STATE;
  if (!esy_os_task_park_supported()) return ESY_ERR_NOT_SUPPORTED;
  uint32_t stack_size = cfg->task_stack_size ? cfg->task_stack_size
      : ESY_PROTO_MODBUS_UART_DEFAULT_TASK_STACK_SIZE;
  if (!cfg->static_resources.task_control_storage || !cfg->on_request ||
      !stack_is_valid(cfg->static_resources.task_stack_buffer,
          cfg->static_resources.task_stack_buffer_size, stack_size)) return ESY_ERR_INVALID_ARG;
  esy_proto_modbus_uart_slave_worker_t *worker =
      (esy_proto_modbus_uart_slave_worker_t *)storage->bytes;
  memset(worker, 0, sizeof(*worker));
  worker->allocated = 1u;
  worker->unit_id = cfg->unit_id;
  atomic_init(&worker->ready, false);
  atomic_init(&worker->initialized, false);
  atomic_init(&worker->stopping, false);
  worker->on_request = cfg->on_request;
  worker->user_ctx = cfg->user_ctx;
  const esy_proto_modbus_uart_slave_channel_config_t channel_cfg = {
    .frame_type = cfg->frame_type, .uart = cfg->uart, .unit_id = cfg->unit_id,
    .rtu_frame_timeout_ms = cfg->rtu_frame_timeout_ms,
    .on_request = slave_request, .user_ctx = worker,
  };
  esy_err_t err = esy_proto_modbus_uart_slave_channel_create(&channel_cfg,
      &worker->channel_storage, &worker->channel);
  if (err != ESY_OK) {
    if (worker->channel) {
      atomic_store(&worker->stopping, true);
      *out_worker = worker;
    } else {
      worker->allocated = 0u;
    }
    return err;
  }
  err = esy_os_task_create_static(cfg->task_name ? cfg->task_name : "esy_proto_modbus_uart_slave_worker",
      slave_task, worker, stack_size, cfg->task_priority,
      cfg->static_resources.task_stack_buffer, cfg->static_resources.task_control_storage, &worker->task);
  if (err != ESY_OK) {
    if (esy_proto_modbus_uart_slave_worker_destroy(worker) != ESY_OK) *out_worker = worker;
    return ESY_ERR_FAIL;
  }
  atomic_store(&worker->initialized, true);
  atomic_store(&worker->ready, true);
  *out_worker = worker;
  return ESY_OK;
}

esy_err_t esy_proto_modbus_uart_master_worker_destroy(esy_proto_modbus_uart_master_worker_t *worker) {
  if (esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if (!worker || !worker->allocated) return ESY_OK;
  if (worker->task ? esy_os_task_is_current(worker->task) : worker->processing)
    return ESY_ERR_STATE;
  atomic_store(&worker->stopping, true);
  esy_err_t err;
  if (worker->task) {
    err = delete_parked(worker->task);
  } else {
    worker->processing = true;
    err = esy_proto_modbus_uart_master_channel_destroy(worker->channel);
    worker->processing = false;
  }
  if (err != ESY_OK) return err;
  worker->channel = NULL;
  worker->task = NULL;
  atomic_store(&worker->initialized, false);
  worker->allocated = 0u;
  return ESY_OK;
}

esy_err_t esy_proto_modbus_uart_slave_worker_destroy(esy_proto_modbus_uart_slave_worker_t *worker) {
  if (esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if (!worker || !worker->allocated) return ESY_OK;
  if (worker->task ? esy_os_task_is_current(worker->task) : worker->processing)
    return ESY_ERR_STATE;
  atomic_store(&worker->stopping, true);
  esy_err_t err;
  if (worker->task) {
    err = delete_parked(worker->task);
  } else {
    worker->processing = true;
    err = esy_proto_modbus_uart_slave_channel_destroy(worker->channel);
    worker->processing = false;
  }
  if (err != ESY_OK) return err;
  worker->channel = NULL;
  worker->task = NULL;
  atomic_store(&worker->initialized, false);
  worker->allocated = 0u;
  return ESY_OK;
}

bool esy_proto_modbus_uart_master_worker_is_initialized(const esy_proto_modbus_uart_master_worker_t *worker) {
  return worker && atomic_load(&worker->initialized);
}

bool esy_proto_modbus_uart_slave_worker_is_initialized(const esy_proto_modbus_uart_slave_worker_t *worker) {
  return worker && atomic_load(&worker->initialized);
}

uint16_t esy_proto_modbus_uart_master_worker_get_queue_depth(const esy_proto_modbus_uart_master_worker_t *worker) {
  return esy_proto_modbus_uart_master_worker_is_initialized(worker)
      ? (uint16_t)atomic_load(&worker->queued_snapshot) : 0u;
}

esy_proto_modbus_master_state_t esy_proto_modbus_uart_master_worker_get_state(
    const esy_proto_modbus_uart_master_worker_t *worker) {
  return esy_proto_modbus_uart_master_worker_is_initialized(worker)
      ? (esy_proto_modbus_master_state_t)atomic_load(&worker->state_snapshot)
      : ESY_PROTO_MODBUS_MASTER_STATE_IDLE;
}

uint8_t esy_proto_modbus_uart_slave_worker_get_addr(const esy_proto_modbus_uart_slave_worker_t *worker) {
  return esy_proto_modbus_uart_slave_worker_is_initialized(worker) ? worker->unit_id : 0u;
}

esy_err_t esy_proto_modbus_uart_master_worker_submit(esy_proto_modbus_uart_master_worker_t *worker,
    const esy_proto_modbus_request_t *request) {
  if (!worker || !request || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if (!atomic_load(&worker->initialized) || atomic_load(&worker->stopping)) return ESY_ERR_STATE;
  if (request->on_complete || request->user_ctx) return ESY_ERR_INVALID_ARG;
  if (esy_os_task_is_current(worker->task)) {
    esy_err_t err = esy_proto_modbus_uart_master_channel_submit(worker->channel, request);
    master_snapshot(worker);
    return err;
  }
  int expected = MODBUS_COMMAND_IDLE;
  if (!atomic_compare_exchange_strong(&worker->command, &expected, MODBUS_COMMAND_WRITING))
    return ESY_ERR_BUSY;
  worker->command_request = *request;
  return master_wait_command(worker, MODBUS_COMMAND_SUBMIT);
}

esy_err_t esy_proto_modbus_uart_master_worker_abort_all(esy_proto_modbus_uart_master_worker_t *worker,
    bool abort_current) {
  if (!worker || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if (!atomic_load(&worker->initialized) || atomic_load(&worker->stopping)) return ESY_ERR_STATE;
  if (esy_os_task_is_current(worker->task)) {
    esy_err_t err = esy_proto_modbus_uart_master_channel_abort_all(worker->channel, abort_current);
    master_snapshot(worker);
    return err;
  }
  int expected = MODBUS_COMMAND_IDLE;
  if (!atomic_compare_exchange_strong(&worker->command, &expected, MODBUS_COMMAND_WRITING))
    return ESY_ERR_BUSY;
  worker->command_abort_current = abort_current;
  return master_wait_command(worker, MODBUS_COMMAND_ABORT);
}

#ifdef TEST
void esy_proto_modbus_uart_master_worker_test_iteration(esy_proto_modbus_uart_master_worker_t *worker) {
  if (master_step(worker) == ESY_OK && worker && !worker->channel && atomic_load(&worker->stopping))
    (void)esy_os_task_park();
  else esy_os_task_delay_ms(ESY_PROTO_MODBUS_UART_TASK_PERIOD_MS);
}
void esy_proto_modbus_uart_master_worker_test_control(esy_proto_modbus_uart_master_worker_t *worker) {
  if (atomic_load(&worker->stopping)) esy_proto_modbus_uart_master_worker_test_iteration(worker);
  else master_process_command(worker);
}
void esy_proto_modbus_uart_slave_worker_test_iteration(esy_proto_modbus_uart_slave_worker_t *worker) {
  if (slave_step(worker) == ESY_OK && worker && !worker->channel && atomic_load(&worker->stopping))
    (void)esy_os_task_park();
  else esy_os_task_delay_ms(ESY_PROTO_MODBUS_UART_TASK_PERIOD_MS);
}
void esy_proto_modbus_uart_slave_worker_test_control(esy_proto_modbus_uart_slave_worker_t *worker) {
  if (atomic_load(&worker->stopping)) esy_proto_modbus_uart_slave_worker_test_iteration(worker);
}
#endif
