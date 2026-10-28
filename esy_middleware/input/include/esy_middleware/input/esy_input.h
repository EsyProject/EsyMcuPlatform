/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_INPUT_H
#define ESY_INPUT_H

#include <stdbool.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_input.h
 * @brief Caller-driven buttons and quadrature encoders with a shared event sink.
 *
 * One main-loop/task owner serializes every call and callback per instance.
 * No entry point is ISR-safe. Callbacks run synchronously inside sample(), must
 * return promptly, and must not reenter any API on that instance. The module
 * has no worker, queue, allocator, hardware or OS dependency. Sampling work is
 * constant, plus at most three button callbacks or one encoder callback.
 *
 * Time is caller-supplied monotonic uint32_t milliseconds, modulo 2^32 (about
 * 49.7 days). Each sample gap must be <= INT32_MAX ms, including idle periods;
 * sample much faster than the shortest physical pulse/encoder edge to avoid
 * losing input. Durations are <= INT32_MAX ms. Modular subtraction is valid
 * because each pending timer is observed within duration + maximum sample gap,
 * strictly below a full cycle. Completed/cancelled timers have explicit inactive
 * state and cannot reactivate after a clock wrap. The clock must keep advancing
 * in sleep if elapsed sleep should count; otherwise timing intentionally freezes.
 * Deinit cancels all pending gestures without callbacks. After a clock reset or
 * an unbounded sampling/OTA/sleep pause, deinit and init before sampling again.
 * An observable backwards/oversized gap returns STATE with no state changes;
 * an unobserved full clock cycle cannot be detected from uint32_t timestamps.
 *
 * Initialize handles with {0}. Never copy, move, clear or modify a live handle.
 * Configurations are copied; sink contexts remain borrowed until deinit returns.
 * Init failures preserve the handle. Repeated init returns STATE; deinit is
 * idempotent for zero handles and permits subsequent init. Stop the sampling
 * owner before deinit; there is no asynchronous cleanup or stop acknowledgement.
 */

/** @brief Unified event kind; codes and source IDs are assigned by the consumer. */
typedef enum {
  ESY_INPUT_EVENT_KEY = 0,       /**< Debounced key: value 1 pressed, 0 released. */
  ESY_INPUT_EVENT_CLICK,         /**< One short press/release: value 1. */
  ESY_INPUT_EVENT_DOUBLE_CLICK,  /**< Two short releases inside the window: value 2. */
  ESY_INPUT_EVENT_LONG_PRESS,    /**< Once per held press: value 1; suppresses click. */
  ESY_INPUT_EVENT_RELATIVE       /**< Encoder movement: value +1 or -1. */
} esy_input_event_type_t;

/** @brief One complete input change; no multi-axis partial frames are emitted. */
typedef struct {
  esy_input_event_type_t type; /**< Event/gesture kind. */
  uint16_t source_id;          /**< Consumer-defined device identifier. */
  uint16_t code;               /**< Consumer-defined key or relative-axis code. */
  int32_t value;               /**< Meaning documented by the event kind. */
  uint32_t timestamp_ms;       /**< Sample time at which this event was recognized. */
} esy_input_event_t;

/**
 * @brief Required synchronous event sink; event is borrowed only until return.
 * @param event Complete immutable event; copy it before retaining/queueing it.
 * @param arg Borrowed sink context; may be NULL.
 * The consumer owns any queue capacity, overflow reporting and fanout. Cross-task
 * GUI delivery must use its existing bounded request/queue interface. The sink
 * must not call LVGL from a non-GUI owner or run another task's business loop.
 */
typedef void (*esy_input_event_callback_t)(const esy_input_event_t *event,
                                         void *arg);

/** @brief Button configuration, copied by init; zero durations disable gestures. */
typedef struct {
  uint16_t source_id; /**< Device ID copied to every event. */
  uint16_t code; /**< Key code copied to every event. */
  uint32_t debounce_ms; /**< Stable candidate duration, 0..INT32_MAX; zero is immediate. */
  uint32_t long_press_ms; /**< Debounced hold duration, 1..INT32_MAX; zero disables. */
  uint32_t double_click_ms; /**< Window between short releases, 1..INT32_MAX; zero disables. */
  esy_input_event_callback_t on_event; /**< Required synchronous sink. */
  void *event_arg; /**< Borrowed sink argument. */
} esy_input_button_config_t;

/** @brief Caller-owned button; all fields are implementation-owned after init. */
typedef struct esy_input_button_s {
  const struct esy_input_button_s *owner; /**< Identity guard; do not modify. */
  esy_input_button_config_t config; /**< Private copied configuration. */
  uint32_t last_sample_ms; /**< Private sample-order guard. */
  uint32_t candidate_since_ms; /**< Private debounce origin. */
  uint32_t pressed_since_ms; /**< Private hold origin. */
  uint32_t released_since_ms; /**< Private click origin. */
  bool sampled; /**< Private clock validity. */
  bool stable_pressed; /**< Private debounced level; initially released. */
  bool candidate_pressed; /**< Private latest raw level. */
  bool debouncing; /**< Private debounce timer enable. */
  bool long_pending; /**< Private one-shot hold timer enable. */
  bool long_emitted; /**< Private current-press click suppression. */
  bool click_pending; /**< Private deferred single-click enable. */
  bool busy; /**< Private callback reentry guard. */
} esy_input_button_t;

/** @brief Encoder configuration; decoding accumulates legal adjacent Gray edges. */
typedef struct {
  uint16_t source_id; /**< Device ID copied to every event. */
  uint16_t code; /**< Relative-axis code copied to every event. */
  uint8_t edges_per_step; /**< Exactly 1, 2 or 4 adjacent transitions per event. */
  bool inverted; /**< Reverse the reported direction. */
  esy_input_event_callback_t on_event; /**< Required synchronous sink. */
  void *event_arg; /**< Borrowed sink argument. */
} esy_input_encoder_config_t;

/** @brief Caller-owned encoder; all fields are implementation-owned after init. */
typedef struct esy_input_encoder_s {
  const struct esy_input_encoder_s *owner; /**< Identity guard; do not modify. */
  esy_input_encoder_config_t config; /**< Private copied configuration. */
  uint32_t last_sample_ms; /**< Private sample-order guard. */
  uint8_t previous_ab; /**< Private previous two-bit Gray state. */
  int8_t accumulated_edges; /**< Private signed partial movement. */
  bool sampled; /**< Private initial-state/clock validity. */
  bool busy; /**< Private callback reentry guard. */
} esy_input_encoder_t;

/**
 * @brief Initialize a released button without reading hardware or emitting events.
 * @param button Exclusive {0} or successfully deinitialized storage.
 * @param config Required sink and durations in [0, INT32_MAX].
 * @return OK, INVALID_ARG for NULL/invalid config, or STATE for live/copied storage.
 * A button held on the first sample is recognized after its debounce interval.
 */
esy_err_t esy_input_button_init(esy_input_button_t *button,
                              const esy_input_button_config_t *config);

/**
 * @brief Cancel pending gestures and release the borrowed sink without events.
 * @param button Exclusive initialized or zero handle; NULL is invalid.
 * @return OK (also on repeated deinit), INVALID_ARG, STATE for a copied handle,
 * or BUSY from its callback. Success restores {0}; no hardware is changed.
 */
esy_err_t esy_input_button_deinit(esy_input_button_t *button);

/**
 * @brief Submit a logical pressed level and advance button timers once.
 * @param button Initialized exclusive handle.
 * @param pressed Logical level after consumer/provider polarity conversion.
 * @param now_ms Monotonic wrapping milliseconds satisfying the file contract.
 * @return OK, INVALID_ARG (NULL), STATE (uninitialized/copied/time discontinuity),
 * or BUSY (callback reentry). Errors leave state unchanged.
 * KEY precedes the click gesture on release. Hold expiry is evaluated before a
 * release at the same sample, so a hold exactly long_press_ms is long. A pending
 * click expires before a second release at exactly double_click_ms; DOUBLE_CLICK
 * requires strictly less than the window. Single CLICK is deferred until that
 * window expires, or immediate on release when double-click is disabled. Long
 * presses produce KEY release but no CLICK/DOUBLE_CLICK for that press. No repeat
 * long event is generated, even across subsequent full clock cycles.
 */
esy_err_t esy_input_button_sample(esy_input_button_t *button, bool pressed,
                                uint32_t now_ms);

/**
 * @brief Initialize encoder state; the first sample seeds AB without movement.
 * @param encoder Exclusive {0} or successfully deinitialized storage.
 * @param config Required sink, with edges_per_step equal to 1, 2 or 4.
 * @return OK, INVALID_ARG for NULL/invalid config, or STATE for live storage.
 * Failures preserve storage. No hardware or callback is accessed.
 */
esy_err_t esy_input_encoder_init(esy_input_encoder_t *encoder,
                               const esy_input_encoder_config_t *config);

/**
 * @brief Discard partial motion and release the sink, without events.
 * @param encoder Exclusive initialized or zero handle; NULL is invalid.
 * @return OK (including repeated deinit), INVALID_ARG, STATE for copied storage,
 * or BUSY during its callback. Success restores {0} and permits init again.
 */
esy_err_t esy_input_encoder_deinit(esy_input_encoder_t *encoder);

/**
 * @brief Submit one coherent two-bit AB state (A in bit 1, B in bit 0).
 * @param encoder Initialized exclusive handle.
 * @param ab State in [0, 3]. Positive order is 00 -> 01 -> 11 -> 10 -> 00.
 * @param now_ms Wrapping milliseconds satisfying the file contract.
 * @return OK, INVALID_ARG for NULL/AB range, STATE for handle/time error,
 * BUSY for callback reentry, or IO for a skipped/opposite two-bit transition.
 * IO resynchronizes to AB and discards partial motion, without emitting an event;
 * other errors leave state unchanged. Adjacent reversals cancel accumulated
 * edges. There is no time-based encoder debounce: contact bounce retraces edges.
 * Poll fast enough to observe every adjacent edge; missed full cycles cannot be
 * detected and are not reconstructed. No acceleration policy is applied.
 */
esy_err_t esy_input_encoder_sample(esy_input_encoder_t *encoder, uint8_t ab,
                                 uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif /* ESY_INPUT_H */
