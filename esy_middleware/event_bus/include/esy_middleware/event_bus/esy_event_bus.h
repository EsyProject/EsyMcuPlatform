/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_EVENT_BUS_H
#define ESY_EVENT_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_event_bus.h
 * @brief OS-backed local publish/subscribe with independent subscriber queues.
 *
 * Multiple task/OS-API-safe ISR publishers may run concurrently. Each subscriber
 * has one task consumer. One external task owner serializes subscribe,
 * unsubscribe and deinit. Initialization requires all users stopped. Queue
 * operations run outside short OS critical sections; no callback, worker task,
 * hardware access or allocation is introduced. Only publish_from_isr is
 * ISR-callable. Native interrupt priority restrictions still apply.
 */

/** @brief Number of topic IDs, numbered 0 through 31, assigned by the consumer. */
#define ESY_EVENT_BUS_TOPIC_COUNT 32u
/** @brief Maximum inline payload bytes; larger data needs a separate owned path. */
#define ESY_EVENT_BUS_PAYLOAD_BYTES 64u
/** @brief Opaque per-bus implementation storage budget. */
#define ESY_EVENT_BUS_STORAGE_BYTES 64u
/** @brief Opaque subscriber budget, including its OS queue control storage. */
#define ESY_EVENT_BUS_SUBSCRIPTION_STORAGE_BYTES 384u
/** @brief Topic mask bit; topic must be less than ESY_EVENT_BUS_TOPIC_COUNT. */
#define ESY_EVENT_BUS_TOPIC_BIT(topic) (UINT32_C(1) << (topic))

/**
 * @brief Self-contained message copied independently into matching queues.
 *
 * Only data[0..size) is meaningful. Publishing zeroes unused bytes in queued
 * copies. The input must remain immutable until publish returns. Pointer values
 * inside data do not transfer ownership or extend pointed-to object lifetimes;
 * prefer values. No DMA buffer lifetime or cache synchronization is provided.
 */
typedef struct {
  uint32_t topic; /**< Consumer-assigned topic ID, less than TOPIC_COUNT. */
  uint32_t size; /**< Payload bytes, zero through PAYLOAD_BYTES inclusive. */
  uint8_t data[ESY_EVENT_BUS_PAYLOAD_BYTES]; /**< Inline payload. */
} esy_event_bus_event_t;

/**
 * @brief Required queue data bytes for a nonzero depth.
 * Evaluate only when depth <= SIZE_MAX / sizeof(esy_event_bus_event_t).
 */
#define ESY_EVENT_BUS_QUEUE_BYTES(depth) \
  (sizeof(esy_event_bus_event_t) * (size_t)(depth))

/**
 * @brief Caller-owned subscriber slot; initialized by bus init, not the caller.
 * Keep the whole slot array resident until successful bus deinit. Never inspect,
 * copy, move or reset a live slot. A returned pointer is invalid for further
 * operations after successful unsubscribe; the slot may be reused immediately.
 */
typedef struct {
  ESY_ALIGNED_STORAGE(storage, ESY_EVENT_BUS_SUBSCRIPTION_STORAGE_BYTES); /**< Private storage. */
} esy_event_bus_subscription_t;

/**
 * @brief Caller-owned bus handle; initialize with {0} before first init.
 * Fields are implementation-owned. Never copy, clear, move or edit a live bus.
 * Successful deinit permits reinit; failed init retains no resources.
 */
typedef struct esy_event_bus_s {
  const struct esy_event_bus_s *owner; /**< Private identity guard. */
  ESY_ALIGNED_STORAGE(storage, ESY_EVENT_BUS_STORAGE_BYTES); /**< Private storage. */
} esy_event_bus_t;

/** @brief Borrowed bus backing storage; contents may initially be arbitrary. */
typedef struct {
  esy_event_bus_subscription_t *subscriptions; /**< Aligned array of unused slots. */
  size_t subscription_capacity; /**< Nonzero array element count; publish scans this count. */
} esy_event_bus_static_resources_t;

/** @brief Bus configuration, copied by init; resource descriptor need not persist. */
typedef struct {
  const esy_event_bus_static_resources_t *static_resources; /**< Required backing storage. */
} esy_event_bus_config_t;

/** @brief Subscriber queue backing storage, borrowed until unsubscribe succeeds. */
typedef struct {
  void *queue_buffer; /**< Aligned, exclusive data storage; use ESY_ALIGNED_STORAGE. */
  size_t queue_buffer_size; /**< At least ESY_EVENT_BUS_QUEUE_BYTES(queue_depth). */
} esy_event_bus_subscription_static_resources_t;

/** @brief Copied subscription configuration; one mask can match several topics. */
typedef struct {
  uint32_t topic_mask; /**< Nonzero OR of TOPIC_BIT values. */
  uint32_t queue_depth; /**< Nonzero queue capacity in complete messages. */
  const esy_event_bus_subscription_static_resources_t *static_resources; /**< Required storage. */
} esy_event_bus_subscription_config_t;

/**
 * @brief Per-publication acceptance report, always reset before validation.
 * Queue acceptance does not acknowledge consumer processing. No matches is OK.
 */
typedef struct {
  size_t matched; /**< Matching open subscribers observed during this call. */
  size_t delivered; /**< Successful queue copies. */
  size_t failed; /**< Matching subscribers whose copy could not be accepted. */
  esy_err_t first_error; /**< First underlying error, or ESY_OK when failed is zero. */
} esy_event_bus_publish_result_t;

/**
 * @brief Initialize unused bus/slot storage without creating OS resources.
 * @return INVALID_ARG for NULL, zero/overflowing capacity or misalignment;
 * STATE for a live/copied handle; otherwise OK. Failure preserves the handle.
 * The handle needs {0}; backing slots need not be initialized. Storage regions
 * must be distinct, exclusive, correctly sized, and must not overlap the bus.
 */
esy_err_t esy_event_bus_init(esy_event_bus_t *bus,
                             const esy_event_bus_config_t *config);

/**
 * @brief Close bus admission and attempt nonblocking teardown, task owner only.
 * BUSY retains all storage and keeps admission closed while publishers or
 * subscriptions remain. Unsubscribe all slots, then retry. New publish and
 * subscribe calls return STATE once closed. Stop all potential bus callers
 * before reclaiming/reinitializing storage, including callers not yet admitted.
 * @return INVALID_ARG for NULL, STATE for a copied handle, BUSY as above, or OK.
 * A zero/already-deinitialized resident handle succeeds idempotently.
 */
esy_err_t esy_event_bus_deinit(esy_event_bus_t *bus);

/**
 * @brief Create an independent static queue, task lifecycle owner only.
 * Resource descriptors are copied, buffers are borrowed. Initialize/stop/retire
 * receivers through the consumer's narrow interfaces; do not expose this handle
 * as application-private wiring. Distinct subscriptions need exclusive buffers.
 * @param out_subscription Required output, cleared on failure; may not alias inputs.
 * @return INVALID_ARG for invalid storage/mask/depth/capacity/alignment; STATE for
 * an uninitialized/closed bus; NO_MEM for no free slot; otherwise queue status.
 * Failed creation leaves the bus and buffer reusable and retains no subscription.
 */
esy_err_t esy_event_bus_subscribe(esy_event_bus_t *bus,
    const esy_event_bus_subscription_config_t *config,
    esy_event_bus_subscription_t **out_subscription);

/**
 * @brief Close one subscriber and attempt queue deletion, task owner only.
 * Admission closes before checking in-flight sends/receives. BUSY retains queue,
 * buffer and slot; retry after those operations return. No new receive enters
 * after closure. Success discards queued messages and invalidates aliases. Stop
 * prospective receivers before reclaiming the slot/buffer. Other subscribers
 * remain usable. Queue deletion and all copies occur outside the bus lock.
 * @return INVALID_ARG for NULL; STATE for an invalid bus; BUSY or OK as above.
 */
esy_err_t esy_event_bus_unsubscribe(esy_event_bus_subscription_t *subscription);

/**
 * @brief Make one nonblocking copy attempt per matching subscriber, task context.
 * Every subscriber is attempted even after failure. Membership is observed per
 * slot, not as one atomic snapshot. Concurrent publishers can interleave; there
 * is no cross-subscriber/global ordering, replay, retention or automatic retry.
 * FIFO applies to accepted messages within each queue. Work is O(slot capacity)
 * plus one bounded message copy per match, under the underlying OS guarantees.
 * @param result Required acceptance report; must not alias event or bus storage.
 * @return INVALID_ARG for NULL/invalid event; STATE for uninitialized/closed bus;
 * BUSY for any failed delivery or exhausted admission counter; otherwise OK.
 * Partial delivery cannot be rolled back; retrying may duplicate earlier copies.
 */
esy_err_t esy_event_bus_publish(esy_event_bus_t *bus,
    const esy_event_bus_event_t *event, esy_event_bus_publish_result_t *result);

/**
 * @brief ISR form of publish; never blocks, logs, allocates or yields.
 * Same result/ownership rules as publish. need_yield may be NULL and is otherwise
 * cleared even on validation failure, then ORed across all queue attempts. The
 * IRQ owner combines this with other notifications and yields exactly once.
 * Budget the configured slot count and copies against the IRQ latency limit.
 */
esy_err_t esy_event_bus_publish_from_isr(esy_event_bus_t *bus,
    const esy_event_bus_event_t *event, esy_event_bus_publish_result_t *result,
    bool *need_yield);

/**
 * @brief Receive one complete message, only from this subscription's task owner.
 * timeout_ms is 0 for polling or a finite millisecond OS wait; UINT32_MAX is
 * rejected so closure cannot leave an indefinitely blocked receiver. OS ticks,
 * rounding, saturation, wrap and suspended-tick behavior follow esy_os_queue.
 * No local clock or deadline arithmetic is added. The queue remains pinned
 * through the wait; choose a bound compatible with stop/OTA acknowledgement.
 * Output changes only on OK; closed subscribers reject new receives with STATE.
 * @return INVALID_ARG for NULL/forever timeout; STATE for closed/uninitialized;
 * BUSY for exhausted admission counter; otherwise the OS receive status.
 */
esy_err_t esy_event_bus_receive(esy_event_bus_subscription_t *subscription,
    esy_event_bus_event_t *event, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
