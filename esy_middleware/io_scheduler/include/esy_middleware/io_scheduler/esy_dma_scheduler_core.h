/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_DMA_SCHEDULER_CORE_H
#define ESY_DMA_SCHEDULER_CORE_H

#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>

#include "esy_common/ds/esy_block_ring.h"
#include "esy_hal/esy_hal_dma.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Terminal scheduler result, delivered once from owner process.
 * Cancellation reports ESY_OK with cancelled=true, without invoking the DMA
 * descriptor's completion/error callback. Native DMA errors report ESY_ERR_IO;
 * prepare/start errors retain their public HAL error code. Do not reenter core
 * APIs or release core/controller storage from any application callback.
 */
typedef void (*esy_dma_scheduler_done_cb_t)(esy_err_t result, bool cancelled,
                                           void *arg);

/** @brief One serialized DMA route request, copied on successful submit.
 * Buffers and every callback argument remain borrowed until done returns, or
 * until the original finite terminal callback returns if done is NULL. Without
 * done, cancellation or submission failure has no original terminal callback;
 * use is_idle before releasing any accepted request's borrowed storage/contexts.
 * Original DMA completion/half/error callbacks retain their owner-context
 * meaning; half and circular completion do not terminate the request.
 */
typedef struct {
    esy_dma_config_t transfer; /**< Single route; no batch/chaining inference. */
    esy_dma_scheduler_done_cb_t done; /**< Optional terminal status callback. */
    void *done_arg; /**< Borrowed terminal callback argument. */
} esy_dma_scheduler_request_t;

/** @brief Caller-owned request slots; count is power-of-two, at least two.
 * Usable capacity is count-1, including any active request. Keep slots alive
 * and do not modify them until successful core deinit.
 */
typedef struct {
    esy_dma_scheduler_request_t *request_storage;
    uint32_t request_storage_count;
} esy_dma_scheduler_core_resources_t;

/** @brief Static queue configuration. */
typedef struct {
    esy_dma_scheduler_core_resources_t static_resources;
} esy_dma_scheduler_core_config_t;

#define ESY_DMA_SCHEDULER_CORE_STORAGE_BYTES 512u

/** @brief Opaque caller-owned core storage; initialize to {0} before first init.
 * One non-ISR owner serializes every API, including submit and process. Drive
 * processing regularly while active; no task, OS object, SDK or allocation is used.
 * Never copy/reset a live core. Failed deinit retains all storage and borrows.
 * Core, queue and controller storage must be mutually disjoint and must not
 * overlap transfer buffers. Payload endpoint overlap follows the HAL contract.
 */
typedef struct {
    alignas(esy_block_ring_t) uint8_t bytes[ESY_DMA_SCHEDULER_CORE_STORAGE_BYTES];
} esy_dma_scheduler_core_t;

/** @brief Borrow an initialized, idle controller without changing its lifecycle.
 * Exclude all other users of this controller and every configured route through
 * successful deinit. The core never initializes/deinitializes the borrowed HAL.
 * Repeated init returns STATE; invalid configuration retains the prior storage.
 * @return OK, INVALID_ARG, STATE for a live core, or BUSY when the controller
 * cannot be proved idle (including an uninitialized controller).
 */
esy_err_t esy_dma_scheduler_core_init(esy_dma_scheduler_core_t *core,
    esy_dma_t *dma, const esy_dma_scheduler_core_config_t *config);

/** @brief Copy one descriptor; reject a full queue with BUSY, stopped with STATE.
 * Route and hardware-specific validation happens in process. A rejected submit
 * borrows nothing. The owner schedules subsequent processing.
 */
esy_err_t esy_dma_scheduler_core_submit(esy_dma_scheduler_core_t *core,
    const esy_dma_scheduler_request_t *request);

/** @brief Advance at most one request: submit, HAL processing call, or one cleanup attempt.
 * Original DMA callbacks and done run only inside this call. A failed start
 * retains its prepared route even when HAL is_busy is false; later processing
 * explicitly aborts it before releasing the slot or buffers. Failed cleanup
 * remains borrowed and retryable. Provider and callback duration contributes to
 * execution time; the request budget is not a wall-clock bound. Reentry is STATE.
 * @return OK for progress/idle, original submission error, or cleanup error.
 */
esy_err_t esy_dma_scheduler_core_process(esy_dma_scheduler_core_t *core);

/** @brief Close admission and drain active finite DMA; optionally cancel queued.
 * Cancellation of queued requests occurs one per process turn after active
 * retirement. Circular DMA needs request_abort to terminate. A later stop never
 * clears a previous cancellation/abort request. Continue process until idle.
 */
esy_err_t esy_dma_scheduler_core_request_stop(esy_dma_scheduler_core_t *core,
                                             bool cancel_queued);

/** @brief Close admission and cancel active plus queued work through process.
 * This upgrades an earlier stop. Native abort suppresses the original DMA
 * terminal callbacks; each accepted request still receives done exactly once.
 * A known start failure keeps its original error after cleanup, even when abort
 * upgrades shutdown before that cleanup succeeds.
 */
esy_err_t esy_dma_scheduler_core_request_abort(esy_dma_scheduler_core_t *core);

/** @brief True only when no queued request, active route or callback is retained.
 * NULL, inactive cores and callback reentry report false. This does not stop an
 * otherwise idle core; submissions remain allowed until stop/abort is requested.
 */
bool esy_dma_scheduler_core_is_idle(const esy_dma_scheduler_core_t *core);

/** @brief Relative owner wait: 0 queued, 1 ms active/cleanup, UINT32_MAX idle.
 * No timestamp or deadline is stored. Requery after each process turn. Active
 * finite and circular DMA both need regular processing to deliver events and
 * retire resources. Cleanup failure retains ownership; the owner bounds retries
 * or enters its fault policy. Invalid cores return UINT32_MAX.
 */
uint32_t esy_dma_scheduler_core_next_wait_ms(const esy_dma_scheduler_core_t *core);

/** @brief Release idle core storage; never stop/destroy the borrowed HAL.
 * NULL and zero storage succeed. BUSY retains queued/active/callback ownership;
 * request stop/abort and continue process, then retry deinit. Success permits
 * reuse of storage and the controller by another owner.
 */
esy_err_t esy_dma_scheduler_core_deinit(esy_dma_scheduler_core_t *core);

#ifdef __cplusplus
}
#endif

#endif /* ESY_DMA_SCHEDULER_CORE_H */
