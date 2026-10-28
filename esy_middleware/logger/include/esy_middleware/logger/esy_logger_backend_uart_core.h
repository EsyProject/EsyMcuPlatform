/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file esy_logger_backend_uart_core.h
 * @brief Scheduling-independent buffered uart logger engine. */
#ifndef ESY_LOGGER_BACKEND_UART_CORE_H
#define ESY_LOGGER_BACKEND_UART_CORE_H
#include <stddef.h>
#include <stdint.h>
#include "esy_logger_backend.h"
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_common/defs/peripheral/esy_uart_defs.h"
#ifdef __cplusplus
extern "C" {
#endif
/** @brief Byte capacity of one caller-owned UART backend context. */
#define ESY_LOGGER_BACKEND_UART_CONTEXT_BYTES 384u
/** @brief Required alignment for the UART backend context and queue state. */
#define ESY_LOGGER_BACKEND_UART_CONTEXT_ALIGN 32u
/**
 * @brief Opaque context storage for one independent UART backend instance.
 *
 * Initialize this object with `{0}` before first use. Do not inspect, copy,
 * clear or reuse it while init succeeded and deinit has not returned ESY_OK,
 * including after BUSY. Successful deinit makes it reusable. Rejected reuse
 * preserves the existing instance; other init failures leave storage reusable.
 * Pool payload, index and ring storage need not be initialized.
 */
typedef struct {
#if defined(__cplusplus)
  alignas(ESY_LOGGER_BACKEND_UART_CONTEXT_ALIGN)
#else
  _Alignas(ESY_LOGGER_BACKEND_UART_CONTEXT_ALIGN)
#endif
      uint8_t bytes[ESY_LOGGER_BACKEND_UART_CONTEXT_BYTES]; /**< Private state. */
} esy_logger_backend_uart_context_storage_t;

/**
 * @brief Opaque ready-ring slot for the UART logger backend.
 */
typedef struct {
#if defined(__cplusplus)
  alignas(4)
#else
  _Alignas(4)
#endif
      uint8_t bytes[8u]; /**< Opaque ready-ring slot bytes. */
} esy_logger_backend_uart_ready_slot_t;

/** @brief Borrowed pool and queue storage; no task resources. */
typedef struct {
  esy_logger_backend_uart_context_storage_t
      *context_storage; /**< Zero-initialized state reserved for this instance. */
  uint8_t *pool_buffers; /**< Backing bytes for the shared log block pool. */
  size_t pool_buffer_size; /**< Total bytes in @ref pool_buffers. */
  uint32_t *pool_index_stacks; /**< Free-index stack entries for the pool. */
  size_t pool_index_stack_count; /**< Number of entries in @ref pool_index_stacks. */
  esy_logger_backend_uart_ready_slot_t
      *ready_ring_slots; /**< Backing slots for the shared ready ring. */
  size_t ready_ring_slot_count; /**< Number of entries in @ref ready_ring_slots. */
} esy_logger_backend_uart_core_resources_t;
/**
 * @brief Declare caller-owned, caller-driven UART backend storage.
 * @param name Object name; initialize it with `{0}` before first use.
 * @param pool_block_count Pool blocks (power of two, 2..32).
 * @param block_size Bytes per log block (1..128).
 * @note One object belongs to one instance until successful deinit.
 */
#define ESY_LOGGER_BACKEND_UART_CORE_STATIC_STORAGE(name, pool_block_count, block_size) \
  struct { \
    esy_logger_backend_uart_context_storage_t context_storage; \
    uint8_t pool_buffers[(pool_block_count) * (block_size)]; \
    uint32_t pool_index_stacks[(pool_block_count)]; \
    esy_logger_backend_uart_ready_slot_t ready_ring_slots[(pool_block_count)]; \
  } name

/**
 * @brief Bind caller-owned core storage without changing its contents.
 * @param resources Pointer to an esy_logger_backend_uart_core_resources_t.
 * @param storage Object declared by ESY_LOGGER_BACKEND_UART_CORE_STATIC_STORAGE.
 */
#define ESY_LOGGER_BACKEND_UART_CORE_BIND_STATIC_RESOURCES(resources, storage) \
  do { \
    (resources)->context_storage = &(storage).context_storage; \
    (resources)->pool_buffers = (storage).pool_buffers; \
    (resources)->pool_buffer_size = sizeof((storage).pool_buffers); \
    (resources)->pool_index_stacks = (storage).pool_index_stacks; \
    (resources)->pool_index_stack_count = \
        sizeof((storage).pool_index_stacks) / sizeof((storage).pool_index_stacks[0]); \
    (resources)->ready_ring_slots = (storage).ready_ring_slots; \
    (resources)->ready_ring_slot_count = \
        sizeof((storage).ready_ring_slots) / sizeof((storage).ready_ring_slots[0]); \
  } while (0)

/** @brief Buffer and transport configuration. */
typedef struct {
  /**
   * @brief Initialized, caller-owned UART handle; retain until successful deinit.
   * Each live instance requires a distinct UART TX resource. Binding the same
   * handle or physical UART to two instances violates the caller contract and
   * is not detected. Other TX clients require exclusive use or external arbitration.
   */
  esy_uart_t *uart;


  /* Transmit buffer pool */
  size_t   block_size;         /**< Size of each buffer block in bytes. */
  uint32_t pool_block_count;   /**< Pool blocks (power of two, 2..32). */

  /* DMA / short-packet strategy */
  uint32_t dma_short_threshold; /**< Length at or below which blocking TX is used. */

  esy_logger_backend_uart_core_resources_t
      *static_resources; /**< Caller-owned static storage resources. */
} esy_logger_backend_uart_core_config_t;
/** @brief Fill buffered backend defaults; static_resources remains NULL. */
void esy_logger_backend_uart_core_default_cfg(esy_logger_backend_uart_core_config_t *cfg);
/**
 * @brief Initialize the caller-driven backend without an OS worker.
 * @note Zero-initialize the backend object and context_storage before first use.
 * Every live instance has separate context, pool, ring and transport resources;
 * there is no fixed instance limit. Init returns STATE for a live handle or
 * context storage already leased to an instance, without changing either.
 * Lifecycle operations that share a storage object must be serialized.
 * Invalid configuration or creation failure leaves the handle unchanged and
 * context storage reusable; success borrows all resources until deinit succeeds.
 * One owner core
 * serializes frontend logging, raw producers, process and lifecycle; direct ISR
 * acquire/submit is allowed only on that core. Other cores must not use this
 * instance. Local IRQ masking protects pool and completion state. Borrowed
 * buffers, HAL handles and resource storage survive until deinit succeeds.
 * Existing OS init retains its multi-producer synchronization contract.
 */
esy_err_t esy_logger_backend_uart_core_init(esy_logger_backend_t *backend,
    const esy_logger_backend_uart_core_config_t *config);
/**
 * @brief Attempt at most block_budget queued blocks from the owner context.
 * @note Positive budget; no task waits. Provider operations determine duration,
 * including blocking UART short sends.
 * Call often enough to avoid queue exhaustion. BUSY sends retain the unsent tail
 * and retry no sooner than 10 ms, measured with the HAL millisecond tick; keep
 * tick gaps below half its range. Active DMA is polled before further sends;
 * completion can recycle storage here or in the port's callback context.
 * A failed submission that remains busy retains its pool block and queue head.
 * Later processing polls and attempts at most one abort per call; retirement
 * discards that failed log without retransmission. Cleanup failure never makes
 * the block available to producers. Each retirement attempt ends that slice,
 * even when the supplied block budget allows more work.
 * Do not call process or lifecycle recursively from providers/callbacks.
 * STATE means wrong mode or uninitialized; budget limits work count, not
 * wall-clock time. UART retirement errors are returned without releasing storage.
 * After deinit returns BUSY, continue processing the active transfer and retry
 * deinit without clearing storage. Shutdown processing starts no queued output.
 */
esy_err_t esy_logger_backend_uart_core_process(esy_logger_backend_t *backend, size_t block_budget);
/** @brief Detach and stop; BUSY retains outstanding leases/I/O for owner retry.
 * Success also requires physical UART TX idle, including FIFO/shift-register
 * drain after DMA source retirement. Source buffers are released independently.
 * Queued output is discarded on successful cleanup. Non-ISR owner calls only. */
esy_err_t esy_logger_backend_uart_deinit(esy_logger_backend_t *backend);
/** @brief Prevent new I/O only after callbacks and physical UART TX are idle.
 * BUSY leaves delivery running; retry while FIFO/shift-register drain continues.
 * The physical-busy query runs outside critical sections under a sender gate;
 * no drain wait or forced abort is performed. Source completion may already have
 * returned its pool block. Requires exclusive TX use or external arbitration. */
esy_err_t esy_logger_backend_uart_quiesce(esy_logger_backend_t *backend);
/** @brief Resume the quiesced owner; caller-driven mode needs subsequent process. */
esy_err_t esy_logger_backend_uart_resume(esy_logger_backend_t *backend);
#ifdef __cplusplus
}
#endif
#endif /* ESY_LOGGER_BACKEND_UART_CORE_H */
