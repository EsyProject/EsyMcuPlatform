/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_spi_scheduler_internal.h"

#include <string.h>

#include "esy_hal/esy_hal_spi.h"
#include "esy_hal/esy_hal_dma.h"
#include "esy_hal/esy_hal_system.h"

#define ESY_SPI_SCHEDULER_DMA_THRESHOLD_BYTES (sizeof(uint32_t))
#define ESY_SPI_SCHEDULER_DEFAULT_BLOCK_COUNT (4u)

_Static_assert(sizeof(esy_spi_scheduler_t) <= ESY_SPI_SCHEDULER_STORAGE_BYTES,
               "ESY_SPI_SCHEDULER_STORAGE_BYTES is too small");

_Static_assert(_Alignof(esy_spi_scheduler_storage_t) >= _Alignof(esy_spi_scheduler_t),
               "SPI scheduler storage alignment is too small");

static void try_start_next_transfer(esy_spi_scheduler_t *gw);

static void esy_spi_scheduler_on_dma_done(
    esy_spi_t *spi, const esy_spi_dma_transfer_result_t *result,
    void *arg);
static esy_err_t release_scheduler(esy_spi_scheduler_t *gw);

static bool scheduler_has_request(const esy_spi_scheduler_t *gw,
                                const esy_spi_scheduler_request_t *req) {
  if (!gw || !gw->req_buf || !req || gw->block_count == 0u) {
    return false;
  }

  const uintptr_t begin = (uintptr_t)gw->req_buf;
  const uintptr_t end =
      begin + ((uintptr_t)gw->block_count * sizeof(esy_spi_scheduler_request_t));
  const uintptr_t ptr = (uintptr_t)req;

  return ptr >= begin && ptr < end &&
         (((ptr - begin) % sizeof(esy_spi_scheduler_request_t)) == 0u);
}

static void reset_request(esy_spi_scheduler_request_t *req) {
  uint8_t *tx_alloc_buf = req->tx_alloc_buf;
  uint8_t *rx_alloc_buf = req->rx_alloc_buf;

  memset(req, 0, sizeof(*req));
  req->tx_alloc_buf = tx_alloc_buf;
  req->rx_alloc_buf = rx_alloc_buf;
  req->tx_buf = tx_alloc_buf;
  req->rx_buf = rx_alloc_buf;
  req->transfer_size = ESY_DMA_TRANSFER_SIZE_8;
  req->result = ESY_ERR_STATE;
}

static void finish_current_request(esy_spi_scheduler_t *gw) {
  if (!gw || !gw->current) {
    return;
  }

  if (gw->current->prepare) {
    gw->current->prepared_done(gw->current->result, gw->current->prepared_arg);
  }
  if (gw->current->result_callback)
    gw->current->result_callback(gw->current->result,
        gw->current->result == ESY_OK ? gw->current->rx_buf : NULL, gw->current->result_callback_arg);

  esy_block_ring_commit_read(&gw->req_ring);
  gw->current = NULL;
  gw->failed_submit = false;
  atomic_fetch_sub_explicit(&gw->pending_count, 1u, memory_order_release);
  esy_io_scheduler_atomic_flag_set(&gw->busy, false);
}

static esy_err_t stop_scheduler(esy_spi_scheduler_t *gw) {
  if (!gw) return ESY_OK;
  if (gw->is_worker && gw->is_worker(gw->task)) return ESY_ERR_STATE;
  if (!gw->task && gw->processing) return ESY_ERR_STATE;
  if (atomic_load_explicit(&gw->pending_count, memory_order_acquire) != 0u) {
    return ESY_ERR_BUSY;
  }
  esy_io_scheduler_atomic_flag_set(&gw->stopping, true);
  if (gw->stop_worker) return gw->stop_worker(gw->task);
  return gw->current || esy_hal_spi_is_busy(gw->spi) ? ESY_ERR_BUSY : ESY_OK;
}

void esy_spi_scheduler_core_default_cfg(esy_spi_scheduler_core_config_t *cfg) {
  if (!cfg) {
    return;
  }

  memset(cfg, 0, sizeof(*cfg));
  cfg->block_count = ESY_SPI_SCHEDULER_DEFAULT_BLOCK_COUNT;
}

static esy_err_t init_scheduler_storage(esy_spi_scheduler_t *gw,
                                      esy_spi_t *spi,
                                      const esy_spi_scheduler_core_config_t *config) {
  if (!gw || !spi || !config || config->block_count < 2u ||
      (config->block_count & (config->block_count - 1u)) != 0u ||
      SIZE_MAX / config->block_count < sizeof(esy_spi_scheduler_request_t)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!config->static_resources.request_storage ||
      config->static_resources.request_storage_count < config->block_count) {
    return ESY_ERR_INVALID_ARG;
  }
  if (config->tx_alloc_size > 0u &&
      (!config->static_resources.tx_buffer_storage ||
       config->tx_alloc_size > config->static_resources.tx_buffer_storage_size / config->block_count)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (config->rx_alloc_size > 0u &&
      (!config->static_resources.rx_buffer_storage ||
       config->rx_alloc_size > config->static_resources.rx_buffer_storage_size / config->block_count)) {
    return ESY_ERR_INVALID_ARG;
  }
  gw->spi = spi;
  esy_io_scheduler_atomic_flag_init(&gw->ready, false);
  esy_io_scheduler_atomic_flag_init(&gw->dma_done, false);
  atomic_init(&gw->pending_count, 0u);
  esy_io_scheduler_atomic_flag_init(&gw->busy, false);
  esy_io_scheduler_atomic_flag_init(&gw->stopping, false);
  esy_io_scheduler_atomic_flag_init(&gw->quiesced, false);
  gw->tx_alloc_size = config->tx_alloc_size;
  gw->rx_alloc_size = config->rx_alloc_size;
  gw->block_count = config->block_count;
  gw->req_buf = config->static_resources.request_storage;
  memset(gw->req_buf, 0, sizeof(esy_spi_scheduler_request_t) * gw->block_count);

  for (uint32_t i = 0; i < gw->block_count; ++i) {
    esy_spi_scheduler_request_t *req = &gw->req_buf[i];
    reset_request(req);
    if (config->tx_alloc_size > 0u) {
      req->tx_alloc_buf =
          &config->static_resources.tx_buffer_storage[(size_t)i *
                                                       (size_t)config->tx_alloc_size];
      req->tx_buf = req->tx_alloc_buf;
    }
    if (config->rx_alloc_size > 0u) {
      req->rx_alloc_buf =
          &config->static_resources.rx_buffer_storage[(size_t)i *
                                                       (size_t)config->rx_alloc_size];
      req->rx_buf = req->rx_alloc_buf;
    }
  }

  esy_err_t err = esy_block_ring_init(&gw->req_ring, (uint8_t *)gw->req_buf,
                                      sizeof(esy_spi_scheduler_request_t),
                                      gw->block_count);
  if (err != ESY_OK) {
    return err;
  }

  esy_io_scheduler_atomic_flag_set(&gw->ready, true);
  return ESY_OK;
}

esy_err_t esy_spi_scheduler_core_create(
    esy_spi_t *spi,
    const esy_spi_scheduler_core_config_t *config,
    esy_spi_scheduler_storage_t *storage,
    esy_spi_scheduler_t **out_gw) {
  if (!spi || !config || !storage || !out_gw) {
    return ESY_ERR_INVALID_ARG;
  }
  *out_gw = NULL;

  esy_spi_scheduler_t *gw = (esy_spi_scheduler_t *)storage->bytes;
  if (storage->bytes[0] != 0u) return ESY_ERR_STATE;
  if (esy_hal_spi_is_busy(spi)) return ESY_ERR_BUSY;
  if (esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  memset(gw, 0, sizeof(*gw));
  gw->allocated = 1u;

  esy_err_t err = init_scheduler_storage(gw, spi, config);
  if (err != ESY_OK) {
    gw->allocated = 0u;
    return err;
  }

  *out_gw = gw;
  return ESY_OK;
}

static esy_err_t release_scheduler(esy_spi_scheduler_t *gw) {
  if (!gw) {
    return ESY_OK;
  }

  esy_err_t err = stop_scheduler(gw);
  if (err != ESY_OK) {
    return err;
  }

  if (gw->task) {
    gw->task = NULL;
  }

  if (!gw->req_buf) {
    return ESY_OK;
  }

  gw->req_buf = NULL;
  memset(&gw->req_ring, 0, sizeof(gw->req_ring));
  esy_io_scheduler_atomic_flag_set(&gw->busy, false);
  gw->current = NULL;
  gw->write_req = NULL;
  gw->pending = NULL;
  gw->allocated = 0u;
  return ESY_OK;
}

esy_err_t esy_spi_scheduler_destroy(esy_spi_scheduler_t *gw) {
  if (esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  return release_scheduler(gw);
}

esy_spi_scheduler_request_t *esy_spi_scheduler_get_write_block(
    esy_spi_scheduler_t *gw) {
  if (!gw || !gw->req_buf ||
      esy_io_scheduler_atomic_flag_get(&gw->stopping) || gw->write_req) {
    return NULL;
  }

  esy_spi_scheduler_request_t *req =
      (esy_spi_scheduler_request_t *)esy_block_ring_get_write_block(
          &gw->req_ring);
  if (!req) return NULL;

  reset_request(req);
  gw->write_req = req;

  return req;
}

esy_err_t esy_spi_scheduler_submit(esy_spi_scheduler_t *gw,
                            esy_spi_scheduler_request_t *req) {
  if (!gw || !req || !gw->req_buf || !esy_io_scheduler_transfer_valid(req->tx_buf, req->rx_buf, req->len, req->transfer_size) ||
      (!req->tx_buf && !req->rx_buf) ||
      esy_io_scheduler_atomic_flag_get(&gw->stopping) ||
      !scheduler_has_request(gw, req) ||
      (req->tx_buf && req->tx_buf == req->tx_alloc_buf && req->len > gw->tx_alloc_size) ||
      (req->rx_buf && req->rx_buf == req->rx_alloc_buf && req->len > gw->rx_alloc_size) ||
      ((req->prepare != NULL) != (req->prepared_done != NULL)) ||
      (req->prepare != NULL && (req->tx_buf == NULL || req->rx_buf != NULL))) {
    return ESY_ERR_INVALID_ARG;
  }
  if (req != gw->write_req) {
    return ESY_ERR_STATE;
  }

  atomic_fetch_add_explicit(&gw->pending_count, 1u, memory_order_acq_rel);
  if (esy_io_scheduler_atomic_flag_get(&gw->quiesced)) {
    atomic_fetch_sub_explicit(&gw->pending_count, 1u, memory_order_release);
    return ESY_ERR_BUSY;
  }
  esy_block_ring_commit_write(&gw->req_ring);
  gw->write_req = NULL;
  if (gw->pending == req) {
    gw->pending = NULL;
  }

  if (!esy_io_scheduler_atomic_flag_get(&gw->busy)) {
    /* Commit is the acceptance point; a published request cannot be rejected.
     * The owner must retain a live, notification-capable worker until drain. */
    if (gw->wake) (void)gw->wake(gw->task);
  }

  return ESY_OK;
}

static void esy_spi_scheduler_on_dma_done(
    esy_spi_t *spi, const esy_spi_dma_transfer_result_t *result, void *arg) {
  esy_spi_scheduler_t *gw = arg;
  (void)spi;
  if (!gw || !gw->current) return;
  esy_spi_scheduler_request_t *req = gw->current;
  req->transferred = 0u;
  req->result = ESY_ERR_IO;
  if (result) {
    req->transferred = req->tx_buf ? result->tx_transferred : result->rx_transferred;
    if (req->tx_buf && req->rx_buf && result->rx_transferred < req->transferred)
      req->transferred = result->rx_transferred;
    if (result->status == ESY_SPI_DMA_TRANSFER_OK && req->transferred == req->len)
      req->result = ESY_OK;
  }
  /* Active workers already poll at 1 ms; support IRQ and owner-task providers. */
  /* Last context access: another core may finish and release storage now. */
  esy_io_scheduler_atomic_flag_set(&gw->dma_done, true);

}

static void try_start_next_transfer(esy_spi_scheduler_t *gw) {
  if (!gw || !gw->spi || !gw->req_buf) return;
  if (esy_io_scheduler_atomic_flag_get(&gw->stopping) ||
      esy_io_scheduler_atomic_flag_get(&gw->quiesced) ||
      esy_io_scheduler_atomic_flag_get(&gw->busy)) return;
  esy_spi_scheduler_request_t *req =
      (esy_spi_scheduler_request_t *)esy_block_ring_get_read_block(&gw->req_ring);
  if (!req) return;
  esy_io_scheduler_atomic_flag_set(&gw->busy, true);
  if (esy_io_scheduler_atomic_flag_get(&gw->quiesced) ||
      esy_io_scheduler_atomic_flag_get(&gw->stopping)) {
    esy_io_scheduler_atomic_flag_set(&gw->busy, false);
    return;
  }
  gw->current = req;
  gw->wire_pending = false;
  gw->failed_submit = false;
  esy_io_scheduler_atomic_flag_set(&gw->dma_done, false);
  if (req->prepare) {
    req->result = req->prepare(req->prepared_arg);
    if (req->result != ESY_OK) {
      finish_current_request(gw);
      if (gw->wake) (void)gw->wake(gw->task);
      return;
    }
  }
  if (req->pre_callback) req->pre_callback(req->pre_callback_arg);
  if (req->len > ESY_SPI_SCHEDULER_DMA_THRESHOLD_BYTES ||
      req->transfer_size != ESY_DMA_TRANSFER_SIZE_8) {
    esy_dma_config_t tx, rx;
    esy_hal_dma_default_cfg(&tx);
    esy_hal_dma_default_cfg(&rx);
    tx.src = req->tx_buf;
    rx.dst = req->rx_buf;
    tx.length = rx.length = req->len;
    tx.transfer_size = rx.transfer_size = req->transfer_size;
    esy_err_t err = esy_hal_spi_transfer_dma(gw->spi,
        req->tx_buf != NULL ? &tx : NULL, req->rx_buf != NULL ? &rx : NULL,
                                                  esy_spi_scheduler_on_dma_done, gw);
    if (err == ESY_OK) {
      gw->wire_pending = true;
      return;
    }
    if (esy_hal_spi_is_busy(gw->spi)) {
      /* A failed submission can still own DMA buffers and a pending callback.
       * Keep prepared resources and the queue slot until owner-side retirement. */
      req->result = err;
      gw->failed_submit = true;
      gw->wire_pending = true;
      return;
    }
    if (err != ESY_ERR_NOT_SUPPORTED && err != ESY_ERR_NOT_IMPLEMENTED) {
      req->result = err;
      finish_current_request(gw);
      if (gw->wake) (void)gw->wake(gw->task);
      return;
    }
  }
  if (req->transfer_size != ESY_DMA_TRANSFER_SIZE_8) {
    req->result = ESY_ERR_NOT_SUPPORTED;
    finish_current_request(gw);
    if (gw->wake) (void)gw->wake(gw->task);
    return;
  }
  int32_t count;
  if (!req->rx_buf) count = esy_hal_spi_write_blocking(gw->spi, req->tx_buf, req->len);
  else if (!req->tx_buf) count = esy_hal_spi_read_blocking(gw->spi, req->rx_buf, req->len);
  else count = esy_hal_spi_transfer_blocking(gw->spi, req->tx_buf, req->rx_buf, req->len);
  req->transferred = count >= 0 ? (size_t)count : 0u;
  req->result = count < 0 ? (esy_err_t)count
      : ((size_t)count == req->len ? ESY_OK : ESY_ERR_IO);
  gw->wire_pending = count > 0;
  esy_io_scheduler_atomic_flag_set(&gw->dma_done, true);
  if (!gw->wire_pending || !esy_hal_spi_is_busy(gw->spi)) finish_current_request(gw);
  if (gw->wake) (void)gw->wake(gw->task);
}

esy_err_t esy_spi_scheduler_quiesce(esy_spi_scheduler_t *gw) {
  if ((gw == NULL) || (gw->req_buf == NULL)) return ESY_ERR_INVALID_ARG;
  if (esy_io_scheduler_atomic_flag_get(&gw->stopping)) return ESY_ERR_STATE;
  if (esy_io_scheduler_atomic_flag_get(&gw->quiesced)) return ESY_ERR_STATE;

  esy_io_scheduler_atomic_flag_set(&gw->quiesced, true);
  if (esy_io_scheduler_atomic_flag_get(&gw->busy) ||
      esy_hal_spi_is_busy(gw->spi) ||
      atomic_load_explicit(&gw->pending_count, memory_order_acquire) != 0u) {
    esy_io_scheduler_atomic_flag_set(&gw->quiesced, false);
    if (gw->wake) (void)gw->wake(gw->task);
    return ESY_ERR_BUSY;
  }
  return ESY_OK;
}

esy_err_t esy_spi_scheduler_resume(esy_spi_scheduler_t *gw) {
  if ((gw == NULL) || (gw->req_buf == NULL)) return ESY_ERR_INVALID_ARG;
  if (!esy_io_scheduler_atomic_flag_get(&gw->quiesced)) return ESY_ERR_STATE;

  esy_io_scheduler_atomic_flag_set(&gw->quiesced, false);
  return gw->wake ? gw->wake(gw->task) : ESY_OK;
}

void esy_spi_scheduler_process_context(esy_spi_scheduler_t *gw) {
  if (!gw || !esy_io_scheduler_atomic_flag_get(&gw->ready)) return;
  bool stopping = esy_io_scheduler_atomic_flag_get(&gw->stopping);
  (void)esy_hal_spi_poll(gw->spi);
  if (gw->current && gw->failed_submit &&
      !esy_io_scheduler_atomic_flag_get(&gw->dma_done)) {
    /* Poll may retire the failed transfer and notify us. Otherwise one abort
     * attempt per slice can certify silent cancellation; errors retain ownership. */
    if (esy_hal_spi_abort(gw->spi) == ESY_OK) {
      gw->wire_pending = false;
      esy_io_scheduler_atomic_flag_set(&gw->dma_done, true);
    }
  }
  /* Drain terminal callbacks. A best-effort abort does not certify ISR retirement. */
  if (gw->current && esy_io_scheduler_atomic_flag_get(&gw->dma_done) &&
      (!gw->wire_pending || !esy_hal_spi_is_busy(gw->spi))) {
    finish_current_request(gw);
  }
  if (stopping) {
    return;
  }
  try_start_next_transfer(gw);
}

esy_err_t esy_spi_scheduler_core_process(esy_spi_scheduler_t *gw) {
  if (!gw || esy_hal_system_is_in_isr()) return ESY_ERR_INVALID_ARG;
  if (!gw->allocated || gw->task || gw->processing) return ESY_ERR_STATE;
  gw->processing = true;
  esy_spi_scheduler_process_context(gw);
  gw->processing = false;
  return ESY_OK;
}


esy_err_t esy_spi_scheduler_get_write_buffer_with_arg(
    uint8_t **tx_buf,
    uint8_t **rx_buf,
    void *arg) {
  esy_spi_scheduler_t *spi_scheduler = (esy_spi_scheduler_t*)arg;
  if (tx_buf) {
    *tx_buf = NULL;
  }
  if (rx_buf) {
    *rx_buf = NULL;
  }
  if (!spi_scheduler || (!tx_buf && !rx_buf)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (spi_scheduler->pending || spi_scheduler->write_req) {
    return ESY_ERR_STATE;
  }

  esy_spi_scheduler_request_t *req = esy_spi_scheduler_get_write_block(spi_scheduler);
  if (!req) {
    return ESY_ERR_BUSY;
  }

  spi_scheduler->pending = req;
  if (tx_buf) {
    *tx_buf = req->tx_buf;
  }
  if (rx_buf) {
    *rx_buf = req->rx_buf;
  }
  return ESY_OK;
}

esy_err_t esy_spi_scheduler_get_read_write_buffer_with_arg(
    uint8_t **tx_buf, uint8_t **rx_buf, size_t minimum_size, void *arg) {
  esy_spi_scheduler_t *gw = arg;
  if (tx_buf) *tx_buf = NULL;
  if (rx_buf) *rx_buf = NULL;
  if (!gw || !tx_buf || !rx_buf || minimum_size == 0u)
    return ESY_ERR_INVALID_ARG;
  if (!gw->allocated || gw->pending || gw->write_req) return ESY_ERR_STATE;
  if (minimum_size > gw->tx_alloc_size || minimum_size > gw->rx_alloc_size)
    return ESY_ERR_NO_MEM;
  return esy_spi_scheduler_get_write_buffer_with_arg(tx_buf, rx_buf, gw);
}

static bool pending_buffers_match(const esy_spi_scheduler_request_t *req,
                                  const uint8_t *tx_buf,
                                  const uint8_t *rx_buf) {
  if (!req) {
    return false;
  }

  if (tx_buf && req->tx_buf && tx_buf != req->tx_buf) {
    return false;
  }
  if (rx_buf && req->rx_buf && rx_buf != req->rx_buf) {
    return false;
  }
  return true;
}

esy_err_t esy_spi_scheduler_submit_result_with_arg(uint8_t *tx_buf, uint8_t *rx_buf, uint32_t len,
                        void (*pre_transfer_cb)(void *arg),
                        void *pre_transfer_cb_arg,
                        esy_spi_transfer_done_cb_t result_callback,
                        void *result_callback_arg, void *arg) {
  esy_spi_scheduler_t *spi_scheduler = (esy_spi_scheduler_t*)arg;
  if (!spi_scheduler || !result_callback || len == 0u || (!tx_buf && !rx_buf)) {
    return ESY_ERR_INVALID_ARG;
  }

  esy_spi_scheduler_request_t *req = spi_scheduler->pending;
  if (req && !pending_buffers_match(req, tx_buf, rx_buf)) {
    return ESY_ERR_STATE;
  }
  if (!req && spi_scheduler->write_req) {
    return ESY_ERR_STATE;
  }

  bool internally_reserved = req == NULL;
  if (!req) {
    req = esy_spi_scheduler_get_write_block(spi_scheduler);
  }
  if (!req) {
    return ESY_ERR_BUSY;
  }

  req->tx_buf = tx_buf;
  req->rx_buf = rx_buf;
  req->len = len;
  req->transfer_size = ESY_DMA_TRANSFER_SIZE_8;
  req->result = ESY_ERR_STATE;

  req->pre_callback = pre_transfer_cb;
  req->pre_callback_arg = pre_transfer_cb_arg;

  req->result_callback = result_callback;
  req->result_callback_arg = result_callback_arg;

  esy_err_t err = esy_spi_scheduler_submit(spi_scheduler, req);
  if (err != ESY_OK && internally_reserved) spi_scheduler->write_req = NULL;
  return err;
}

esy_err_t esy_spi_scheduler_submit_prepared_with_arg(
    const uint8_t *tx_buf, uint32_t len, esy_spi_prepare_cb_t prepare,
    esy_spi_prepared_done_cb_t done, void *callback_arg, void *arg) {
  esy_spi_scheduler_t *gw = (esy_spi_scheduler_t *)arg;
  if ((gw == NULL) || (gw->req_buf == NULL) || (tx_buf == NULL) ||
      (len == 0u) || (prepare == NULL) || (done == NULL) ||
      esy_io_scheduler_atomic_flag_get(&gw->stopping)) return ESY_ERR_INVALID_ARG;
  if (gw->pending != NULL || gw->write_req != NULL) return ESY_ERR_STATE;
  if (esy_io_scheduler_atomic_flag_get(&gw->quiesced)) return ESY_ERR_BUSY;
  esy_spi_scheduler_request_t *req = esy_spi_scheduler_get_write_block(gw);
  if (req == NULL) return ESY_ERR_BUSY;
  req->tx_buf = (uint8_t *)tx_buf;
  req->rx_buf = NULL;
  req->len = len;
  req->prepare = prepare;
  req->prepared_done = done;
  req->prepared_arg = callback_arg;
  esy_err_t err = esy_spi_scheduler_submit(gw, req);
  if (err != ESY_OK) gw->write_req = NULL;
  return err;
}
