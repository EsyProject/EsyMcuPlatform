/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_segger_rtt.h"

#include <string.h>

#include "SEGGER_RTT.h"

static fake_segger_rtt_state_t s_state;

void fake_segger_rtt_reset(void) { memset(&s_state, 0, sizeof(s_state)); }

fake_segger_rtt_state_t *fake_segger_rtt_get_state(void) { return &s_state; }

void fake_segger_rtt_set_forced_write_len(unsigned len) {
  s_state.forced_write_len = len;
}

unsigned SEGGER_RTT_Write(unsigned buffer_index, const void *buffer,
                          unsigned num_bytes) {
  s_state.write_count++;
  s_state.last_buffer_index = buffer_index;
  if (s_state.write_hook) s_state.write_hook();

  if (buffer == NULL) {
    return 0u;
  }

  unsigned accepted = s_state.forced_write_len != 0u
                          ? s_state.forced_write_len
                          : num_bytes;
  if (accepted > num_bytes) {
    accepted = num_bytes;
  }

  size_t free_len = sizeof(s_state.capture) - s_state.capture_len;
  size_t copy_len = (size_t)accepted < free_len ? (size_t)accepted : free_len;
  if (copy_len > 0u) {
    memcpy(&s_state.capture[s_state.capture_len], buffer, copy_len);
    s_state.capture_len += copy_len;
  }

  return accepted;
}
