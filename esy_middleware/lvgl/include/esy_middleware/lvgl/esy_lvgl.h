/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_lvgl.h
 * @brief Single-owner LVGL runtime using caller-owned OS and drawing storage.
 *
 * The implementation adapts LVGL 9.6.0, RGB565/PARTIAL, one display and optional
 * pointer input. The consumer supplies the public lvgl target. UI callbacks may
 * include its real headers; no substitute LVGL types are declared here.
 */
#ifndef ESY_LVGL_H
#define ESY_LVGL_H

#include <stdbool.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/lcd/esy_lcd.h"
#include "esy_middleware/os/esy_os_queue.h"
#include "esy_middleware/os/esy_os_task.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque context capacity; rebuild consumers after layout changes. */
#define ESY_LVGL_PRIV_BYTES 384u
/** @brief Default number of queued UI requests. */
#define ESY_LVGL_DEFAULT_EVENT_QUEUE_DEPTH 16u

/**
 * @brief Caller-owned handle. Initialize with {0} before first create.
 * Never move, copy or reset a live handle. Only successful destroy releases its
 * storage. Create/start/destroy have one externally serialized task-context owner;
 * GUI callbacks may also request stop.
 */
typedef struct {
  ESY_ALIGNED_STORAGE(priv, ESY_LVGL_PRIV_BYTES);
} esy_lvgl_t;

/** @brief Observable runtime states. STOPPED/FAILED still require destroy. */
typedef enum {
  ESY_LVGL_INITIALIZED = 0, /**< Resources reserved, no task started. */
  ESY_LVGL_STARTING,        /**< Task launched, UI not yet ready. */
  ESY_LVGL_RUNNING,         /**< UI requests may be submitted. */
  ESY_LVGL_STOPPING,        /**< Submission closed; cleanup is in progress. */
  ESY_LVGL_STOPPED,         /**< GUI cleanup finished; task is parking. */
  ESY_LVGL_FAILED           /**< GUI cleanup finished after a latched error. */
} esy_lvgl_state_t;

/** @brief Exactly one terminal result for each accepted event. */
typedef enum {
  ESY_LVGL_EVENT_EXECUTED = 0, /**< callback returned normally. */
  ESY_LVGL_EVENT_CANCELLED     /**< callback was not started. */
} esy_lvgl_event_result_t;

/**
 * @brief Asynchronous UI request, copied into an MPSC OS queue.
 * callback and complete execute only on the GUI task and must return promptly.
 * Both are required. arg is borrowed until complete returns; complete may free
 * it. Neither callback may destroy the runtime, wait for GUI work, consume its
 * OS task notifications, or change the adapter's display/flush configuration.
 * callback may use LVGL and enqueue subsequent work. Stop cancels queued work.
 */
typedef struct {
  void (*callback)(void *arg); /**< Bounded UI operation. */
  void (*complete)(esy_lvgl_event_result_t result, void *arg); /**< Terminal callback. */
  void *arg; /**< Borrowed application context. */
} esy_lvgl_event_t;

/**
 * @brief Backing storage borrowed until destroy succeeds.
 * Buffers need no initial clearing; context requires {0}. Drawing buffers are
 * writable native RGB565, aligned to the provider's LV_DRAW_BUF_ALIGN, at least
 * width * 2 bytes each. Two drawing buffers must not overlap. All storage objects
 * must be distinct, unmoved and alive; the LCD backend must consume RGB565.
 */
typedef struct {
  esy_lvgl_t *context; /**< Zero-initialized context. */
  void *task_stack_buffer; /**< OS-aligned stack. */
  uint32_t task_stack_buffer_size; /**< Stack bytes, a valid OS stack word multiple. */
  esy_os_task_static_storage_t *task_control_storage; /**< Native task storage. */
  esy_lvgl_event_t *event_queue_buffer; /**< Queue item backing array. */
  uint32_t event_queue_depth; /**< Number of entries, nonzero. */
  esy_os_queue_static_storage_t *event_queue_control_storage; /**< Queue storage. */
  uint8_t *draw_buffer1; /**< Required writable drawing buffer. */
  uint8_t *draw_buffer2; /**< Optional equally sized drawing buffer. */
  uint32_t draw_buffer_size; /**< Capacity of each drawing buffer in bytes. */
} esy_lvgl_static_resources_t;

/**
 * @brief Declare initialized context/OS storage; drawing buffers bind separately.
 * @param name Object name; keep alive until destroy succeeds.
 * @param depth Queue capacity.
 * @param stack_bytes OS stack capacity in bytes.
 */
#define ESY_LVGL_STATIC_STORAGE(name, depth, stack_bytes)                    \
  struct {                                                                 \
    esy_lvgl_t context;                                                     \
    ESY_ALIGNED_STORAGE(task_stack_buffer, (stack_bytes));                   \
    esy_os_task_static_storage_t task_control_storage;                       \
    esy_lvgl_event_t event_queue_buffer[(depth)];                            \
    esy_os_queue_static_storage_t event_queue_control_storage;               \
  } name = {0}

/**
 * @brief Bind OS storage, clearing the descriptor including draw buffer fields.
 * @param resources Descriptor pointer.
 * @param storage Object declared with ESY_LVGL_STATIC_STORAGE.
 */
#define ESY_LVGL_BIND_STATIC_RESOURCES(resources, storage)                   \
  do {                                                                     \
    (resources)->context = &(storage).context;                              \
    (resources)->task_stack_buffer = (storage).task_stack_buffer.bytes;      \
    (resources)->task_stack_buffer_size =                                   \
        sizeof((storage).task_stack_buffer.bytes);                          \
    (resources)->task_control_storage = &(storage).task_control_storage;    \
    (resources)->event_queue_buffer = (storage).event_queue_buffer;          \
    (resources)->event_queue_depth = sizeof((storage).event_queue_buffer) / \
        sizeof((storage).event_queue_buffer[0]);                            \
    (resources)->event_queue_control_storage =                              \
        &(storage).event_queue_control_storage;                             \
    (resources)->draw_buffer1 = NULL;                                       \
    (resources)->draw_buffer2 = NULL;                                       \
    (resources)->draw_buffer_size = 0u;                                     \
  } while (0)

/** @brief RGB565 byte order required by the LCD transport, independent of CPU endian. */
typedef enum {
  ESY_LVGL_RGB565_MSB_FIRST = 0, /**< Default: high byte, then low byte. */
  ESY_LVGL_RGB565_LSB_FIRST = 1 /**< Low byte, then high byte. */
} esy_lvgl_rgb565_byte_order_t;

/**
 * @brief Optional pointer provider, copied by create.
 *
 * read runs synchronously on the GUI task only. It must validate its output
 * pointers, write display coordinates and pressed on success, and return an
 * error otherwise. A failed read is presented as released; previous output is
 * ignored. The callback must return promptly and must not reenter lifecycle or
 * wait for GUI work. Its argument remains borrowed until destroy succeeds.
 * No callback is invoked after GUI cleanup; no task or resource is created here.
 */
typedef struct {
  esy_err_t (*read)(uint16_t *x, uint16_t *y, bool *pressed, void *arg); /**< Required. */
  void *arg; /**< Borrowed provider context, which may be NULL. */
} esy_lvgl_pointer_input_t;

/**
 * @brief Runtime configuration, copied by create (including the descriptor).
 * LCD, pointer-provider context and ui_arg remain borrowed until destroy succeeds.
 * Both UI
 * hooks are optional as a pair. on_start runs after display/input registration;
 * its negative result starts cleanup. on_stop runs whenever on_start was entered,
 * including partial startup failure. It must remove application timers and
 * external callbacks referencing UI objects before the adapter deletes displays.
 * Hooks run on the GUI task. No other task may call LVGL, lv_init or lv_deinit.
 */
typedef struct {
  uint16_t width; /**< Display width, nonzero. */
  uint16_t height; /**< Display height, nonzero. */
  esy_lcd_t *lcd; /**< Attached RGB565 LCD. */
  const esy_lvgl_pointer_input_t *pointer_input; /**< Optional descriptor; NULL disables input. */
  uint32_t task_priority; /**< OS task priority. */
  uint8_t task_core_affinity; /**< OS core affinity mask. */
  uint32_t event_budget; /**< Maximum events per iteration, nonzero. */
  uint32_t flush_timeout_ms; /**< Error deadline, 1..INT32_MAX milliseconds. */
  esy_err_t (*on_start)(void *arg); /**< Optional UI initialization. */
  void (*on_stop)(void *arg); /**< Paired partial/final UI cleanup. */
  void *ui_arg; /**< Borrowed UI hook context. */
  const esy_lvgl_static_resources_t *static_resources; /**< Required resources. */
  esy_lvgl_rgb565_byte_order_t rgb565_byte_order; /**< LCD wire byte order.
      * Defaults to MSB first; conversion is restored after completion or
      * rejection. No other layer may swap the draw buffer. */
} esy_lvgl_config_t;

/** @brief Atomic observations; last_error is the first asynchronous error. */
typedef struct {
  esy_lvgl_state_t state; /**< State sampled before last_error. */
  esy_err_t last_error; /**< ESY_OK or latched startup/flush error. */
} esy_lvgl_status_t;

/**
 * @brief Set defaults; NULL is a no-op. Supply dimensions, LCD and resources.
 * @param[out] config Configuration to populate.
 */
void esy_lvgl_default_config(esy_lvgl_config_t *config);

/**
 * @brief Reserve singleton resources; does not start a task or call LVGL.
 * @param[in] config Configuration, copied on success.
 * @param[out] out_lvgl Created handle. Must not alias context/config storage.
 * @retval ESY_OK INITIALIZED; call start separately.
 * @retval ESY_ERR_STATE Live storage or another reserved instance; output unchanged.
 * Also returned for an unattached LCD, in which case output is NULL.
 * @retval ESY_ERR_INVALID_ARG Invalid configuration; output NULL.
 * @retval ESY_ERR_NOT_SUPPORTED Port cannot park tasks; output NULL.
 * @retval <0 OS creation failure; output NULL, no resources retained.
 */
esy_err_t esy_lvgl_create(const esy_lvgl_config_t *config, esy_lvgl_t **out_lvgl);

/**
 * @brief Launch the GUI task asynchronously. Poll get_status for RUNNING/failure.
 * @param[in,out] lvgl Initialized handle. Owner task only, serialized with stop.
 * @retval ESY_OK Task created; not a UI readiness guarantee.
 * @retval ESY_ERR_STATE Not INITIALIZED.
 * @retval ESY_ERR_INVALID_ARG NULL/uncreated handle.
 * @retval <0 OS failure; remains INITIALIZED and can retry or destroy.
 */
esy_err_t esy_lvgl_start(esy_lvgl_t *lvgl);

/**
 * @brief Close submission and request cancellation/cleanup; task context only.
 * @param[in,out] lvgl Created handle. Serialized lifecycle owner or GUI callback.
 * @retval ESY_OK Stop requested or already terminal; not a reclamation guarantee.
 * @retval ESY_ERR_INVALID_ARG Invalid handle.
 * Pending DMA must finish even after timeout; resources are never forcibly freed.
 * Stop external producers before destroy. OTA must await successful destroy before
 * Flash operations requiring GUI silence, retaining transport workers until then.
 */
esy_err_t esy_lvgl_request_stop(esy_lvgl_t *lvgl);

/**
 * @brief Observe readiness/error from any task; exclude concurrent destroy.
 * @param[in] lvgl Created handle.
 * @param[out] status Non-NULL output. Fields are individual atomic observations.
 * @retval ESY_OK Snapshot written. @retval ESY_ERR_INVALID_ARG Invalid argument.
 */
esy_err_t esy_lvgl_get_status(esy_lvgl_t *lvgl, esy_lvgl_status_t *status);

/**
 * @brief Reclaim resources after confirmed parking; never executes LVGL itself.
 * Externally exclude all API callers including ISR producers and status readers.
 * GUI callbacks must not call destroy. NULL/already destroyed is a no-op.
 * @param[in,out] lvgl Handle to reclaim.
 * @retval ESY_OK Storage reusable; LCD and pointer provider remain caller-owned.
 * @retval ESY_ERR_BUSY Stop/cleanup/parking/publication is not complete; retain all storage.
 * @retval ESY_ERR_STATE Called from the GUI task.
 * @retval <0 OS deletion failure; retain ownership and retry.
 */
esy_err_t esy_lvgl_destroy(esy_lvgl_t *lvgl);

/**
 * @brief Nonblocking MPSC submission; task context. Descriptor is copied.
 * @param[in] lvgl Running handle. Exclude destroy for the entire call.
 * @param[in] event Request with non-NULL callback and complete.
 * @retval ESY_OK Accepted; exactly one terminal completion, possibly before return.
 * @retval ESY_ERR_STATE Not running/stopping. @retval ESY_ERR_BUSY Publisher limit.
 * @retval ESY_ERR_INVALID_ARG Invalid input. @retval <0 Queue rejection; retains nothing.
 * Notification failure after queue acceptance does not revoke acceptance.
 */
esy_err_t esy_lvgl_send_event(esy_lvgl_t *lvgl, const esy_lvgl_event_t *event);

/**
 * @brief ISR counterpart of send_event; no LVGL operations execute in the ISR.
 * @param[in] lvgl Running handle, kept alive until ISR producers are stopped.
 * @param[in] event Copied descriptor with context alive until terminal completion.
 * @param[out] need_yield Optional OR of queue/task wake flags; initialized false.
 * @return Same ownership/status contract as send_event, with OS ISR errors.
 * Caller performs the single yield after completing all ISR notifications.
 * Native ports use always-lock-free atomics. RP2040 uses IRQ-masked SDK atomic
 * primitives which may briefly spin on another core; no RTOS wait is performed.
 * NMI/fault handlers and core halt/reset during lock ownership are excluded.
 * Qualify queue/atomic contention and ISR latency on the target.
 */
esy_err_t esy_lvgl_send_event_from_isr(esy_lvgl_t *lvgl,
                                     const esy_lvgl_event_t *event,
                                     bool *need_yield);

#ifdef __cplusplus
}
#endif
#endif
