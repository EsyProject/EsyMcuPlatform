/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_SPI_SCHEDULER_INTERNAL_H
#define ESY_SPI_SCHEDULER_INTERNAL_H
#include <stdatomic.h>
#include "esy_middleware/io_scheduler/esy_spi_scheduler_core.h"
#include "esy_io_scheduler_core_internal.h"
/**
 * @brief SPI scheduler context.
 */
struct esy_spi_scheduler_s {
  uint8_t allocated;
  esy_spi_t *spi;
  esy_block_ring_t req_ring;          ///< Request ring buffer.
  esy_spi_scheduler_request_t *req_buf; ///< Request storage array.
  uint32_t tx_alloc_size;
  uint32_t rx_alloc_size;
  uint32_t block_count;               ///< Number of request blocks.
  atomic_bool busy;                   ///< Current transfer is active.
  atomic_bool stopping;               ///< Stop requested; reject new work.
  atomic_bool quiesced;                ///< PM transition blocks new transfers.
  atomic_bool ready;
  atomic_bool dma_done;
  bool wire_pending;
  bool failed_submit;
  esy_spi_scheduler_request_t *write_req; ///< Currently borrowed write block.
  esy_spi_scheduler_request_t *current; ///< Request currently being processed.
  esy_spi_scheduler_request_t *pending; ///< Convenience API reserved block.
  atomic_uint pending_count;       ///< Accepted transactions still owned.
  void *task;                ///< Optional OS adapter worker.
  bool (*is_worker)(void *task);
  esy_err_t (*wake)(void *task);
  esy_err_t (*stop_worker)(void *task);
  bool processing;
};

void esy_spi_scheduler_process_context(esy_spi_scheduler_t *gw);
#endif /* ESY_SPI_SCHEDULER_INTERNAL_H */
