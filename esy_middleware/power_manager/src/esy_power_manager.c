/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/power_manager/esy_power_manager.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define ESY_POWER_MANAGER_MAGIC               0x4553504du
#define ESY_POWER_LOCK_MAGIC                  0x4553504cu
#define ESY_POWER_QUIESCE_MAGIC               0x45535051u
#define ESY_WAKE_SOURCE_MAGIC                 0x45535753u
#define ESY_POWER_CONSTRAINT_MAGIC            0x45535043u

#define ESY_POWER_TRANSACTION_PHASE_EMPTY     0u
#define ESY_POWER_TRANSACTION_PHASE_PREPARED  1u
#define ESY_POWER_TRANSACTION_PHASE_ENTERED   2u
#define ESY_POWER_TRANSACTION_PHASE_RECOVERED 3u
#define ESY_POWER_TRANSACTION_PHASE_DONE      4u

typedef enum {
  POWER_MANAGER_PHASE_UNINITIALIZED = 0,
  POWER_MANAGER_PHASE_CONFIGURING,
  POWER_MANAGER_PHASE_READY,
  POWER_MANAGER_PHASE_TRANSITIONING,
  POWER_MANAGER_PHASE_FAULTED
} power_manager_phase_t;

/* Sequential consistency orders phase admission against concurrent lock counts. */
static uint32_t atomic_load_u32(const esy_power_atomic_u32_t *value) {
  return atomic_load_explicit(value, memory_order_seq_cst);
}

static void atomic_store_u32(esy_power_atomic_u32_t *value, uint32_t next) {
  atomic_store_explicit(value, next, memory_order_seq_cst);
}

static bool atomic_cas_u32(esy_power_atomic_u32_t *value, uint32_t *expected,
                           uint32_t desired) {
  return atomic_compare_exchange_strong_explicit(
      value, expected, desired, memory_order_seq_cst, memory_order_seq_cst);
}

static bool manager_is_valid(const esy_power_manager_t *manager) {
  return (manager != NULL) && (manager->magic == ESY_POWER_MANAGER_MAGIC) &&
         (manager->device_registry != NULL) &&
         (manager->static_resources != NULL);
}

static bool lock_is_valid(const esy_power_lock_t *lock) {
  return (lock != NULL) && (lock->magic == ESY_POWER_LOCK_MAGIC) &&
         manager_is_valid(lock->manager) &&
         (lock->manager_index < lock->manager->lock_count) &&
         (lock->manager->static_resources->locks[lock->manager_index] == lock);
}

static bool participant_is_valid(
    const esy_power_quiesce_participant_t *participant) {
  return (participant != NULL) &&
         (participant->magic == ESY_POWER_QUIESCE_MAGIC) &&
         manager_is_valid(participant->manager) &&
         (participant->manager_index <
          participant->manager->participant_count) &&
         (participant->manager->static_resources
              ->participants[participant->manager_index] == participant);
}

static bool wake_source_is_valid(const esy_wake_source_t *source) {
  return (source != NULL) && (source->magic == ESY_WAKE_SOURCE_MAGIC) &&
         manager_is_valid(source->manager) &&
         (source->manager_index < source->manager->wake_source_count) &&
         (source->manager->static_resources
              ->wake_sources[source->manager_index] == source);
}

static bool constraint_is_valid(const esy_power_constraint_t *constraint) {
  return (constraint != NULL) &&
         (constraint->magic == ESY_POWER_CONSTRAINT_MAGIC) &&
         manager_is_valid(constraint->manager) &&
         (constraint->manager_index < constraint->manager->constraint_count) &&
         (constraint->manager->static_resources
              ->constraints[constraint->manager_index] == constraint);
}

static uint32_t valid_state_mask(void) {
  return (1u << (uint32_t)ESY_POWER_STATE_COUNT) - 1u;
}

static uint32_t registered_wake_source_mask(
    const esy_power_manager_t *manager) {
  uint32_t mask = 0u;
  for (uint32_t i = 0u; i < manager->wake_source_count; i++) {
    if (manager->static_resources->wake_sources[i] != NULL) mask |= 1u << i;
  }
  return mask;
}

static esy_err_t increment_reference(esy_power_atomic_u32_t *count) {
  uint32_t value = atomic_load_explicit(count, memory_order_relaxed);
  for (;;) {
    if (value == UINT32_MAX) return ESY_ERR_STATE;
    if (atomic_compare_exchange_weak_explicit(count, &value, value + 1u,
                                              memory_order_seq_cst,
                                              memory_order_relaxed)) {
      return ESY_OK;
    }
  }
}

static esy_err_t decrement_reference(esy_power_atomic_u32_t *count) {
  uint32_t value = atomic_load_explicit(count, memory_order_relaxed);
  for (;;) {
    if (value == 0u) return ESY_ERR_STATE;
    if (atomic_compare_exchange_weak_explicit(count, &value, value - 1u,
                                              memory_order_seq_cst,
                                              memory_order_relaxed)) {
      return ESY_OK;
    }
  }
}

static uint32_t lock_allowed_state_mask(const esy_power_manager_t *manager) {
  uint32_t mask = valid_state_mask();
  for (uint32_t i = 0u; i < manager->lock_count; i++) {
    const esy_power_lock_t *lock = manager->static_resources->locks[i];
    if (atomic_load_u32(&lock->usage_count) != 0u) {
      mask &= (1u << ((uint32_t)lock->deepest_allowed_state + 1u)) - 1u;
    }
  }
  return mask;
}

static esy_err_t get_backend_capabilities(
    const esy_power_manager_t *manager,
    esy_power_capabilities_t *out_capabilities) {
  esy_err_t ret = manager->backend_ops->get_capabilities(manager->backend_ctx,
                                                         out_capabilities);
  if (ret != ESY_OK) return ret;
  if (((out_capabilities->supported_state_mask &
        ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)) == 0u) ||
      ((out_capabilities->supported_state_mask & ~valid_state_mask()) != 0u)) {
    return ESY_ERR_STATE;
  }
  return ESY_OK;
}

static esy_err_t get_wake_state_mask(const esy_wake_source_t *source,
                                     uint32_t *out_state_mask) {
  uint32_t mask = 0u;
  esy_err_t ret = source->ops->get_supported_state_mask(source->ctx, &mask);
  if (ret != ESY_OK) return ret;
  if ((mask & ~valid_state_mask()) != 0u) return ESY_ERR_STATE;
  *out_state_mask = mask;
  return ESY_OK;
}

static esy_err_t get_optional_state_mask(esy_err_t (*get_mask)(void *,
                                                               uint32_t *),
                                         void *ctx, uint32_t *out_state_mask) {
  uint32_t mask = 0u;
  esy_err_t ret;
  if (get_mask == NULL) {
    *out_state_mask = 0u;
    return ESY_OK;
  }
  ret = get_mask(ctx, &mask);
  if (ret != ESY_OK) return ret;
  if ((mask & ~valid_state_mask()) != 0u) return ESY_ERR_STATE;
  *out_state_mask = mask;
  return ESY_OK;
}

static uint64_t saturated_add_u64(uint64_t left, uint64_t right) {
  return left > (UINT64_MAX - right) ? UINT64_MAX : left + right;
}

static uint64_t state_minimum_budget(
    const esy_power_state_capability_t *capability) {
  return saturated_add_u64(saturated_add_u64(capability->entry_latency_us,
                                             capability->min_residency_us),
                           capability->exit_latency_us);
}

static esy_err_t get_hard_deadline(const esy_power_manager_t *manager,
                                   uint64_t now_us, esy_power_state_t state,
                                   uint64_t requested_deadline_us,
                                   uint64_t *out_deadline_us) {
  uint64_t deadline = requested_deadline_us;
  for (uint32_t i = 0u; i < manager->constraint_count; i++) {
    uint64_t candidate = ESY_POWER_TIME_INFINITE;
    esy_power_constraint_t *constraint =
        manager->static_resources->constraints[i];
    esy_err_t ret = constraint->ops->get_hard_deadline(constraint->ctx, now_us,
                                                       state, &candidate);
    if (ret != ESY_OK) return ret;
    if ((candidate != ESY_POWER_TIME_INFINITE) && (candidate < now_us)) {
      return ESY_ERR_BUSY;
    }
    if (candidate < deadline) deadline = candidate;
  }
  *out_deadline_us = deadline;
  return ESY_OK;
}

static esy_err_t select_state_and_sources(const esy_power_manager_t *manager,
                                          const esy_power_request_t *request,
                                          uint64_t now_us,
                                          esy_power_state_t *out_state,
                                          uint64_t *out_hard_deadline_us,
                                          uint64_t *out_wake_time_us,
                                          uint32_t *out_source_mask) {
  esy_power_capabilities_t backend_capabilities;
  uint32_t device_mask;
  uint32_t deadline_mask;
  uint32_t clocksource_mask;
  uint32_t candidate_mask;
  uint32_t required_mask = request->required_wake_source_mask;
  esy_err_t ret;

  if ((required_mask & ~registered_wake_source_mask(manager)) != 0u) {
    return ESY_ERR_INVALID_ARG;
  }
  ret = esy_device_registry_get_allowed_state_mask(manager->device_registry,
                                                   &device_mask);
  if (ret != ESY_OK) return ret;
  ret = get_backend_capabilities(manager, &backend_capabilities);
  if (ret != ESY_OK) return ret;
  ret = get_optional_state_mask(
      manager->deadline_timer_ops == NULL
          ? NULL
          : manager->deadline_timer_ops->get_supported_state_mask,
      manager->deadline_timer_ctx, &deadline_mask);
  if (ret != ESY_OK) return ret;
  ret = get_optional_state_mask(
      manager->clocksource_ops == NULL
          ? NULL
          : manager->clocksource_ops->get_supported_state_mask,
      manager->clock.ctx, &clocksource_mask);
  if (ret != ESY_OK) return ret;

  candidate_mask = device_mask & backend_capabilities.supported_state_mask &
                   request->allowed_state_mask &
                   lock_allowed_state_mask(manager);
  if (manager->clock.ops != NULL) candidate_mask &= clocksource_mask;
  for (uint32_t i = 0u; i < manager->wake_source_count; i++) {
    uint32_t bit = 1u << i;
    uint32_t source_mask;
    if ((required_mask & bit) == 0u) continue;
    ret = get_wake_state_mask(manager->static_resources->wake_sources[i],
                              &source_mask);
    if (ret != ESY_OK) return ret;
    candidate_mask &= source_mask;
  }

  for (uint32_t state = (uint32_t)ESY_POWER_STATE_RETENTION;
       state >= (uint32_t)ESY_POWER_STATE_CPU_IDLE; state--) {
    const esy_power_state_capability_t *capability =
        &backend_capabilities.states[state];
    uint64_t hard_deadline_us;
    uint64_t minimum_window_us;
    uint64_t wake_advance_us;
    uint32_t state_bit = ESY_POWER_STATE_MASK(state);
    if ((candidate_mask & state_bit) == 0u) continue;
    if ((request->maximum_exit_latency_us != ESY_POWER_TIME_INFINITE) &&
        (capability->exit_latency_us > request->maximum_exit_latency_us)) {
      continue;
    }
    ret = get_hard_deadline(manager, now_us, (esy_power_state_t)state,
                            request->latest_resume_time_us, &hard_deadline_us);
    if (ret != ESY_OK) return ret;
    if (hard_deadline_us != ESY_POWER_TIME_INFINITE) {
      if (hard_deadline_us <= now_us) continue;
      minimum_window_us = saturated_add_u64(state_minimum_budget(capability),
                                            request->late_resume_guard_us);
      if (((deadline_mask & state_bit) == 0u) ||
          ((clocksource_mask & state_bit) == 0u) ||
          ((hard_deadline_us - now_us) < minimum_window_us)) {
        continue;
      }
      wake_advance_us = saturated_add_u64(capability->exit_latency_us,
                                          request->late_resume_guard_us);
      if (hard_deadline_us <= wake_advance_us) continue;
      *out_wake_time_us = hard_deadline_us - wake_advance_us;
    } else {
      *out_wake_time_us = ESY_POWER_TIME_INFINITE;
    }
    *out_state = (esy_power_state_t)state;
    *out_hard_deadline_us = hard_deadline_us;
    *out_source_mask = required_mask;
    return ESY_OK;
  }
  return ESY_ERR_NOT_SUPPORTED;
}

static esy_err_t quiesce_participants(esy_power_manager_t *manager,
                                      esy_power_state_t state) {
  for (uint32_t i = manager->participant_count; i > 0u; i--) {
    esy_power_quiesce_participant_t *participant =
        manager->static_resources->participants[i - 1u];
    esy_err_t ret = participant->ops->quiesce(participant->ctx, state);
    if (ret != ESY_OK) return ret;
    participant->quiesced = true;
  }
  return ESY_OK;
}

static esy_err_t resume_participants(esy_power_manager_t *manager,
                                     esy_power_state_t state) {
  esy_err_t result = ESY_OK;
  for (uint32_t i = 0u; i < manager->participant_count; i++) {
    esy_power_quiesce_participant_t *participant =
        manager->static_resources->participants[i];
    if (!participant->quiesced) continue;
    if (participant->ops->resume(participant->ctx, state) == ESY_OK) {
      participant->quiesced = false;
    } else {
      result = ESY_ERR_STATE;
    }
  }
  return result;
}

static esy_err_t prepare_constraints(esy_power_transaction_t *transaction) {
  esy_power_manager_t *manager = transaction->manager;
  for (uint32_t i = 0u; i < manager->constraint_count; i++) {
    esy_power_constraint_t *constraint =
        manager->static_resources->constraints[i];
    esy_err_t ret = constraint->ops->prepare(constraint->ctx,
                                             transaction->result.selected_state,
                                             transaction->result.wake_time_us);
    if (ret != ESY_OK) return ret;
    constraint->prepared = true;
  }
  transaction->constraints_prepared = true;
  return ESY_OK;
}

static esy_err_t resume_constraints(esy_power_transaction_t *transaction,
                                    uint64_t resumed_at_us) {
  esy_power_manager_t *manager = transaction->manager;
  esy_err_t result = ESY_OK;
  for (uint32_t i = manager->constraint_count; i > 0u; i--) {
    esy_power_constraint_t *constraint =
        manager->static_resources->constraints[i - 1u];
    if (!constraint->prepared) continue;
    if (constraint->ops->resume(constraint->ctx,
                                transaction->result.selected_state,
                                resumed_at_us) == ESY_OK) {
      constraint->prepared = false;
    } else {
      result = ESY_ERR_STATE;
    }
  }
  transaction->constraints_prepared = result != ESY_OK;
  return result;
}

static esy_err_t prepare_wake_sources(esy_power_transaction_t *transaction) {
  esy_power_manager_t *manager = transaction->manager;
  for (uint32_t i = 0u; i < manager->wake_source_count; i++) {
    uint32_t bit = 1u << i;
    esy_wake_source_t *source;
    esy_err_t ret;
    if ((transaction->armed_wake_source_mask & bit) == 0u) continue;
    source = manager->static_resources->wake_sources[i];
    ret = source->ops->prepare(source->ctx, transaction->result.selected_state);
    if (ret != ESY_OK) return ret;
    source->prepared = true;
  }
  transaction->wake_sources_prepared = true;
  return ESY_OK;
}

static esy_err_t abort_wake_sources(esy_power_transaction_t *transaction) {
  esy_power_manager_t *manager = transaction->manager;
  esy_err_t result = ESY_OK;
  for (uint32_t i = manager->wake_source_count; i > 0u; i--) {
    esy_wake_source_t *source = manager->static_resources->wake_sources[i - 1u];
    if ((source == NULL) || !source->prepared) continue;
    if (source->ops->abort(source->ctx) == ESY_OK) {
      source->prepared = false;
      source->captured_and_masked = false;
    } else {
      result = ESY_ERR_STATE;
    }
  }
  transaction->wake_sources_prepared = result != ESY_OK;
  return result;
}

static esy_err_t capture_wake_results_atomic(
    esy_power_transaction_t *transaction) {
  esy_power_manager_t *manager = transaction->manager;
  esy_err_t result = ESY_OK;
  for (uint32_t i = 0u; i < manager->wake_source_count; i++) {
    uint32_t bit = 1u << i;
    esy_wake_source_result_t wake_result = {0};
    esy_wake_source_t *source;
    if ((transaction->armed_wake_source_mask & bit) == 0u) continue;
    source = manager->static_resources->wake_sources[i];
    if (source->captured_and_masked) continue;
    if (source->ops->capture_and_mask_atomic(source->ctx, &wake_result) !=
        ESY_OK) {
      result = ESY_ERR_STATE;
      continue;
    }
    source->captured_and_masked = true;
    if (wake_result.triggered) {
      transaction->result.triggered_wake_source_mask |= bit;
    }
  }
  return result;
}

static esy_err_t finish_wake_sources(esy_power_transaction_t *transaction) {
  esy_power_manager_t *manager = transaction->manager;
  esy_err_t result = ESY_OK;
  for (uint32_t i = manager->wake_source_count; i > 0u; i--) {
    esy_wake_source_t *source = manager->static_resources->wake_sources[i - 1u];
    if ((source == NULL) || !source->prepared) continue;
    if (!source->captured_and_masked ||
        (source->ops->finish_and_unmask(source->ctx) != ESY_OK)) {
      result = ESY_ERR_STATE;
      continue;
    }
    source->prepared = false;
    source->captured_and_masked = false;
  }
  transaction->wake_sources_prepared = result != ESY_OK;
  return result;
}

static esy_err_t capture_deadline_atomic(esy_power_transaction_t *transaction) {
  bool expired = false;
  if (!transaction->deadline_armed) return ESY_OK;
  if (transaction->manager->deadline_timer_ops->capture_and_cancel_atomic(
          transaction->manager->deadline_timer_ctx, &expired) != ESY_OK) {
    return ESY_ERR_STATE;
  }
  transaction->result.deadline_expired = expired;
  transaction->deadline_armed = false;
  return ESY_OK;
}

static esy_err_t finish_deadline(esy_power_transaction_t *transaction) {
  if (!transaction->deadline_prepared) return ESY_OK;
  if (transaction->deadline_armed) return ESY_ERR_STATE;
  if (transaction->manager->deadline_timer_ops->finish(
          transaction->manager->deadline_timer_ctx) != ESY_OK) {
    return ESY_ERR_STATE;
  }
  transaction->deadline_prepared = false;
  return ESY_OK;
}

static esy_err_t abort_deadline(esy_power_transaction_t *transaction) {
  if (!transaction->deadline_prepared) return ESY_OK;
  if (transaction->deadline_armed) return ESY_ERR_STATE;
  if (transaction->manager->deadline_timer_ops->abort(
          transaction->manager->deadline_timer_ctx) != ESY_OK) {
    return ESY_ERR_STATE;
  }
  transaction->deadline_prepared = false;
  return ESY_OK;
}

static esy_err_t finish_abort_recovery(esy_power_transaction_t *transaction,
                                         bool captured) {
  esy_power_manager_t *manager = transaction->manager;
  uint64_t resumed_at_us = 0u;

  /* Single transaction owner: never release execution against a faulted graph. */
  if (esy_device_registry_is_faulted(manager->device_registry))
    return ESY_ERR_STATE;
  if (transaction->graph_prepared) {
    if (esy_device_registry_system_abort(manager->device_registry) != ESY_OK)
      return ESY_ERR_STATE;
    transaction->graph_prepared = false;
  }
  if (transaction->backend_begun) {
    if (manager->backend_ops->abort(manager->backend_ctx) != ESY_OK)
      return ESY_ERR_STATE;
    transaction->backend_begun = false;
  }
  if (abort_deadline(transaction) != ESY_OK) return ESY_ERR_STATE;
  if ((manager->clock.ops != NULL) &&
      (manager->clock.ops->read_atomic(manager->clock.ctx, &resumed_at_us) !=
       ESY_OK)) return ESY_ERR_STATE;
  if (resume_constraints(transaction, resumed_at_us) != ESY_OK)
    return ESY_ERR_STATE;
  if (resume_participants(manager, transaction->result.selected_state) != ESY_OK)
    return ESY_ERR_STATE;
  if ((captured ? finish_wake_sources(transaction)
                : abort_wake_sources(transaction)) != ESY_OK)
    return ESY_ERR_STATE;
  if (transaction->cores_prepared) {
    if (manager->core_coordinator_ops->abort(
            manager->core_coordinator_ctx, transaction->result.selected_state) !=
        ESY_OK) return ESY_ERR_STATE;
    transaction->cores_prepared = false;
    transaction->peers_parked = false;
  }
  return ESY_OK;
}

static esy_err_t rollback_prepare(esy_power_transaction_t *transaction) {
  return finish_abort_recovery(transaction, false);
}

void esy_power_manager_reset(esy_power_manager_t *manager) {
  if (manager != NULL) memset(manager, 0, sizeof(*manager));
}

void esy_power_transaction_init(esy_power_transaction_t *transaction) {
  if (transaction != NULL) memset(transaction, 0, sizeof(*transaction));
}

esy_err_t esy_power_manager_init(esy_power_manager_t *manager,
                                 const esy_power_manager_config_t *config) {
  esy_power_manager_static_resources_t *resources;
  if ((manager == NULL) || (config == NULL) ||
      (config->device_registry == NULL) || (config->backend_ops == NULL) ||
      (config->backend_ops->get_capabilities == NULL) ||
      (config->backend_ops->begin == NULL) ||
      (config->backend_ops->enter_atomic == NULL) ||
      (config->backend_ops->complete == NULL) ||
      (config->backend_ops->abort == NULL) ||
      (config->managed_core_count == 0u) ||
      (config->static_resources == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (((config->deadline_timer_ops == NULL) &&
       (config->deadline_timer_ctx != NULL)) ||
      ((config->clocksource_ops == NULL) != (config->clock.ops == NULL)) ||
      ((config->clock.ops != NULL) &&
       (config->clock.ops->read_atomic == NULL))) {
    return ESY_ERR_INVALID_ARG;
  }
  if ((config->deadline_timer_ops != NULL) &&
      ((config->deadline_timer_ops->get_supported_state_mask == NULL) ||
       (config->deadline_timer_ops->prepare == NULL) ||
       (config->deadline_timer_ops->arm_absolute_atomic == NULL) ||
       (config->deadline_timer_ops->capture_and_cancel_atomic == NULL) ||
       (config->deadline_timer_ops->finish == NULL) ||
       (config->deadline_timer_ops->abort == NULL))) {
    return ESY_ERR_INVALID_ARG;
  }
  if ((config->clocksource_ops != NULL) &&
      (config->clocksource_ops->get_supported_state_mask == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if ((config->managed_core_count > 1u) &&
      ((config->core_coordinator_ops == NULL) ||
       (config->core_coordinator_ops->prepare == NULL) ||
       (config->core_coordinator_ops->park_peers_atomic == NULL) ||
       (config->core_coordinator_ops->release_peers == NULL) ||
       (config->core_coordinator_ops->complete == NULL) ||
       (config->core_coordinator_ops->abort == NULL))) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  resources = config->static_resources;
  if (manager->magic != 0u) return ESY_ERR_STATE;
  if ((resources->locks == NULL) || (resources->lock_capacity == 0u) ||
      (resources->participants == NULL) ||
      (resources->participant_capacity == 0u) ||
      (resources->wake_sources == NULL) ||
      (resources->wake_source_capacity == 0u) ||
      (resources->wake_source_capacity > 32u) ||
      (resources->constraints == NULL) ||
      (resources->constraint_capacity == 0u)) {
    return ESY_ERR_INVALID_ARG;
  }
  memset(resources->locks, 0,
         resources->lock_capacity * sizeof(resources->locks[0]));
  memset(resources->participants, 0,
         resources->participant_capacity * sizeof(resources->participants[0]));
  memset(resources->wake_sources, 0,
         resources->wake_source_capacity * sizeof(resources->wake_sources[0]));
  memset(resources->constraints, 0,
         resources->constraint_capacity * sizeof(resources->constraints[0]));
  esy_power_manager_reset(manager);
  manager->device_registry = config->device_registry;
  manager->backend_ops = config->backend_ops;
  manager->backend_ctx = config->backend_ctx;
  manager->deadline_timer_ops = config->deadline_timer_ops;
  manager->deadline_timer_ctx = config->deadline_timer_ctx;
  manager->clocksource_ops = config->clocksource_ops;
  manager->clock = config->clock;
  manager->managed_core_count = config->managed_core_count;
  manager->core_coordinator_ops = config->core_coordinator_ops;
  manager->core_coordinator_ctx = config->core_coordinator_ctx;
  manager->static_resources = resources;
  atomic_init(&manager->phase, POWER_MANAGER_PHASE_CONFIGURING);
  manager->magic = ESY_POWER_MANAGER_MAGIC;
  return ESY_OK;
}

esy_err_t esy_power_manager_start(esy_power_manager_t *manager) {
  if (!manager_is_valid(manager)) return ESY_ERR_INVALID_ARG;
  if (atomic_load_u32(&manager->phase) != POWER_MANAGER_PHASE_CONFIGURING) {
    return ESY_ERR_STATE;
  }
  if (!esy_device_registry_is_ready(manager->device_registry)) {
    return ESY_ERR_STATE;
  }
  atomic_store_u32(&manager->phase, POWER_MANAGER_PHASE_READY);
  return ESY_OK;
}

esy_err_t esy_power_manager_deinit(esy_power_manager_t *manager) {
  if (!manager_is_valid(manager)) return ESY_ERR_INVALID_ARG;
  if ((atomic_load_u32(&manager->phase) != POWER_MANAGER_PHASE_CONFIGURING) &&
      (atomic_load_u32(&manager->phase) != POWER_MANAGER_PHASE_READY)) {
    return ESY_ERR_STATE;
  }
  for (uint32_t i = 0u; i < manager->lock_count; i++) {
    if (atomic_load_u32(&manager->static_resources->locks[i]->usage_count) !=
        0u) {
      return ESY_ERR_BUSY;
    }
  }
  for (uint32_t i = 0u; i < manager->wake_source_count; i++) {
    if ((manager->static_resources->wake_sources[i] != NULL) &&
        manager->static_resources->wake_sources[i]->prepared)
      return ESY_ERR_BUSY;
  }
  for (uint32_t i = 0u; i < manager->constraint_count; i++) {
    if (manager->static_resources->constraints[i]->prepared)
      return ESY_ERR_BUSY;
  }
  for (uint32_t i = 0u; i < manager->lock_count; i++)
    memset(manager->static_resources->locks[i], 0,
           sizeof(*manager->static_resources->locks[i]));
  for (uint32_t i = 0u; i < manager->participant_count; i++)
    memset(manager->static_resources->participants[i], 0,
           sizeof(*manager->static_resources->participants[i]));
  for (uint32_t i = 0u; i < manager->wake_source_count; i++) {
    if (manager->static_resources->wake_sources[i] != NULL)
      memset(manager->static_resources->wake_sources[i], 0,
             sizeof(*manager->static_resources->wake_sources[i]));
  }
  for (uint32_t i = 0u; i < manager->constraint_count; i++)
    memset(manager->static_resources->constraints[i], 0,
           sizeof(*manager->static_resources->constraints[i]));
  esy_power_manager_reset(manager);
  return ESY_OK;
}

esy_err_t esy_power_lock_register(esy_power_manager_t *manager,
                                  esy_power_lock_t *lock,
                                  const esy_power_lock_config_t *config) {
  if (!manager_is_valid(manager) || (lock == NULL) || (config == NULL) ||
      (config->name == NULL) ||
      (config->deepest_allowed_state >= ESY_POWER_STATE_COUNT)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (atomic_load_u32(&manager->phase) != POWER_MANAGER_PHASE_CONFIGURING)
    return ESY_ERR_STATE;
  if (lock->magic != 0u) return ESY_ERR_STATE;
  for (uint32_t i = 0u; i < manager->lock_count; i++) {
    if (manager->static_resources->locks[i] == lock) return ESY_ERR_STATE;
  }
  if (manager->lock_count >= manager->static_resources->lock_capacity)
    return ESY_ERR_NO_MEM;
  memset(lock, 0, sizeof(*lock));
  lock->manager = manager;
  lock->name = config->name;
  lock->deepest_allowed_state = config->deepest_allowed_state;
  atomic_init(&lock->usage_count, 0u);
  lock->manager_index = manager->lock_count;
  lock->magic = ESY_POWER_LOCK_MAGIC;
  manager->static_resources->locks[manager->lock_count++] = lock;
  return ESY_OK;
}

esy_err_t esy_power_lock_unregister(esy_power_lock_t *lock) {
  esy_power_manager_t *manager;
  uint32_t index;
  if (!lock_is_valid(lock)) return ESY_ERR_INVALID_ARG;
  manager = lock->manager;
  if (atomic_load_u32(&manager->phase) != POWER_MANAGER_PHASE_CONFIGURING)
    return ESY_ERR_STATE;
  if (atomic_load_u32(&lock->usage_count) != 0u) return ESY_ERR_BUSY;
  index = lock->manager_index;
  for (uint32_t i = index; i + 1u < manager->lock_count; i++) {
    manager->static_resources->locks[i] =
        manager->static_resources->locks[i + 1u];
    manager->static_resources->locks[i]->manager_index = i;
  }
  manager->static_resources->locks[--manager->lock_count] = NULL;
  memset(lock, 0, sizeof(*lock));
  return ESY_OK;
}

esy_err_t esy_power_lock_acquire(esy_power_lock_t *lock) {
  esy_err_t ret;
  if (!lock_is_valid(lock)) return ESY_ERR_INVALID_ARG;
  if (atomic_load_u32(&lock->manager->phase) != POWER_MANAGER_PHASE_READY)
    return ESY_ERR_BUSY;
  ret = increment_reference(&lock->usage_count);
  if (ret != ESY_OK) return ret;
  if (atomic_load_u32(&lock->manager->phase) != POWER_MANAGER_PHASE_READY) {
    (void)decrement_reference(&lock->usage_count);
    return ESY_ERR_BUSY;
  }
  return ESY_OK;
}

esy_err_t esy_power_lock_release(esy_power_lock_t *lock) {
  return lock_is_valid(lock) ? decrement_reference(&lock->usage_count)
                             : ESY_ERR_INVALID_ARG;
}

static esy_err_t power_lock_guard_acquire(void *ctx) {
  return esy_power_lock_acquire(ctx);
}

static esy_err_t power_lock_guard_release(void *ctx) {
  return esy_power_lock_release(ctx);
}

esy_err_t esy_power_lock_bind_activity_guard(esy_power_lock_t *lock,
                                             esy_activity_guard_t *guard) {
  if (!lock_is_valid(lock) || (guard == NULL)) return ESY_ERR_INVALID_ARG;
  guard->ctx = lock;
  guard->acquire = power_lock_guard_acquire;
  guard->release = power_lock_guard_release;
  return ESY_OK;
}

esy_err_t esy_power_quiesce_register(
    esy_power_manager_t *manager, esy_power_quiesce_participant_t *participant,
    const esy_power_quiesce_config_t *config) {
  if (!manager_is_valid(manager) || (participant == NULL) || (config == NULL) ||
      (config->name == NULL) || (config->ops == NULL) ||
      (config->ops->quiesce == NULL) || (config->ops->resume == NULL))
    return ESY_ERR_INVALID_ARG;
  if (atomic_load_u32(&manager->phase) != POWER_MANAGER_PHASE_CONFIGURING)
    return ESY_ERR_STATE;
  if (participant->magic != 0u) return ESY_ERR_STATE;
  for (uint32_t i = 0u; i < manager->participant_count; i++) {
    if (manager->static_resources->participants[i] == participant)
      return ESY_ERR_STATE;
  }
  if (manager->participant_count >=
      manager->static_resources->participant_capacity)
    return ESY_ERR_NO_MEM;
  memset(participant, 0, sizeof(*participant));
  participant->manager = manager;
  participant->name = config->name;
  participant->ops = config->ops;
  participant->ctx = config->ctx;
  participant->manager_index = manager->participant_count;
  participant->magic = ESY_POWER_QUIESCE_MAGIC;
  manager->static_resources->participants[manager->participant_count++] =
      participant;
  return ESY_OK;
}

esy_err_t esy_power_quiesce_unregister(
    esy_power_quiesce_participant_t *participant) {
  esy_power_manager_t *manager;
  uint32_t index;
  if (!participant_is_valid(participant)) return ESY_ERR_INVALID_ARG;
  manager = participant->manager;
  if (atomic_load_u32(&manager->phase) != POWER_MANAGER_PHASE_CONFIGURING)
    return ESY_ERR_STATE;
  if (participant->quiesced) return ESY_ERR_BUSY;
  index = participant->manager_index;
  for (uint32_t i = index; i + 1u < manager->participant_count; i++) {
    manager->static_resources->participants[i] =
        manager->static_resources->participants[i + 1u];
    manager->static_resources->participants[i]->manager_index = i;
  }
  manager->static_resources->participants[--manager->participant_count] = NULL;
  memset(participant, 0, sizeof(*participant));
  return ESY_OK;
}

esy_err_t esy_wake_source_register(esy_power_manager_t *manager,
                                   esy_wake_source_t *source,
                                   const esy_wake_source_config_t *config) {
  uint32_t index;
  if (!manager_is_valid(manager) || (source == NULL) || (config == NULL) ||
      (config->name == NULL) || (config->ops == NULL) ||
      (config->ops->get_supported_state_mask == NULL) ||
      (config->ops->prepare == NULL) ||
      (config->ops->capture_and_mask_atomic == NULL) ||
      (config->ops->finish_and_unmask == NULL) || (config->ops->abort == NULL))
    return ESY_ERR_INVALID_ARG;
  if (atomic_load_u32(&manager->phase) != POWER_MANAGER_PHASE_CONFIGURING)
    return ESY_ERR_STATE;
  if (source->magic != 0u) return ESY_ERR_STATE;
  for (index = 0u; index < manager->wake_source_count; index++) {
    if (manager->static_resources->wake_sources[index] == NULL) break;
  }
  if (index >= manager->static_resources->wake_source_capacity)
    return ESY_ERR_NO_MEM;
  memset(source, 0, sizeof(*source));
  source->manager = manager;
  source->name = config->name;
  source->ops = config->ops;
  source->ctx = config->ctx;
  source->manager_index = index;
  source->magic = ESY_WAKE_SOURCE_MAGIC;
  manager->static_resources->wake_sources[index] = source;
  if (index == manager->wake_source_count) manager->wake_source_count++;
  return ESY_OK;
}

esy_err_t esy_wake_source_unregister(esy_wake_source_t *source) {
  esy_power_manager_t *manager;
  uint32_t index;
  if (!wake_source_is_valid(source)) return ESY_ERR_INVALID_ARG;
  manager = source->manager;
  if (atomic_load_u32(&manager->phase) != POWER_MANAGER_PHASE_CONFIGURING)
    return ESY_ERR_STATE;
  if (source->prepared) return ESY_ERR_BUSY;
  index = source->manager_index;
  manager->static_resources->wake_sources[index] = NULL;
  memset(source, 0, sizeof(*source));
  return ESY_OK;
}

esy_err_t esy_power_constraint_register(
    esy_power_manager_t *manager, esy_power_constraint_t *constraint,
    const esy_power_constraint_config_t *config) {
  if (!manager_is_valid(manager) || (constraint == NULL) || (config == NULL) ||
      (config->name == NULL) || (config->ops == NULL) ||
      (config->ops->get_hard_deadline == NULL) ||
      (config->ops->prepare == NULL) ||
      (config->ops->revalidate_atomic == NULL) || (config->ops->resume == NULL))
    return ESY_ERR_INVALID_ARG;
  if (atomic_load_u32(&manager->phase) != POWER_MANAGER_PHASE_CONFIGURING)
    return ESY_ERR_STATE;
  if (constraint->magic != 0u) return ESY_ERR_STATE;
  for (uint32_t i = 0u; i < manager->constraint_count; i++) {
    if (manager->static_resources->constraints[i] == constraint)
      return ESY_ERR_STATE;
  }
  if (manager->constraint_count >=
      manager->static_resources->constraint_capacity)
    return ESY_ERR_NO_MEM;
  memset(constraint, 0, sizeof(*constraint));
  constraint->manager = manager;
  constraint->name = config->name;
  constraint->ops = config->ops;
  constraint->ctx = config->ctx;
  constraint->manager_index = manager->constraint_count;
  constraint->magic = ESY_POWER_CONSTRAINT_MAGIC;
  manager->static_resources->constraints[manager->constraint_count++] =
      constraint;
  return ESY_OK;
}

esy_err_t esy_power_constraint_unregister(esy_power_constraint_t *constraint) {
  esy_power_manager_t *manager;
  uint32_t index;
  if (!constraint_is_valid(constraint)) return ESY_ERR_INVALID_ARG;
  manager = constraint->manager;
  if (atomic_load_u32(&manager->phase) != POWER_MANAGER_PHASE_CONFIGURING)
    return ESY_ERR_STATE;
  if (constraint->prepared) return ESY_ERR_BUSY;
  index = constraint->manager_index;
  for (uint32_t i = index; i + 1u < manager->constraint_count; i++) {
    manager->static_resources->constraints[i] =
        manager->static_resources->constraints[i + 1u];
    manager->static_resources->constraints[i]->manager_index = i;
  }
  manager->static_resources->constraints[--manager->constraint_count] = NULL;
  memset(constraint, 0, sizeof(*constraint));
  return ESY_OK;
}

void esy_power_request_default(esy_power_request_t *request) {
  if (request == NULL) return;
  memset(request, 0, sizeof(*request));
  request->latest_resume_time_us = ESY_POWER_TIME_INFINITE;
  request->maximum_exit_latency_us = ESY_POWER_TIME_INFINITE;
  request->allowed_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                                ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE) |
                                ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION);
}

esy_err_t esy_power_manager_get_deepest_allowed_state(
    const esy_power_manager_t *manager, esy_power_state_t *out_state) {
  esy_power_request_t request;
  uint64_t hard_deadline_us;
  uint64_t wake_time_us;
  uint64_t now_us = 0u;
  uint32_t source_mask;
  if (!manager_is_valid(manager) || (out_state == NULL))
    return ESY_ERR_INVALID_ARG;
  if (atomic_load_u32(&manager->phase) != POWER_MANAGER_PHASE_READY)
    return ESY_ERR_STATE;
  esy_power_request_default(&request);
  if ((manager->clock.ops != NULL) &&
      (manager->clock.ops->read_atomic(manager->clock.ctx, &now_us) !=
       ESY_OK)) {
    return ESY_ERR_STATE;
  }
  return select_state_and_sources(manager, &request, now_us, out_state,
                                  &hard_deadline_us, &wake_time_us,
                                  &source_mask);
}

esy_err_t esy_power_manager_runtime_suspend_idle(esy_power_manager_t *manager) {
  if (!manager_is_valid(manager)) return ESY_ERR_INVALID_ARG;
  if (atomic_load_u32(&manager->phase) != POWER_MANAGER_PHASE_READY)
    return ESY_ERR_STATE;
  return esy_device_registry_runtime_suspend_idle(manager->device_registry);
}

esy_err_t esy_power_manager_now_atomic(const esy_power_manager_t *manager,
                                       uint64_t *out_time_us) {
  if (!manager_is_valid(manager) || (out_time_us == NULL) ||
      (manager->clock.ops == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  return manager->clock.ops->read_atomic(manager->clock.ctx, out_time_us);
}

esy_err_t esy_power_transaction_prepare(esy_power_manager_t *manager,
                                        esy_power_transaction_t *transaction,
                                        const esy_power_request_t *request) {
  uint32_t expected = POWER_MANAGER_PHASE_READY;
  esy_power_state_t state;
  uint64_t hard_deadline_us;
  uint64_t wake_time_us;
  uint64_t now_us = 0u;
  uint32_t source_mask;
  esy_err_t ret;
  if (!manager_is_valid(manager) || (transaction == NULL) ||
      (request == NULL) ||
      ((request->latest_resume_time_us == ESY_POWER_TIME_INFINITE) &&
       (request->required_wake_source_mask == 0u)) ||
      ((request->allowed_state_mask & ~valid_state_mask()) != 0u) ||
      ((transaction->phase != ESY_POWER_TRANSACTION_PHASE_EMPTY) &&
       (transaction->phase != ESY_POWER_TRANSACTION_PHASE_DONE)))
    return ESY_ERR_INVALID_ARG;
  if (!atomic_cas_u32(&manager->phase, &expected,
                      POWER_MANAGER_PHASE_TRANSITIONING))
    return ESY_ERR_BUSY;
  if (manager->clock.ops != NULL) {
    ret = esy_power_manager_now_atomic(manager, &now_us);
    if (ret != ESY_OK) goto fail_ready;
  } else if (request->latest_resume_time_us != ESY_POWER_TIME_INFINITE) {
    ret = ESY_ERR_NOT_SUPPORTED;
    goto fail_ready;
  }
  ret =
      select_state_and_sources(manager, request, now_us, &state,
                               &hard_deadline_us, &wake_time_us, &source_mask);
  if (ret != ESY_OK) goto fail_ready;

  esy_power_transaction_init(transaction);
  transaction->manager = manager;
  transaction->request = *request;
  transaction->result.selected_state = state;
  transaction->result.hard_deadline_us = hard_deadline_us;
  transaction->result.wake_time_us = wake_time_us;
  transaction->result.primary_error = ESY_OK;
  transaction->result.recovery_error = ESY_OK;
  transaction->result.fatal_error = ESY_OK;
  transaction->armed_wake_source_mask = source_mask;

  if (state > ESY_POWER_STATE_CPU_IDLE) {
    ret = quiesce_participants(manager, state);
    if (ret != ESY_OK) goto fail_transaction;
  }
  ret = prepare_constraints(transaction);
  if (ret != ESY_OK) goto fail_transaction;
  ret = prepare_wake_sources(transaction);
  if (ret != ESY_OK) goto fail_transaction;
  if (hard_deadline_us != ESY_POWER_TIME_INFINITE) {
    ret = manager->deadline_timer_ops->prepare(manager->deadline_timer_ctx,
                                               state);
    if (ret != ESY_OK) goto fail_transaction;
    transaction->deadline_prepared = true;
  }
  if (manager->managed_core_count > 1u) {
    ret = manager->core_coordinator_ops->prepare(manager->core_coordinator_ctx,
                                                 state);
    if (ret != ESY_OK) goto fail_transaction;
    transaction->cores_prepared = true;
  }
  ret = manager->backend_ops->begin(manager->backend_ctx, state);
  if (ret != ESY_OK) goto fail_transaction;
  transaction->backend_begun = true;
  ret = esy_device_registry_system_prepare(manager->device_registry, state);
  if (ret != ESY_OK) goto fail_transaction;
  transaction->graph_prepared = true;
  transaction->phase = ESY_POWER_TRANSACTION_PHASE_PREPARED;
  return ESY_OK;

fail_transaction:
  transaction->result.primary_error = ret;
  if (rollback_prepare(transaction) != ESY_OK)
    transaction->result.recovery_error = ESY_ERR_STATE;
  transaction->phase = ESY_POWER_TRANSACTION_PHASE_DONE;
  atomic_store_u32(&manager->phase, transaction->result.recovery_error == ESY_OK
                                        ? POWER_MANAGER_PHASE_READY
                                        : POWER_MANAGER_PHASE_FAULTED);
  return ret;

fail_ready:
  atomic_store_u32(&manager->phase, POWER_MANAGER_PHASE_READY);
  return ret;
}

static esy_err_t revalidate_deadline_atomic(
    esy_power_transaction_t *transaction, uint64_t now_us,
    const esy_power_state_capability_t *capability,
    uint64_t *out_wake_time_us) {
  esy_power_manager_t *manager = transaction->manager;
  uint64_t hard_deadline_us = transaction->request.latest_resume_time_us;
  uint64_t wake_advance_us;
  uint64_t minimum_window_us;

  for (uint32_t i = 0u; i < manager->constraint_count; i++) {
    uint64_t candidate = ESY_POWER_TIME_INFINITE;
    esy_power_constraint_t *constraint =
        manager->static_resources->constraints[i];
    esy_err_t ret = constraint->ops->revalidate_atomic(
        constraint->ctx, now_us, transaction->result.selected_state,
        &candidate);
    if (ret != ESY_OK) return ret;
    if ((candidate != ESY_POWER_TIME_INFINITE) && (candidate <= now_us)) {
      return ESY_ERR_BUSY;
    }
    if (candidate < hard_deadline_us) hard_deadline_us = candidate;
  }

  if (hard_deadline_us == ESY_POWER_TIME_INFINITE) {
    *out_wake_time_us = ESY_POWER_TIME_INFINITE;
    transaction->result.hard_deadline_us = hard_deadline_us;
    transaction->result.wake_time_us = hard_deadline_us;
    return ESY_OK;
  }
  minimum_window_us =
      saturated_add_u64(state_minimum_budget(capability),
                        transaction->request.late_resume_guard_us);
  if ((hard_deadline_us <= now_us) ||
      ((hard_deadline_us - now_us) < minimum_window_us)) {
    return ESY_ERR_BUSY;
  }
  wake_advance_us = saturated_add_u64(
      capability->exit_latency_us, transaction->request.late_resume_guard_us);
  if (hard_deadline_us <= wake_advance_us) return ESY_ERR_BUSY;
  *out_wake_time_us = hard_deadline_us - wake_advance_us;
  if (*out_wake_time_us <= now_us) return ESY_ERR_BUSY;
  transaction->result.hard_deadline_us = hard_deadline_us;
  transaction->result.wake_time_us = *out_wake_time_us;
  return ESY_OK;
}

esy_err_t esy_power_transaction_enter_atomic(
    esy_power_transaction_t *transaction) {
  esy_power_manager_t *manager;
  esy_power_capabilities_t capabilities;
  esy_power_backend_result_t backend_result = {0};
  uint64_t started_us = 0u;
  uint64_t finished_us = 0u;
  uint64_t wake_time_us = ESY_POWER_TIME_INFINITE;
  bool clock_started = false;
  esy_err_t ret;
  if ((transaction == NULL) || !manager_is_valid(transaction->manager))
    return ESY_ERR_INVALID_ARG;
  if (transaction->phase != ESY_POWER_TRANSACTION_PHASE_PREPARED)
    return ESY_ERR_STATE;
  manager = transaction->manager;
  ret = get_backend_capabilities(manager, &capabilities);
  if (ret != ESY_OK) goto fail_before_commit;
  if ((lock_allowed_state_mask(manager) &
       ESY_POWER_STATE_MASK(transaction->result.selected_state)) == 0u) {
    ret = ESY_ERR_BUSY;
    goto fail_before_commit;
  }

  if (transaction->cores_prepared) {
    ret = manager->core_coordinator_ops->park_peers_atomic(
        manager->core_coordinator_ctx, transaction->result.selected_state);
    if (ret != ESY_OK) goto fail_before_commit;
    transaction->peers_parked = true;
  }
  if (manager->clock.ops != NULL) {
    ret = manager->clock.ops->read_atomic(manager->clock.ctx, &started_us);
    if (ret != ESY_OK) goto fail_before_commit;
    clock_started = true;
  }
  ret = revalidate_deadline_atomic(
      transaction, started_us,
      &capabilities.states[transaction->result.selected_state], &wake_time_us);
  if (ret != ESY_OK) goto fail_before_commit;
  if ((wake_time_us != ESY_POWER_TIME_INFINITE) &&
      !transaction->deadline_prepared) {
    ret = ESY_ERR_BUSY;
    goto fail_before_commit;
  }
  if (transaction->deadline_prepared &&
      (wake_time_us != ESY_POWER_TIME_INFINITE)) {
    ret = manager->deadline_timer_ops->arm_absolute_atomic(
        manager->deadline_timer_ctx, transaction->result.selected_state,
        wake_time_us);
    if (ret != ESY_OK) goto fail_before_commit;
    transaction->deadline_armed = true;
  }
  ret = esy_device_registry_system_commit_atomic(manager->device_registry);
  if (ret != ESY_OK) goto fail_before_commit;
  transaction->graph_prepared = false;
  transaction->device_graph_committed = true;

  transaction->backend_enter_attempted = true;
  ret =
      manager->backend_ops->enter_atomic(manager->backend_ctx, &backend_result);
  transaction->result.entered = backend_result.entered;
  transaction->result.primary_error = ret;
  if (backend_result.fatal_error != ESY_OK) {
    transaction->result.fatal = true;
    transaction->result.fatal_error = backend_result.fatal_error;
    transaction->phase = ESY_POWER_TRANSACTION_PHASE_ENTERED;
    return backend_result.fatal_error;
  }
  if (capture_wake_results_atomic(transaction) != ESY_OK) {
    transaction->result.fatal = true;
    transaction->result.fatal_error = ESY_ERR_STATE;
    transaction->phase = ESY_POWER_TRANSACTION_PHASE_ENTERED;
    return ESY_ERR_STATE;
  }
  if (capture_deadline_atomic(transaction) != ESY_OK) {
    transaction->result.fatal = true;
    transaction->result.fatal_error = ESY_ERR_STATE;
    transaction->phase = ESY_POWER_TRANSACTION_PHASE_ENTERED;
    return ESY_ERR_STATE;
  }
  if (clock_started) {
    if ((manager->clock.ops->read_atomic(manager->clock.ctx, &finished_us) ==
         ESY_OK) &&
        (finished_us >= started_us)) {
      transaction->result.elapsed_valid = true;
      transaction->result.elapsed_us = finished_us - started_us;
    } else {
      transaction->result.fatal = true;
      transaction->result.fatal_error = ESY_ERR_STATE;
      transaction->phase = ESY_POWER_TRANSACTION_PHASE_ENTERED;
      return ESY_ERR_STATE;
    }
  }
  transaction->phase = ESY_POWER_TRANSACTION_PHASE_ENTERED;
  return ret;

fail_before_commit:
  if (capture_deadline_atomic(transaction) != ESY_OK) {
    transaction->result.fatal = true;
    transaction->result.fatal_error = ESY_ERR_STATE;
    transaction->phase = ESY_POWER_TRANSACTION_PHASE_ENTERED;
    return ESY_ERR_STATE;
  }
  /* Successful peer parking remains owned until late recovery completes. */
  if (clock_started &&
      (manager->clock.ops->read_atomic(manager->clock.ctx, &finished_us) ==
       ESY_OK) &&
      (finished_us >= started_us)) {
    transaction->result.elapsed_valid = true;
    transaction->result.elapsed_us = finished_us - started_us;
  }
  transaction->result.primary_error = ret;
  return ret;
}

esy_err_t esy_power_transaction_recover_atomic(
    esy_power_transaction_t *transaction) {
  esy_power_manager_t *manager;
  esy_err_t recovery = ESY_OK;

  if ((transaction == NULL) || !manager_is_valid(transaction->manager)) {
    return ESY_ERR_INVALID_ARG;
  }
  if ((transaction->phase != ESY_POWER_TRANSACTION_PHASE_PREPARED) &&
      (transaction->phase != ESY_POWER_TRANSACTION_PHASE_ENTERED)) {
    return ESY_ERR_STATE;
  }
  if (transaction->result.fatal) return ESY_ERR_STATE;
  manager = transaction->manager;

  if (capture_wake_results_atomic(transaction) != ESY_OK) {
    recovery = ESY_ERR_STATE;
  }
  if (transaction->backend_begun) {
    esy_err_t backend_ret =
        transaction->backend_enter_attempted
            ? manager->backend_ops->complete(manager->backend_ctx)
            : manager->backend_ops->abort(manager->backend_ctx);
    if (backend_ret != ESY_OK) {
      recovery = ESY_ERR_STATE;
    } else {
      transaction->backend_begun = false;
    }
  }
  if ((recovery == ESY_OK) && transaction->device_graph_committed) {
    if (esy_device_registry_system_resume_atomic(manager->device_registry) !=
        ESY_OK) {
      recovery = ESY_ERR_STATE;
    }
  } else if ((recovery == ESY_OK) && transaction->graph_prepared) {
    if (esy_device_registry_system_abort_atomic(manager->device_registry) !=
        ESY_OK) {
      recovery = ESY_ERR_STATE;
    }
  }
  if (recovery != ESY_OK) {
    transaction->result.fatal = true;
    transaction->result.fatal_error = ESY_ERR_STATE;
    atomic_store_u32(&manager->phase, POWER_MANAGER_PHASE_FAULTED);
    return ESY_ERR_STATE;
  }
  transaction->phase = ESY_POWER_TRANSACTION_PHASE_RECOVERED;
  return ESY_OK;
}

esy_err_t esy_power_transaction_complete(esy_power_transaction_t *transaction,
                                         esy_power_result_t *out_result) {
  esy_power_manager_t *manager;
  esy_err_t recovery;
  uint64_t resumed_at_us = 0u;
  if ((transaction == NULL) || (out_result == NULL) ||
      !manager_is_valid(transaction->manager))
    return ESY_ERR_INVALID_ARG;
  if ((transaction->phase != ESY_POWER_TRANSACTION_PHASE_RECOVERED) ||
      !transaction->device_graph_committed)
    return ESY_ERR_STATE;
  if (transaction->result.fatal) return ESY_ERR_STATE;
  manager = transaction->manager;
  recovery = ESY_ERR_STATE;
  if (finish_deadline(transaction) != ESY_OK) goto finish;
  if (esy_device_registry_system_resume(manager->device_registry) != ESY_OK)
    goto finish;
  transaction->device_graph_committed = false;
  if ((manager->clock.ops != NULL) &&
      (manager->clock.ops->read_atomic(manager->clock.ctx, &resumed_at_us) !=
       ESY_OK)) goto finish;
  if (resume_constraints(transaction, resumed_at_us) != ESY_OK) goto finish;
  if (resume_participants(manager, transaction->result.selected_state) != ESY_OK)
    goto finish;
  if (finish_wake_sources(transaction) != ESY_OK) goto finish;
  if (transaction->peers_parked) {
    if (manager->core_coordinator_ops->release_peers(
            manager->core_coordinator_ctx, transaction->result.selected_state) !=
        ESY_OK) goto finish;
    transaction->peers_parked = false;
  }
  if (transaction->cores_prepared) {
    if (manager->core_coordinator_ops->complete(
            manager->core_coordinator_ctx, transaction->result.selected_state) !=
        ESY_OK) goto finish;
    transaction->cores_prepared = false;
  }
  recovery = ESY_OK;
finish:
  transaction->result.recovery_error = recovery;
  transaction->phase = ESY_POWER_TRANSACTION_PHASE_DONE;
  atomic_store_u32(&manager->phase, recovery == ESY_OK
                                        ? POWER_MANAGER_PHASE_READY
                                        : POWER_MANAGER_PHASE_FAULTED);
  *out_result = transaction->result;
  return recovery;
}

esy_err_t esy_power_transaction_abort(esy_power_transaction_t *transaction,
                                      esy_power_result_t *out_result) {
  esy_err_t recovery = ESY_OK;
  if ((transaction == NULL) || (out_result == NULL) ||
      !manager_is_valid(transaction->manager))
    return ESY_ERR_INVALID_ARG;
  if (transaction->phase == ESY_POWER_TRANSACTION_PHASE_PREPARED) {
    if (rollback_prepare(transaction) != ESY_OK) recovery = ESY_ERR_STATE;
  } else if ((transaction->phase == ESY_POWER_TRANSACTION_PHASE_RECOVERED) &&
             !transaction->device_graph_committed) {
    if (finish_abort_recovery(transaction, true) != ESY_OK) recovery = ESY_ERR_STATE;
  } else {
    return ESY_ERR_STATE;
  }
  transaction->result.recovery_error = recovery;
  transaction->phase = ESY_POWER_TRANSACTION_PHASE_DONE;
  atomic_store_u32(&transaction->manager->phase,
                   recovery == ESY_OK ? POWER_MANAGER_PHASE_READY
                                      : POWER_MANAGER_PHASE_FAULTED);
  *out_result = transaction->result;
  return recovery;
}
