/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_prio_slot_queue.h
 * @brief Priority slot queue (small transaction scheduler).
 *
 * This module implements a lightweight, malloc-free priority scheduler for
 * "request/transaction" style workloads (not a byte stream).
 *
 * Design highlights:
 * - No malloc/free and no OS dependency.
 * - Slot metadata storage is provided by the caller (array of slots).
 * - Priority scheduling: larger priority value means higher priority.
 * - FIFO within the same priority using caller-owned intrusive slot links.
 *   Ordering has no limit on enqueue age or total lifetime operations.
 * - Slot lifecycle states: FREE / RESERVED / QUEUED / IN_PROGRESS.
 * - Per-slot caller-defined opaque field @ref esy_prio_slot_t::user_data for
 *   mapping slots to external payloads/IDs/pointers.
 *
 * Typical usage:
 * 1) Caller provides an array of @ref esy_prio_slot_t and initializes the
 * queue. 2) Allocate a FREE slot -> becomes RESERVED. 3) Fill the caller-owned
 * payload associated with that slot (by index or user_data). 4) Enqueue the
 * slot -> becomes QUEUED. 5) Pick the next slot -> mark it IN_PROGRESS. 6) On
 * completion, free the slot or requeue it (for retry).
 *
 * This container is not thread-safe. Its owner must serialize all operations,
 * including selection followed by marking a slot in progress.
 */

#ifndef ESY_COMMON_DS_ESY_PRIO_SLOT_QUEUE_H
#define ESY_COMMON_DS_ESY_PRIO_SLOT_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   /* size_t */
#include <inttypes.h> /* uintptr_t */

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Types                                                                      */
/* -------------------------------------------------------------------------- */

/** @brief Largest slot count whose indices fit the nonnegative int16_t result. */
#define ESY_PRIO_SLOT_QUEUE_MAX_SLOT_COUNT (INT16_MAX + 1u)

/**
 * @brief Slot state.
 */
typedef enum {
  ESY_PRIO_SLOT_STATE_FREE = 0,    /**< Free slot, can be allocated. */
  ESY_PRIO_SLOT_STATE_RESERVED,    /**< Allocated but not enqueued yet. */
  ESY_PRIO_SLOT_STATE_QUEUED,      /**< Enqueued and waiting to be scheduled. */
  ESY_PRIO_SLOT_STATE_IN_PROGRESS, /**< Currently being processed. */
} esy_prio_slot_state_t;

/**
 * @brief Slot metadata (caller-owned payload is external to this module).
 *
 * Notes:
 * - This module owns and updates state, priority and FIFO links. Callers must
 *   not change this bookkeeping directly.
 * - @ref user_data is caller-defined and is not interpreted by this module.
 * - @ref user_data uses @c uintptr_t to support either integers or pointer-like
 * tokens.
 */
typedef struct {
  esy_prio_slot_state_t state; /**< Slot lifecycle state. */
  uint8_t priority;            /**< Larger means higher priority. */
  int16_t fifo_prev; /**< Previous QUEUED slot index, or -1. */
  int16_t fifo_next; /**< Next QUEUED slot index, or -1. */
  uintptr_t
      user_data; /**< Caller-defined opaque value (e.g. index/ID/pointer). */
} esy_prio_slot_t;

/**
 * @brief Queue initialization configuration.
 */
typedef struct {
  esy_prio_slot_t *slots; /**< Slot array provided by the caller. */
  uint16_t slot_count; /**< 1..ESY_PRIO_SLOT_QUEUE_MAX_SLOT_COUNT slots. */
} esy_prio_slot_queue_config_t;

/**
 * @brief Priority slot queue instance.
 *
 * This object only manages slot metadata. The caller typically maintains a
 * separate payload array indexed by slot index or mapped via @ref user_data.
 */
typedef struct {
  esy_prio_slot_t *slots; /**< External slot array pointer. */
  uint16_t slot_count;    /**< Total slots. */
  uint16_t queued_count;  /**< Number of QUEUED slots (excludes
                             RESERVED/IN_PROGRESS). */
  int16_t fifo_head;      /**< Oldest QUEUED slot index, or -1. */
  int16_t fifo_tail;      /**< Newest QUEUED slot index, or -1. */
  bool initialized;       /**< Initialization flag. */
} esy_prio_slot_queue_t;

/* -------------------------------------------------------------------------- */
/* API                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Get a default configuration.
 *
 * Default values:
 * - slots = NULL
 * - slot_count = 0
 */
esy_prio_slot_queue_config_t esy_prio_slot_queue_default_cfg(void);

/**
 * @brief Initialize the priority slot queue.
 *
 * This function resets all internal bookkeeping and sets every slot to FREE.
 * Slot counts above ESY_PRIO_SLOT_QUEUE_MAX_SLOT_COUNT are rejected so every
 * successful allocation, selection and lookup has a nonnegative int16_t index.
 * Invalid configuration leaves both the queue and supplied slots unchanged.
 * The queue object and slot array may contain arbitrary initial bytes. Repeated
 * init replaces the binding and discards all previous slot state and user_data;
 * it does not release caller payloads. Stop all users and finish every reserved,
 * queued or in-progress payload/slot borrow before init or deinit. After a
 * failed first init, retry init successfully before querying or operating on
 * the queue. A rejected reinit preserves the previous initialized queue.
 *
 * @param[out] q   Queue instance (caller-provided storage).
 * @param[in]  cfg Initialization configuration.
 *
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG NULL pointers or slot_count outside the documented range.
 */
esy_err_t esy_prio_slot_queue_init(esy_prio_slot_queue_t *q,
                                   const esy_prio_slot_queue_config_t *cfg);

/**
 * @brief Deinitialize the queue (clears internal state).
 *
 * Call after successful init, with all users stopped and slot/payload borrows
 * ended. Clears the queue binding and bookkeeping; the old slot array and
 * payloads remain caller-owned and are not cleared or freed. Repeated deinit
 * after successful deinit is harmless. To use again, call @ref
 * esy_prio_slot_queue_init; deinit does not establish a new slot binding.
 *
 * @param[in,out] q Queue instance.
 */
void esy_prio_slot_queue_deinit(esy_prio_slot_queue_t *q);

/**
 * @brief Check whether the queue is initialized.
 *
 * @param[in] q Queue instance.
 * @return true if initialized; false otherwise.
 */
bool esy_prio_slot_queue_is_initialized(const esy_prio_slot_queue_t *q);

/**
 * @brief Allocate a FREE slot and mark it as RESERVED.
 *
 * The queue scans for the first FREE slot. If found, it becomes RESERVED and
 * its @ref esy_prio_slot_t::user_data is set to @p user_data.
 *
 * @param[in,out] q         Queue instance.
 * @param[in]     user_data Caller-defined opaque value (may be 0).
 *
 * @return >=0  Allocated slot index.
 * @return -1   No FREE slot available.
 */
int16_t esy_prio_slot_queue_alloc_slot(esy_prio_slot_queue_t *q,
                                       uintptr_t user_data);

/**
 * @brief Free a slot (regardless of its state).
 *
 * If the slot is currently QUEUED, queued_count is decremented accordingly.
 * The slot will end up in FREE state and its fields are cleared.
 *
 * @param[in,out] q          Queue instance.
 * @param[in]     slot_index Slot index.
 */
void esy_prio_slot_queue_free_slot(esy_prio_slot_queue_t *q,
                                   uint16_t slot_index);

/**
 * @brief Enqueue a slot (typically from RESERVED).
 *
 * The slot becomes QUEUED, its priority is set, and it is appended to the FIFO
 * chain in O(1).
 *
 * @param[in,out] q          Queue instance.
 * @param[in]     slot_index Slot index to enqueue.
 * @param[in]     priority   Priority value (larger means higher priority).
 *
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid parameters (NULL/idx out of
 * range/uninitialized).
 * @retval ESY_ERR_STATE       Slot state does not allow enqueue (already
 * queued/in progress, etc.).
 */
esy_err_t esy_prio_slot_queue_enqueue(esy_prio_slot_queue_t *q,
                                      uint16_t slot_index, uint8_t priority);

/**
 * @brief Pick the next slot to process among all QUEUED slots.
 *
 * Selection rule:
 * - Highest priority wins.
 * - If same priority, earlier enqueue wins regardless of its age.
 * Selection scans the QUEUED FIFO chain in O(queued_count).
 *
 * @param[in] q Queue instance.
 * @return >=0 Selected slot index.
 * @return -1  No QUEUED slot available.
 */
int16_t esy_prio_slot_queue_pick_next(const esy_prio_slot_queue_t *q);

/**
 * @brief Mark a slot as IN_PROGRESS (typically after pick_next()).
 *
 * If the slot was QUEUED, queued_count is decremented.
 *
 * @param[in,out] q          Queue instance.
 * @param[in]     slot_index Slot index.
 */
void esy_prio_slot_queue_mark_in_progress(esy_prio_slot_queue_t *q,
                                          uint16_t slot_index);

/**
 * @brief Requeue an IN_PROGRESS slot (commonly used for retry).
 *
 * The slot becomes QUEUED again at the FIFO tail, after already queued peers.
 *
 * @param[in,out] q          Queue instance.
 * @param[in]     slot_index Slot index.
 * @param[in]     priority   New priority to use when requeued.
 *
 * @retval ESY_OK              Success.
 * @retval ESY_ERR_INVALID_ARG Invalid parameters.
 * @retval ESY_ERR_STATE       Slot is not IN_PROGRESS.
 */
esy_err_t esy_prio_slot_queue_requeue(esy_prio_slot_queue_t *q,
                                      uint16_t slot_index, uint8_t priority);

/**
 * @brief Get the number of QUEUED slots.
 *
 * @param[in] q Queue instance.
 * @return Number of QUEUED slots (0..slot_count).
 */
uint16_t esy_prio_slot_queue_get_depth(const esy_prio_slot_queue_t *q);

/**
 * @brief Get the current state of a slot.
 *
 * @param[in] q          Queue instance.
 * @param[in] slot_index Slot index.
 *
 * @return Slot state. If parameters are invalid, returns
 * ESY_PRIO_SLOT_STATE_FREE.
 */
esy_prio_slot_state_t esy_prio_slot_queue_get_slot_state(
    const esy_prio_slot_queue_t *q, uint16_t slot_index);

/**
 * @brief Get the user_data of a slot.
 *
 * @param[in] q          Queue instance.
 * @param[in] slot_index Slot index.
 *
 * @return user_data value. If parameters are invalid, returns (uintptr_t)0.
 */
uintptr_t esy_prio_slot_queue_get_slot_user_data(const esy_prio_slot_queue_t *q,
                                                 uint16_t slot_index);

/**
 * @brief Find the first slot whose user_data matches @p user_data.
 *
 * @param[in] q         Queue instance.
 * @param[in] user_data Target user_data.
 *
 * @return >=0 Slot index of the first match.
 * @return -1  Not found.
 */
int16_t esy_prio_slot_queue_find_by_user_data(const esy_prio_slot_queue_t *q,
                                              uintptr_t user_data);

#ifdef __cplusplus
}
#endif

#endif /* ESY_COMMON_DS_ESY_PRIO_SLOT_QUEUE_H */
