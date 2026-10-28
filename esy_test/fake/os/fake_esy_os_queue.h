/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fake_esy_os_queue.h
 * @brief Fake esy_os_queue contract for unit tests.
 */

#ifndef FAKE_ESY_OS_QUEUE_H
#define FAKE_ESY_OS_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esy_middleware/os/esy_os_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset the global fake queue state.
 */
void fake_esy_os_queue_reset(void);

/**
 * @brief Set the result returned by queue creation.
 */
void fake_esy_os_queue_set_create_result(esy_err_t ret);

/**
 * @brief Set the send result returned by the fake.
 */
void fake_esy_os_queue_set_send_result(esy_err_t ret);

/**
 * @brief Set the receive result returned by the fake.
 */
void fake_esy_os_queue_set_receive_result(esy_err_t ret);

/** @brief Queue-operation observations; counts include failed attempts. */
typedef struct {
  uint32_t create_count; /**< Static queue creation attempts. */
  uint32_t delete_count; /**< Successful deletions of live queues. */
  uint32_t send_count; /**< Sends, including ISR sends. */
  uint32_t send_from_isr_count; /**< ISR send attempts. */
  uint32_t receive_count; /**< Receives, including ISR receives. */
  uint32_t last_send_timeout_ms; /**< Most recent send wait argument. */
  uint32_t last_receive_timeout_ms; /**< Most recent receive wait argument. */
} fake_esy_os_queue_observations_t;

/** @brief Read observations; borrowed until the next reset or fake operation. */
const fake_esy_os_queue_observations_t *fake_esy_os_queue_get_observations(void);

/**
 * @brief Set a one-shot scheduling hook before send inspects a live queue.
 * The fake invokes it synchronously and clears it before invocation. This
 * models task/ISR interleaving, not native blocking or host-thread safety.
 */
void fake_esy_os_queue_set_send_hook(void (*hook)(void));

/** @brief Set a one-shot scheduling hook before receive inspects a live queue. */
void fake_esy_os_queue_set_receive_hook(void (*hook)(void));

/**
 * @brief Request need_yield on one successful ISR send, numbered from reset.
 * Zero disables the request. Other ISR send attempts overwrite it with false.
 */
void fake_esy_os_queue_set_isr_yield_on_call(uint32_t call);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESY_OS_QUEUE_H */
