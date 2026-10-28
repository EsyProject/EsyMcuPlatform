/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/ds/esy_prio_slot_queue.h"

#include <string.h>

/* Key invariants:
 * - No malloc/free, no OS dependency.
 * - slots[] storage is provided by the caller.
 * - queued_count tracks only QUEUED slots.
 * - QUEUED slots form a FIFO chain; ordering never depends on counter age.
 */

esy_prio_slot_queue_config_t esy_prio_slot_queue_default_cfg(void) {
  esy_prio_slot_queue_config_t cfg;
  cfg.slots = NULL;
  cfg.slot_count = 0;
  return cfg;
}

static bool slot_index_valid(const esy_prio_slot_queue_t *q, uint16_t idx) {
  return (q != NULL) && (idx < q->slot_count) && (q->slots != NULL);
}

static void append_queued(esy_prio_slot_queue_t *q, uint16_t idx) {
  esy_prio_slot_t *slot = &q->slots[idx];
  slot->fifo_prev = q->fifo_tail;
  slot->fifo_next = -1;
  if (q->fifo_tail >= 0) {
    q->slots[q->fifo_tail].fifo_next = (int16_t)idx;
  } else {
    q->fifo_head = (int16_t)idx;
  }
  q->fifo_tail = (int16_t)idx;
  q->queued_count++;
}

static void unlink_queued(esy_prio_slot_queue_t *q, uint16_t idx) {
  esy_prio_slot_t *slot = &q->slots[idx];
  if (slot->fifo_prev >= 0) {
    q->slots[slot->fifo_prev].fifo_next = slot->fifo_next;
  } else {
    q->fifo_head = slot->fifo_next;
  }
  if (slot->fifo_next >= 0) {
    q->slots[slot->fifo_next].fifo_prev = slot->fifo_prev;
  } else {
    q->fifo_tail = slot->fifo_prev;
  }
  slot->fifo_prev = -1;
  slot->fifo_next = -1;
  q->queued_count--;
}

static void reset_slot(esy_prio_slot_t *slot) {
  if (!slot) {
    return;
  }

  slot->state = ESY_PRIO_SLOT_STATE_FREE;
  slot->priority = 0;
  slot->fifo_prev = -1;
  slot->fifo_next = -1;
  slot->user_data = (uintptr_t)0;
}

esy_err_t esy_prio_slot_queue_init(esy_prio_slot_queue_t *q,
                                   const esy_prio_slot_queue_config_t *cfg) {
  if (!q || !cfg) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!cfg->slots || cfg->slot_count == 0u ||
      cfg->slot_count > ESY_PRIO_SLOT_QUEUE_MAX_SLOT_COUNT) {
    return ESY_ERR_INVALID_ARG;
  }

  memset(q, 0, sizeof(*q));

  q->slots = cfg->slots;
  q->slot_count = cfg->slot_count;
  q->queued_count = 0;
  q->fifo_head = -1;
  q->fifo_tail = -1;
  q->initialized = true;

  /* Initialize all slots to FREE. */
  for (uint16_t i = 0; i < q->slot_count; ++i) {
    reset_slot(&q->slots[i]);
  }

  return ESY_OK;
}

void esy_prio_slot_queue_deinit(esy_prio_slot_queue_t *q) {
  if (!q) {
    return;
  }

  /* We do not own slots memory; just drop pointers and clear bookkeeping. */
  q->slots = NULL;
  q->slot_count = 0;
  q->queued_count = 0;
  q->fifo_head = -1;
  q->fifo_tail = -1;
  q->initialized = false;
}

bool esy_prio_slot_queue_is_initialized(const esy_prio_slot_queue_t *q) {
  return (q != NULL) && q->initialized && (q->slots != NULL) &&
         (q->slot_count > 0u);
}

int16_t esy_prio_slot_queue_alloc_slot(esy_prio_slot_queue_t *q,
                                       uintptr_t user_data) {
  if (!esy_prio_slot_queue_is_initialized(q)) {
    return -1;
  }

  for (uint16_t i = 0; i < q->slot_count; ++i) {
    esy_prio_slot_t *s = &q->slots[i];
    if (s->state == ESY_PRIO_SLOT_STATE_FREE) {
      s->state = ESY_PRIO_SLOT_STATE_RESERVED;
      s->priority = 0;
      s->fifo_prev = -1;
      s->fifo_next = -1;
      s->user_data = user_data;
      return (int16_t)i;
    }
  }
  return -1;
}

void esy_prio_slot_queue_free_slot(esy_prio_slot_queue_t *q,
                                   uint16_t slot_index) {
  if (!esy_prio_slot_queue_is_initialized(q)) {
    return;
  }
  if (!slot_index_valid(q, slot_index)) {
    return;
  }

  esy_prio_slot_t *s = &q->slots[slot_index];

  if (s->state == ESY_PRIO_SLOT_STATE_QUEUED) {
    unlink_queued(q, slot_index);
  }

  reset_slot(s);
}

esy_err_t esy_prio_slot_queue_enqueue(esy_prio_slot_queue_t *q,
                                      uint16_t slot_index, uint8_t priority) {
  if (!esy_prio_slot_queue_is_initialized(q)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!slot_index_valid(q, slot_index)) {
    return ESY_ERR_INVALID_ARG;
  }

  esy_prio_slot_t *s = &q->slots[slot_index];

  /* Allowed transitions:
   * - RESERVED -> QUEUED
   * - FREE -> QUEUED (permissive; caller may skip alloc_slot if desired)
   */
  if (!(s->state == ESY_PRIO_SLOT_STATE_RESERVED ||
        s->state == ESY_PRIO_SLOT_STATE_FREE)) {
    return ESY_ERR_STATE;
  }

  s->state = ESY_PRIO_SLOT_STATE_QUEUED;
  s->priority = priority;
  append_queued(q, slot_index);

  return ESY_OK;
}

int16_t esy_prio_slot_queue_pick_next(const esy_prio_slot_queue_t *q) {
  if (!esy_prio_slot_queue_is_initialized(q)) {
    return -1;
  }
  if (q->queued_count == 0u) {
    return -1;
  }

  int16_t best_idx = -1;
  uint8_t best_prio = 0;
  for (int16_t i = q->fifo_head; i >= 0; i = q->slots[i].fifo_next) {
    const esy_prio_slot_t *s = &q->slots[i];
    /* Visit in FIFO order; only a strictly higher priority replaces the best. */
    if (best_idx < 0 || s->priority > best_prio) {
      best_idx = i;
      best_prio = s->priority;
    }
  }

  return best_idx;
}

void esy_prio_slot_queue_mark_in_progress(esy_prio_slot_queue_t *q,
                                          uint16_t slot_index) {
  if (!esy_prio_slot_queue_is_initialized(q)) {
    return;
  }
  if (!slot_index_valid(q, slot_index)) {
    return;
  }

  esy_prio_slot_t *s = &q->slots[slot_index];

  if (s->state == ESY_PRIO_SLOT_STATE_QUEUED) {
    unlink_queued(q, slot_index);
    s->state = ESY_PRIO_SLOT_STATE_IN_PROGRESS;
    return;
  }

  /* If already IN_PROGRESS, keep it as-is; other states: ignore. */
}

esy_err_t esy_prio_slot_queue_requeue(esy_prio_slot_queue_t *q,
                                      uint16_t slot_index, uint8_t priority) {
  if (!esy_prio_slot_queue_is_initialized(q)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!slot_index_valid(q, slot_index)) {
    return ESY_ERR_INVALID_ARG;
  }

  esy_prio_slot_t *s = &q->slots[slot_index];
  if (s->state != ESY_PRIO_SLOT_STATE_IN_PROGRESS) {
    return ESY_ERR_STATE;
  }

  s->state = ESY_PRIO_SLOT_STATE_QUEUED;
  s->priority = priority;
  append_queued(q, slot_index);

  return ESY_OK;
}

uint16_t esy_prio_slot_queue_get_depth(const esy_prio_slot_queue_t *q) {
  if (!esy_prio_slot_queue_is_initialized(q)) {
    return 0;
  }
  return q->queued_count;
}

esy_prio_slot_state_t esy_prio_slot_queue_get_slot_state(
    const esy_prio_slot_queue_t *q, uint16_t slot_index) {
  if (!esy_prio_slot_queue_is_initialized(q)) {
    return ESY_PRIO_SLOT_STATE_FREE;
  }
  if (!slot_index_valid(q, slot_index)) {
    return ESY_PRIO_SLOT_STATE_FREE;
  }
  return q->slots[slot_index].state;
}

uintptr_t esy_prio_slot_queue_get_slot_user_data(const esy_prio_slot_queue_t *q,
                                                 uint16_t slot_index) {
  if (!esy_prio_slot_queue_is_initialized(q)) {
    return (uintptr_t)0;
  }
  if (!slot_index_valid(q, slot_index)) {
    return (uintptr_t)0;
  }
  return q->slots[slot_index].user_data;
}

int16_t esy_prio_slot_queue_find_by_user_data(const esy_prio_slot_queue_t *q,
                                              uintptr_t user_data) {
  if (!esy_prio_slot_queue_is_initialized(q)) {
    return -1;
  }

  for (uint16_t i = 0; i < q->slot_count; ++i) {
    const esy_prio_slot_t *s = &q->slots[i];
    if (s->state == ESY_PRIO_SLOT_STATE_FREE) {
      continue;
    }

    if (s->user_data == user_data) {
      return (int16_t)i;
    }
  }
  return -1;
}
