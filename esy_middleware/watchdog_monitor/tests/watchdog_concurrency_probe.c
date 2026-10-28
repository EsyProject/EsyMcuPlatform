/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/watchdog_monitor/esy_watchdog_monitor.h"
#include "esy_watchdog_monitor_internal.h"
#include "fake_esy_os_task.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

#define WORKER_COUNT 4u
#define ITERATIONS 5000u

typedef struct {
  ESY_WATCHDOG_MONITOR_STATIC_STORAGE(storage, WORKER_COUNT, 512u);
  esy_watchdog_monitor_static_resources_t resources;
  esy_watchdog_monitor_ctx_t *monitor;
  atomic_bool go;
  unsigned context_a, context_b;
} concurrency_probe_ctx_t;
static concurrency_probe_ctx_t s_ctx;

static void callback_a(const esy_watchdog_alert_event_t *event, void *ctx) {
  assert(ctx == &s_ctx.context_a);
  assert(event->task_name[0] == 'w');
}
static void callback_b(const esy_watchdog_alert_event_t *event, void *ctx) {
  assert(ctx == &s_ctx.context_b);
  assert(event->task_name[0] == 'w');
}
static void await_start(void) { while (!atomic_load(&s_ctx.go)) {} }
static void *worker(void *arg) {
  (void)arg;
  await_start();
  for (unsigned i = 0; i < ITERATIONS; ++i) {
    esy_watchdog_report_t report;
    bool alive;
    char name[] = "worker transient name";
    assert(esy_watchdog_monitor_register(name, &report) == ESY_OK);
    name[0] = 'X';
    esy_watchdog_monitor_report(&report);
    esy_watchdog_monitor_disable_task_check(&report);
    assert(esy_watchdog_monitor_enable_task_check(&report) == ESY_OK);
    assert(esy_watchdog_monitor_get_task_status(&report, &alive) == ESY_OK);
    esy_watchdog_report_t stale = report;
    esy_watchdog_monitor_unregister(&report);
    assert(esy_watchdog_monitor_get_task_status(&stale, &alive) == ESY_ERR_INVALID_ARG);
    esy_watchdog_monitor_report(&stale);
  }
  return NULL;
}
static void *monitor(void *arg) {
  (void)arg;
  await_start();
  for (unsigned i = 0; i < ITERATIONS; ++i)
    esy_watchdog_monitor_check_once(s_ctx.monitor);
  return NULL;
}
static void *replace_callback(void *arg) {
  (void)arg;
  await_start();
  for (unsigned i = 0; i < ITERATIONS; ++i) {
    esy_err_t ret = (i & 1u)
        ? esy_watchdog_monitor_set_alert_cb(callback_a, &s_ctx.context_a)
        : esy_watchdog_monitor_set_alert_cb(callback_b, &s_ctx.context_b);
    assert(ret == ESY_OK || ret == ESY_ERR_BUSY);
  }
  return NULL;
}
int main(void) {
  esy_watchdog_monitor_config_t cfg;
  esy_watchdog_monitor_default_cfg(&cfg);
  ESY_WATCHDOG_MONITOR_BIND_STATIC_RESOURCES(&s_ctx.resources, s_ctx.storage);
  cfg.static_resources = &s_ctx.resources;
  assert(esy_watchdog_monitor_init(&cfg) == ESY_OK);
  assert(esy_watchdog_monitor_start() == ESY_OK);
  s_ctx.monitor = fake_esy_os_task_get_last_created()->arg;
  pthread_t workers[WORKER_COUNT], monitor_thread, callback_thread;
  for (unsigned i = 0; i < WORKER_COUNT; ++i)
    assert(pthread_create(&workers[i], NULL, worker, NULL) == 0);
  assert(pthread_create(&monitor_thread, NULL, monitor, NULL) == 0);
  assert(pthread_create(&callback_thread, NULL, replace_callback, NULL) == 0);
  atomic_store(&s_ctx.go, true);
  for (unsigned i = 0; i < WORKER_COUNT; ++i) assert(pthread_join(workers[i], NULL) == 0);
  assert(pthread_join(monitor_thread, NULL) == 0);
  assert(pthread_join(callback_thread, NULL) == 0);
  assert(esy_watchdog_monitor_get_task_count() == 0);
  assert(esy_watchdog_monitor_set_alert_cb(NULL, NULL) == ESY_OK);
  fake_esy_os_task_get_last_created()->parked = true;
  assert(esy_watchdog_monitor_deinit() == ESY_OK);
  return 0;
}
