/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fake_segger_rtt.h
 * @brief Reusable fake implementation contract for SEGGER RTT tests.
 */

#ifndef FAKE_SEGGER_RTT_H
#define FAKE_SEGGER_RTT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FAKE_SEGGER_RTT_CAPTURE_SIZE
#define FAKE_SEGGER_RTT_CAPTURE_SIZE 256u
#endif

/**
 * @brief Observable fake SEGGER RTT state.
 */
typedef struct {
  uint8_t capture[FAKE_SEGGER_RTT_CAPTURE_SIZE];
  size_t capture_len;
  size_t write_count;
  unsigned forced_write_len;
  unsigned last_buffer_index;
  void (*write_hook)(void); /**< Synchronous write hook; cleared by reset. */
} fake_segger_rtt_state_t;

void fake_segger_rtt_reset(void);
fake_segger_rtt_state_t *fake_segger_rtt_get_state(void);
void fake_segger_rtt_set_forced_write_len(unsigned len);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_SEGGER_RTT_H */
