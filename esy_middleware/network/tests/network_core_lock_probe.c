/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

#include "esy_middleware/network/esy_network_lwip.h"
#include "fake_lwip.h"

#define CALLBACK_ITERATIONS 4000u
#define RECEIVE_BUFFER_BYTES 32u

typedef struct {
  pthread_mutex_t core_lock;
  esy_network_lwip_t endpoint;
  esy_network_stream_t stream;
  uint8_t rx_buffer[RECEIVE_BUFFER_BYTES];
  struct tcp_pcb *pcb;
  atomic_bool finished;
} network_core_lock_probe_ctx_t;

static network_core_lock_probe_ctx_t s_ctx = {
    .core_lock = PTHREAD_MUTEX_INITIALIZER,
};

static void enter_core(void *context) {
  network_core_lock_probe_ctx_t *ctx = context;
  assert(pthread_mutex_lock(&ctx->core_lock) == 0);
}

static void leave_core(void *context) {
  network_core_lock_probe_ctx_t *ctx = context;
  assert(pthread_mutex_unlock(&ctx->core_lock) == 0);
}

static void *native_callbacks(void *context) {
  network_core_lock_probe_ctx_t *ctx = context;
  for (unsigned i = 0u; i < CALLBACK_ITERATIONS; ++i) {
    enter_core(ctx);
    assert(fake_lwip_connect() == ERR_OK);
    assert(fake_lwip_fin(ctx->pcb) == ERR_OK);
    leave_core(ctx);
  }
  atomic_store_explicit(&ctx->finished, true, memory_order_release);
  return NULL;
}

int main(void) {
  fake_lwip_reset();
  const esy_network_lwip_static_resources_t resources = {
      .rx_buffer = s_ctx.rx_buffer,
      .rx_buffer_size = sizeof(s_ctx.rx_buffer),
  };
  const esy_network_lwip_config_t config = {
      .static_resources = &resources,
      .owner_enter = enter_core,
      .owner_leave = leave_core,
      .owner_context = &s_ctx,
  };
  assert(esy_network_lwip_init(&s_ctx.endpoint, &config) == ESY_OK);
  assert(esy_network_lwip_get_stream(&s_ctx.endpoint, &s_ctx.stream) == ESY_OK);
  assert(esy_network_lwip_begin(&s_ctx.endpoint, "192.0.2.1", 1883u) == ESY_OK);
  s_ctx.pcb = fake_lwip_last_pcb();
  pthread_t worker;
  assert(pthread_create(&worker, NULL, native_callbacks, &s_ctx) == 0);
  do {
    esy_network_state_t state = esy_network_lwip_get_state(&s_ctx.endpoint);
    assert(state == ESY_NETWORK_CONNECTING || state == ESY_NETWORK_CONNECTED ||
           state == ESY_NETWORK_CLOSED);
    assert(esy_network_lwip_listen(&s_ctx.endpoint, 1883u, 1u) == ESY_ERR_STATE);
  } while (!atomic_load_explicit(&s_ctx.finished, memory_order_acquire));
  assert(pthread_join(worker, NULL) == 0);
  assert(esy_network_lwip_get_state(&s_ctx.endpoint) == ESY_NETWORK_CLOSED);
  assert(esy_network_lwip_deinit(&s_ctx.endpoint) == ESY_OK);
  assert(fake_lwip_live_pcbs() == 0u);
  assert(pthread_mutex_destroy(&s_ctx.core_lock) == 0);
  return 0;
}
