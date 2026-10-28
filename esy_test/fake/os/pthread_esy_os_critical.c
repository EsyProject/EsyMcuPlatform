/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Host-only non-nested cross-thread provider for tests of OS critical-section clients.
 * Link instead of the record-only fake endpoints; no native RTOS is involved. */
#include "esy_middleware/os/esy_os_task.h"
#include <assert.h>
#include <pthread.h>

typedef struct { pthread_mutex_t mutex; } pthread_esy_os_critical_ctx_t;
static pthread_esy_os_critical_ctx_t s_ctx = {PTHREAD_MUTEX_INITIALIZER};

void esy_os_task_enter_critical(void) {
  int result = pthread_mutex_lock(&s_ctx.mutex);
  assert(result == 0);
  (void)result;
}
void esy_os_task_exit_critical(void) {
  int result = pthread_mutex_unlock(&s_ctx.mutex);
  assert(result == 0);
  (void)result;
}
