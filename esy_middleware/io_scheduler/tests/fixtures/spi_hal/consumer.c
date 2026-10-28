/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "esy_middleware/io_scheduler/esy_spi_scheduler_core.h"

/* This consumer links the real public SPI wrapper and scheduler. The provider
 * models borrowing only; no HAL backend implementation is compiled or tested. */
typedef struct {
  const void *tx;
  void *rx;
  size_t length;
  esy_dma_transfer_size_t width;
  esy_spi_dma_complete_cb_t callback;
  void *callback_arg;
  esy_err_t submit_result;
  esy_err_t poll_result;
  esy_err_t abort_result;
  esy_err_t completed_result;
  uint8_t *completed_rx;
  unsigned submitted;
  unsigned aborted;
  unsigned completed;
  unsigned prepared;
  unsigned prepared_done;
  bool busy;
  bool retire;
} spi_hal_consumer_ctx_t;

static spi_hal_consumer_ctx_t s_ctx;

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "SPI HAL consumer assertion failed at line %u: %s\n", \
        (unsigned)__LINE__, #expression); \
    return 1; \
  } \
} while (0)

static bool provider_is_busy(esy_spi_t *spi) {
  (void)spi;
  return s_ctx.busy;
}

static esy_err_t provider_submit(esy_spi_t *spi, esy_dma_config_t *tx,
    esy_dma_config_t *rx, esy_spi_dma_complete_cb_t callback, void *arg) {
  (void)spi;
  s_ctx.tx = tx != NULL ? tx->src : NULL;
  s_ctx.rx = rx != NULL ? rx->dst : NULL;
  s_ctx.length = tx != NULL ? tx->length : rx->length;
  s_ctx.width = tx != NULL ? tx->transfer_size : rx->transfer_size;
  s_ctx.callback = callback;
  s_ctx.callback_arg = arg;
  s_ctx.busy = true;
  ++s_ctx.submitted;
  return s_ctx.submit_result;
}

static esy_err_t provider_poll(esy_spi_t *spi) {
  if (s_ctx.busy && s_ctx.retire) {
    esy_spi_dma_complete_cb_t callback = s_ctx.callback;
    void *arg = s_ctx.callback_arg;
    const esy_spi_dma_transfer_result_t result = {
        .status = s_ctx.submit_result == ESY_OK ? ESY_SPI_DMA_TRANSFER_OK
                                               : ESY_SPI_DMA_TRANSFER_DMA_ERROR,
        .tx_transferred = s_ctx.tx != NULL ? s_ctx.length : 0u,
        .rx_transferred = s_ctx.rx != NULL ? s_ctx.length : 0u,
    };
    s_ctx.busy = false;
    s_ctx.callback = NULL;
    s_ctx.tx = NULL;
    s_ctx.rx = NULL;
    callback(spi, &result, arg);
  }
  return s_ctx.poll_result;
}

static esy_err_t provider_abort(esy_spi_t *spi) {
  (void)spi;
  ++s_ctx.aborted;
  if (s_ctx.abort_result == ESY_OK) {
    s_ctx.busy = false;
    s_ctx.callback = NULL;
    s_ctx.tx = NULL;
    s_ctx.rx = NULL;
  }
  return s_ctx.abort_result;
}

static void completed(esy_err_t result, uint8_t *rx, void *arg) {
  (void)arg;
  s_ctx.completed_result = result;
  s_ctx.completed_rx = rx;
  ++s_ctx.completed;
}

static esy_err_t prepared(void *arg) {
  (void)arg;
  ++s_ctx.prepared;
  return ESY_OK;
}

static void prepared_done(esy_err_t result, void *arg) {
  (void)arg;
  s_ctx.completed_result = result;
  ++s_ctx.prepared_done;
}

static const esy_spi_vtable_t k_provider_ops = {
    .transfer_dma = provider_submit,
    .poll = provider_poll,
    .abort = provider_abort,
    .is_busy = provider_is_busy,
};

static int exercise(bool rx_only, esy_dma_transfer_size_t width,
    bool use_prepare, bool fail_submit, bool silent_abort) {
  esy_spi_scheduler_storage_t storage = {0};
  esy_spi_scheduler_request_t requests[2];
  esy_spi_scheduler_core_config_t cfg;
  esy_spi_scheduler_core_default_cfg(&cfg);
  cfg.block_count = 2u;
  cfg.static_resources.request_storage = requests;
  cfg.static_resources.request_storage_count = 2u;
  esy_spi_t spi = {0};
  spi.ops = &k_provider_ops;
  uint16_t data[4] = {0};
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.submit_result = fail_submit ? ESY_ERR_IO : ESY_OK;
  s_ctx.abort_result = ESY_ERR_IO;
  esy_spi_scheduler_t *scheduler = NULL;
  CHECK(esy_spi_scheduler_core_create(&spi, &cfg, &storage, &scheduler) == ESY_OK);
  if (use_prepare) {
    CHECK(esy_spi_scheduler_submit_prepared_with_arg((const uint8_t *)data,
        sizeof(data), prepared, prepared_done, NULL, scheduler) == ESY_OK);
  } else {
    esy_spi_scheduler_request_t *request = esy_spi_scheduler_get_write_block(scheduler);
    CHECK(request != NULL);
    request->tx_buf = rx_only ? NULL : (uint8_t *)data;
    request->rx_buf = rx_only ? (uint8_t *)data : NULL;
    request->len = sizeof(data);
    request->transfer_size = width;
    request->result_callback = completed;
    CHECK(esy_spi_scheduler_submit(scheduler, request) == ESY_OK);
  }
  CHECK(esy_spi_scheduler_core_process(scheduler) == ESY_OK);
  CHECK(s_ctx.submitted == 1u);
  CHECK(s_ctx.tx == (rx_only ? NULL : data));
  CHECK(s_ctx.rx == (rx_only ? data : NULL));
  CHECK(s_ctx.width == width && s_ctx.length == sizeof(data));
  CHECK(s_ctx.completed == 0u && s_ctx.prepared_done == 0u);
  CHECK(s_ctx.prepared == (use_prepare ? 1u : 0u));
  CHECK(esy_spi_scheduler_destroy(scheduler) == ESY_ERR_BUSY);
  if (fail_submit) {
    s_ctx.poll_result = ESY_ERR_BUSY;
    for (unsigned i = 0u; i < 2u; ++i) {
      CHECK(esy_spi_scheduler_core_process(scheduler) == ESY_OK);
      CHECK(s_ctx.completed == 0u && s_ctx.prepared_done == 0u);
      CHECK(s_ctx.busy && s_ctx.submitted == 1u);
      CHECK(s_ctx.tx == (rx_only ? NULL : data));
      CHECK(s_ctx.rx == (rx_only ? data : NULL));
      CHECK(esy_spi_scheduler_destroy(scheduler) == ESY_ERR_BUSY);
    }
    CHECK(s_ctx.aborted == 2u);
  }
  s_ctx.poll_result = ESY_OK;
  s_ctx.retire = !silent_abort;
  if (silent_abort) s_ctx.abort_result = ESY_OK;
  CHECK(esy_spi_scheduler_core_process(scheduler) == ESY_OK);
  CHECK(!s_ctx.busy && s_ctx.tx == NULL && s_ctx.rx == NULL);
  CHECK(s_ctx.completed_result == (fail_submit ? ESY_ERR_IO : ESY_OK));
  CHECK(s_ctx.completed == (use_prepare ? 0u : 1u));
  CHECK(s_ctx.prepared_done == (use_prepare ? 1u : 0u));
  if (!use_prepare) {
    CHECK(s_ctx.completed_rx == (!fail_submit && rx_only ? (uint8_t *)data : NULL));
  }
  CHECK(esy_spi_scheduler_core_process(scheduler) == ESY_OK);
  CHECK(s_ctx.completed + s_ctx.prepared_done == 1u);
  CHECK(esy_spi_scheduler_destroy(scheduler) == ESY_OK);
  CHECK(esy_spi_scheduler_core_create(&spi, &cfg, &storage, &scheduler) == ESY_OK);
  CHECK(esy_spi_scheduler_destroy(scheduler) == ESY_OK);
  return 0;
}

int main(void) {
  for (unsigned width = 0u; width < 2u; ++width) {
    for (unsigned rx = 0u; rx < 2u; ++rx) {
      CHECK(exercise(rx != 0u, width != 0u ? ESY_DMA_TRANSFER_SIZE_16
          : ESY_DMA_TRANSFER_SIZE_8, false, false, false) == 0);
      CHECK(exercise(rx != 0u, width != 0u ? ESY_DMA_TRANSFER_SIZE_16
          : ESY_DMA_TRANSFER_SIZE_8, false, true, false) == 0);
    }
  }
  CHECK(exercise(false, ESY_DMA_TRANSFER_SIZE_8, true, false, false) == 0);
  CHECK(exercise(false, ESY_DMA_TRANSFER_SIZE_8, true, true, false) == 0);
  CHECK(exercise(false, ESY_DMA_TRANSFER_SIZE_8, true, true, true) == 0);
  return 0;
}
