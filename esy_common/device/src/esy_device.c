/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/defs/coordination/esy_atomic_defs.h"
#include "esy_common/device/esy_device.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define ESY_DEVICE_MAGIC          0x45534456u
#define ESY_DEVICE_REGISTRY_MAGIC 0x45534452u
#define ESY_DEVICE_WORK_DONE      UINT32_MAX

typedef enum {
  DEVICE_REGISTRY_PHASE_UNINITIALIZED = 0,
  DEVICE_REGISTRY_PHASE_CONFIGURING,
  DEVICE_REGISTRY_PHASE_READY,
  DEVICE_REGISTRY_PHASE_TRANSITIONING,
  DEVICE_REGISTRY_PHASE_PREPARED,
  DEVICE_REGISTRY_PHASE_SUSPENDED,
  DEVICE_REGISTRY_PHASE_ABORTING,
  DEVICE_REGISTRY_PHASE_RESUMING,
  DEVICE_REGISTRY_PHASE_FAULTED
} device_registry_phase_t;

static uint32_t atomic_load_u32(const esy_device_atomic_u32_t *value) {
  return atomic_load_explicit(value, memory_order_acquire);
}

static void atomic_store_u32(esy_device_atomic_u32_t *value, uint32_t next) {
  atomic_store_explicit(value, next, memory_order_release);
}

static bool atomic_cas_u32(esy_device_atomic_u32_t *value, uint32_t *expected,
                           uint32_t desired) {
  return atomic_compare_exchange_strong_explicit(
      value, expected, desired, memory_order_acq_rel, memory_order_acquire);
}

static bool registry_is_valid(const esy_device_registry_t *registry) {
  return (registry != NULL) && (registry->magic == ESY_DEVICE_REGISTRY_MAGIC) &&
         (registry->static_resources != NULL);
}

static bool device_is_valid(const esy_device_t *device) {
  return (device != NULL) && (device->magic == ESY_DEVICE_MAGIC) &&
         registry_is_valid(device->registry) &&
         (device->registry_index < device->registry->device_count) &&
         (device->registry->static_resources->devices[device->registry_index] ==
          device);
}

static uint32_t valid_state_mask(void) {
  return ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
         ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE) |
         ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION);
}

static bool device_has_dependency(const esy_device_t *device,
                                  const esy_device_t *dependency) {
  for (uint32_t i = 0u; i < device->dependency_count; i++) {
    if (device->dependencies[i] == dependency) {
      return true;
    }
  }
  return false;
}

static esy_err_t increment_reference(esy_device_atomic_u32_t *reference_count) {
  uint32_t count = atomic_load_explicit(reference_count, memory_order_relaxed);

  for (;;) {
    if (count == UINT32_MAX) {
      return ESY_ERR_STATE;
    }
    if (atomic_compare_exchange_weak_explicit(reference_count, &count,
                                              count + 1u, memory_order_acq_rel,
                                              memory_order_relaxed)) {
      return ESY_OK;
    }
  }
}

static esy_err_t decrement_reference(esy_device_atomic_u32_t *reference_count) {
  uint32_t count = atomic_load_explicit(reference_count, memory_order_relaxed);

  for (;;) {
    if (count == 0u) {
      return ESY_ERR_STATE;
    }
    if (atomic_compare_exchange_weak_explicit(reference_count, &count,
                                              count - 1u, memory_order_acq_rel,
                                              memory_order_relaxed)) {
      return ESY_OK;
    }
  }
}

static esy_err_t device_put_internal(esy_device_t *device);

static esy_err_t resume_runtime_device(esy_device_t *device) {
  uint32_t expected = ESY_DEVICE_STATE_RUNTIME_SUSPENDED;
  esy_err_t ret;

  if (!atomic_cas_u32(&device->state, &expected,
                      ESY_DEVICE_STATE_TRANSITIONING)) {
    return (expected == ESY_DEVICE_STATE_ACTIVE) ? ESY_OK : ESY_ERR_BUSY;
  }

  ret = device->ops->pm_action(device, ESY_DEVICE_PM_ACTION_RUNTIME_RESUME,
                               ESY_POWER_STATE_ACTIVE);
  if (ret != ESY_OK) {
    atomic_store_u32(&device->state, ESY_DEVICE_STATE_FAULTED);
    atomic_store_u32(&device->registry->phase, DEVICE_REGISTRY_PHASE_FAULTED);
    return ret;
  }

  atomic_store_u32(&device->state, ESY_DEVICE_STATE_ACTIVE);
  return ESY_OK;
}

static esy_err_t device_get_internal(esy_device_t *device) {
  uint32_t acquired_dependency_count = 0u;
  esy_err_t ret;

  if (atomic_load_u32(&device->registry->phase) !=
      DEVICE_REGISTRY_PHASE_READY) {
    return ESY_ERR_BUSY;
  }

  for (uint32_t i = 0u; i < device->dependency_count; i++) {
    ret = device_get_internal(device->dependencies[i]);
    if (ret != ESY_OK) {
      goto rollback_dependencies;
    }
    acquired_dependency_count++;
  }

  /* Concurrent get/PM owners must see this reservation before resume starts.
   * Otherwise autosuspend can mistake an in-flight resume for a bad state. */
  ret = increment_reference(&device->usage_count);
  if (ret != ESY_OK) {
    goto rollback_dependencies;
  }

  if (atomic_load_u32(&device->registry->phase) !=
      DEVICE_REGISTRY_PHASE_READY) {
    ret = ESY_ERR_BUSY;
    goto rollback_reference;
  }

  if (atomic_load_u32(&device->state) == ESY_DEVICE_STATE_RUNTIME_SUSPENDED) {
    ret = resume_runtime_device(device);
    if (ret != ESY_OK) {
      goto rollback_reference;
    }
  }

  if ((atomic_load_u32(&device->registry->phase) !=
       DEVICE_REGISTRY_PHASE_READY) ||
      (atomic_load_u32(&device->state) != ESY_DEVICE_STATE_ACTIVE)) {
    ret = ESY_ERR_BUSY;
    goto rollback_reference;
  }

  return ESY_OK;

rollback_reference:
  (void)decrement_reference(&device->usage_count);
rollback_dependencies:
  while (acquired_dependency_count > 0u) {
    acquired_dependency_count--;
    (void)device_put_internal(device->dependencies[acquired_dependency_count]);
  }
  return ret;
}

static esy_err_t device_put_internal(esy_device_t *device) {
  esy_err_t ret = decrement_reference(&device->usage_count);

  if (ret != ESY_OK) {
    return ret;
  }

  for (uint32_t i = device->dependency_count; i > 0u; i--) {
    ret = device_put_internal(device->dependencies[i - 1u]);
    if (ret != ESY_OK) {
      return ret;
    }
  }
  return ESY_OK;
}

static bool all_references_released(const esy_device_registry_t *registry) {
  for (uint32_t i = 0u; i < registry->device_count; i++) {
    if (atomic_load_u32(&registry->static_resources->devices[i]->usage_count) !=
        0u) {
      return false;
    }
  }
  return true;
}

static esy_err_t validate_system_transition(
    const esy_device_registry_t *registry, esy_power_state_t state) {
  for (uint32_t i = 0u; i < registry->device_count; i++) {
    const esy_device_t *device = registry->static_resources->devices[i];

    if ((state > ESY_POWER_STATE_CPU_IDLE) &&
        (atomic_load_u32(&device->usage_count) != 0u)) {
      return ESY_ERR_BUSY;
    }
    if ((device->supported_state_mask & ESY_POWER_STATE_MASK(state)) == 0u) {
      return ESY_ERR_NOT_SUPPORTED;
    }
  }
  return ESY_OK;
}

static esy_err_t rollback_system_suspend(esy_device_registry_t *registry) {
  for (uint32_t i = 0u; i < registry->topology_count; i++) {
    esy_device_t *device =
        registry->static_resources
            ->devices[registry->static_resources->topology[i]];

    uint32_t previous_state =
        registry->static_resources
            ->work[registry->static_resources->topology[i]];

    if (atomic_load_u32(&device->state) != ESY_DEVICE_STATE_SYSTEM_SUSPENDED) {
      continue;
    }

    if (previous_state == ESY_DEVICE_STATE_RUNTIME_SUSPENDED) {
      atomic_store_u32(&device->state, ESY_DEVICE_STATE_RUNTIME_SUSPENDED);
      continue;
    }

    /* Rollback uses the same published callback state as ordinary resume. */
    atomic_store_u32(&device->state, ESY_DEVICE_STATE_TRANSITIONING);
    esy_err_t ret = device->ops->pm_action(
        device, ESY_DEVICE_PM_ACTION_SYSTEM_RESUME, registry->suspended_state);
    if (ret != ESY_OK) {
      atomic_store_u32(&device->state, ESY_DEVICE_STATE_FAULTED);
      return ret;
    }
    atomic_store_u32(&device->state, ESY_DEVICE_STATE_ACTIVE);
  }
  return ESY_OK;
}

static esy_err_t recover_system_atomic(esy_device_registry_t *registry) {
  for (uint32_t i = 0u; i < registry->topology_count; i++) {
    uint32_t device_index = registry->static_resources->topology[i];
    esy_device_t *device = registry->static_resources->devices[device_index];

    if (atomic_load_u32(&device->state) != ESY_DEVICE_STATE_SYSTEM_SUSPENDED) {
      continue;
    }
    if (registry->static_resources->work[device_index] ==
        ESY_DEVICE_STATE_RUNTIME_SUSPENDED) {
      atomic_store_u32(&device->state, ESY_DEVICE_STATE_RUNTIME_SUSPENDED);
      continue;
    }
    atomic_store_u32(&device->state, ESY_DEVICE_STATE_TRANSITIONING);
    if ((device->ops->system_resume_atomic != NULL) &&
        (device->ops->system_resume_atomic(device, registry->suspended_state) !=
         ESY_OK)) {
      atomic_store_u32(&device->state, ESY_DEVICE_STATE_FAULTED);
      return ESY_ERR_STATE;
    }
  }
  return ESY_OK;
}

static esy_err_t finish_system_recovery(esy_device_registry_t *registry) {
  for (uint32_t i = 0u; i < registry->topology_count; i++) {
    uint32_t device_index = registry->static_resources->topology[i];
    esy_device_t *device = registry->static_resources->devices[device_index];

    if (registry->static_resources->work[device_index] ==
        ESY_DEVICE_STATE_RUNTIME_SUSPENDED) {
      continue;
    }
    if (atomic_load_u32(&device->state) != ESY_DEVICE_STATE_TRANSITIONING) {
      atomic_store_u32(&device->state, ESY_DEVICE_STATE_FAULTED);
      return ESY_ERR_STATE;
    }
    esy_err_t ret = device->ops->pm_action(
        device, ESY_DEVICE_PM_ACTION_SYSTEM_RESUME, registry->suspended_state);
    if (ret != ESY_OK) {
      atomic_store_u32(&device->state, ESY_DEVICE_STATE_FAULTED);
      return ret;
    }
    atomic_store_u32(&device->state, ESY_DEVICE_STATE_ACTIVE);
  }
  return ESY_OK;
}

esy_err_t esy_device_registry_init(esy_device_registry_t *registry,
                                   const esy_device_registry_config_t *config) {
  esy_device_registry_static_resources_t *resources;

  if ((registry == NULL) || (config == NULL) ||
      (config->static_resources == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (registry->magic == ESY_DEVICE_REGISTRY_MAGIC) {
    return ESY_ERR_STATE;
  }

  resources = config->static_resources;
  if ((resources->devices == NULL) || (resources->topology == NULL) ||
      (resources->work == NULL) || (resources->capacity == 0u)) {
    return ESY_ERR_INVALID_ARG;
  }

  memset(resources->devices, 0,
         resources->capacity * sizeof(resources->devices[0]));
  memset(resources->topology, 0,
         resources->capacity * sizeof(resources->topology[0]));
  memset(resources->work, 0, resources->capacity * sizeof(resources->work[0]));
  memset(registry, 0, sizeof(*registry));
  registry->static_resources = resources;
  registry->suspended_state = ESY_POWER_STATE_ACTIVE;
  atomic_init(&registry->phase, DEVICE_REGISTRY_PHASE_CONFIGURING);
  registry->magic = ESY_DEVICE_REGISTRY_MAGIC;
  return ESY_OK;
}

esy_err_t esy_device_registry_start(esy_device_registry_t *registry) {
  esy_device_registry_static_resources_t *resources;

  if (!registry_is_valid(registry)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (atomic_load_u32(&registry->phase) != DEVICE_REGISTRY_PHASE_CONFIGURING) {
    return ESY_ERR_STATE;
  }

  resources = registry->static_resources;
  registry->topology_count = 0u;
  for (uint32_t i = 0u; i < registry->device_count; i++) {
    esy_device_t *device = resources->devices[i];

    for (uint32_t j = 0u; j < device->dependency_count; j++) {
      if (!device_is_valid(device->dependencies[j]) ||
          (device->dependencies[j]->registry != registry)) {
        return ESY_ERR_INVALID_ARG;
      }
    }
    resources->work[i] = device->dependency_count;
  }

  while (registry->topology_count < registry->device_count) {
    uint32_t selected = registry->device_count;

    for (uint32_t i = 0u; i < registry->device_count; i++) {
      if (resources->work[i] == 0u) {
        selected = i;
        break;
      }
    }
    if (selected == registry->device_count) {
      registry->topology_count = 0u;
      return ESY_ERR_STATE;
    }

    resources->topology[registry->topology_count] = selected;
    registry->topology_count++;
    resources->work[selected] = ESY_DEVICE_WORK_DONE;

    for (uint32_t i = 0u; i < registry->device_count; i++) {
      if ((resources->work[i] != ESY_DEVICE_WORK_DONE) &&
          device_has_dependency(resources->devices[i],
                                resources->devices[selected])) {
        resources->work[i]--;
      }
    }
  }

  atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_READY);
  return ESY_OK;
}

bool esy_device_registry_is_ready(const esy_device_registry_t *registry) {
  return registry_is_valid(registry) &&
         (atomic_load_u32(&registry->phase) == DEVICE_REGISTRY_PHASE_READY);
}

esy_err_t esy_device_registry_deinit(esy_device_registry_t *registry) {
  uint32_t phase;
  uint32_t expected;

  if (!registry_is_valid(registry)) {
    return ESY_ERR_INVALID_ARG;
  }
  phase = atomic_load_u32(&registry->phase);
  if ((phase != DEVICE_REGISTRY_PHASE_CONFIGURING) &&
      (phase != DEVICE_REGISTRY_PHASE_READY)) {
    return ESY_ERR_STATE;
  }

  expected = phase;
  if (!atomic_cas_u32(&registry->phase, &expected,
                      DEVICE_REGISTRY_PHASE_TRANSITIONING)) {
    return ESY_ERR_STATE;
  }
  if (!all_references_released(registry)) {
    atomic_store_u32(&registry->phase, phase);
    return ESY_ERR_BUSY;
  }

  for (uint32_t i = 0u; i < registry->device_count; i++) {
    memset(registry->static_resources->devices[i], 0,
           sizeof(*registry->static_resources->devices[i]));
  }
  memset(registry->static_resources->devices, 0,
         registry->static_resources->capacity *
             sizeof(registry->static_resources->devices[0]));
  memset(registry->static_resources->topology, 0,
         registry->static_resources->capacity *
             sizeof(registry->static_resources->topology[0]));
  memset(registry->static_resources->work, 0,
         registry->static_resources->capacity *
             sizeof(registry->static_resources->work[0]));

  registry->device_count = 0u;
  registry->topology_count = 0u;
  registry->suspended_state = ESY_POWER_STATE_ACTIVE;
  registry->static_resources = NULL;
  atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_UNINITIALIZED);
  registry->magic = 0u;
  return ESY_OK;
}

esy_err_t esy_device_register(esy_device_registry_t *registry,
                              esy_device_t *device,
                              const esy_device_config_t *config) {
  const uint32_t deep_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION);

  if (!registry_is_valid(registry) || (device == NULL) || (config == NULL) ||
      (config->name == NULL) ||
      ((config->supported_state_mask &
        ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE)) == 0u) ||
      ((config->supported_state_mask & ~valid_state_mask()) != 0u) ||
      ((config->dependency_count > 0u) && (config->dependencies == NULL)) ||
      (((config->supported_state_mask & deep_mask) != 0u) &&
       ((config->ops == NULL) || (config->ops->pm_action == NULL)))) {
    return ESY_ERR_INVALID_ARG;
  }
  if (atomic_load_u32(&registry->phase) != DEVICE_REGISTRY_PHASE_CONFIGURING) {
    return ESY_ERR_STATE;
  }
  if (device->magic == ESY_DEVICE_MAGIC) {
    return ESY_ERR_STATE;
  }
  if (registry->device_count >= registry->static_resources->capacity) {
    return ESY_ERR_NO_MEM;
  }

  for (uint32_t i = 0u; i < config->dependency_count; i++) {
    if ((config->dependencies[i] == NULL) ||
        (config->dependencies[i] == device)) {
      return ESY_ERR_INVALID_ARG;
    }
    for (uint32_t j = 0u; j < i; j++) {
      if (config->dependencies[i] == config->dependencies[j]) {
        return ESY_ERR_INVALID_ARG;
      }
    }
  }

  memset(device, 0, sizeof(*device));
  device->name = config->name;
  device->ops = config->ops;
  device->driver_ctx = config->driver_ctx;
  device->dependencies = config->dependencies;
  device->dependency_count = config->dependency_count;
  device->supported_state_mask = config->supported_state_mask;
  device->registry = registry;
  atomic_init(&device->usage_count, 0u);
  atomic_init(&device->state, ESY_DEVICE_STATE_ACTIVE);
  device->registry_index = registry->device_count;
  device->magic = ESY_DEVICE_MAGIC;

  registry->static_resources->devices[registry->device_count] = device;
  registry->device_count++;
  return ESY_OK;
}

esy_err_t esy_device_unregister(esy_device_t *device) {
  esy_device_registry_t *registry;
  uint32_t index;

  if (!device_is_valid(device)) {
    return ESY_ERR_INVALID_ARG;
  }
  registry = device->registry;
  if (atomic_load_u32(&registry->phase) != DEVICE_REGISTRY_PHASE_CONFIGURING) {
    return ESY_ERR_STATE;
  }
  if (atomic_load_u32(&device->usage_count) != 0u) {
    return ESY_ERR_BUSY;
  }

  for (uint32_t i = 0u; i < registry->device_count; i++) {
    if ((registry->static_resources->devices[i] != device) &&
        device_has_dependency(registry->static_resources->devices[i], device)) {
      return ESY_ERR_BUSY;
    }
  }

  index = device->registry_index;
  for (uint32_t i = index; i + 1u < registry->device_count; i++) {
    registry->static_resources->devices[i] =
        registry->static_resources->devices[i + 1u];
    registry->static_resources->devices[i]->registry_index = i;
  }
  registry->device_count--;
  registry->static_resources->devices[registry->device_count] = NULL;
  memset(device, 0, sizeof(*device));
  return ESY_OK;
}

esy_err_t esy_device_get(esy_device_t *device) {
  if (!device_is_valid(device)) {
    return ESY_ERR_INVALID_ARG;
  }
  return device_get_internal(device);
}

esy_err_t esy_device_put(esy_device_t *device) {
  if (!device_is_valid(device)) {
    return ESY_ERR_INVALID_ARG;
  }
  return device_put_internal(device);
}

esy_err_t esy_device_put_from_isr(esy_device_t *device) {
  if (!device_is_valid(device)) {
    return ESY_ERR_INVALID_ARG;
  }
  return device_put_internal(device);
}

esy_err_t esy_device_get_usage_count(const esy_device_t *device,
                                     uint32_t *out_usage_count) {
  if (!device_is_valid(device) || (out_usage_count == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  *out_usage_count = atomic_load_u32(&device->usage_count);
  return ESY_OK;
}

esy_err_t esy_device_get_state(const esy_device_t *device,
                               esy_device_state_t *out_state) {
  if (!device_is_valid(device) || (out_state == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  *out_state = (esy_device_state_t)atomic_load_u32(&device->state);
  return ESY_OK;
}

static void mark_runtime_dependency_closure(esy_device_registry_t *registry,
                                            const esy_device_t *device) {
  if (registry->static_resources->work[device->registry_index] != 0u) return;
  registry->static_resources->work[device->registry_index] = 1u;
  for (uint32_t i = 0u; i < device->dependency_count; i++) {
    mark_runtime_dependency_closure(registry, device->dependencies[i]);
  }
}

esy_err_t esy_device_registry_runtime_suspend_idle(
    esy_device_registry_t *registry) {
  uint32_t expected = DEVICE_REGISTRY_PHASE_READY;

  if (!registry_is_valid(registry)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!atomic_cas_u32(&registry->phase, &expected,
                      DEVICE_REGISTRY_PHASE_TRANSITIONING)) {
    return ESY_ERR_STATE;
  }

  memset(registry->static_resources->work, 0,
         registry->static_resources->capacity *
             sizeof(registry->static_resources->work[0]));

  for (uint32_t i = 0u; i < registry->device_count; i++) {
    const esy_device_t *device = registry->static_resources->devices[i];
    /* A held reference also forbids inspecting driver-owned idle state. */
    if ((atomic_load_u32(&device->usage_count) != 0u) ||
        ((device->ops != NULL) && (device->ops->is_runtime_idle != NULL) &&
         !device->ops->is_runtime_idle(device))) {
      mark_runtime_dependency_closure(registry, device);
    }
  }

  for (uint32_t i = registry->topology_count; i > 0u; i--) {
    uint32_t device_index = registry->static_resources->topology[i - 1u];
    esy_device_t *device = registry->static_resources->devices[device_index];
    uint32_t device_state = atomic_load_u32(&device->state);

    if ((registry->static_resources->work[device_index] == 1u) ||
        (atomic_load_u32(&device->usage_count) != 0u) ||
        (device_state == ESY_DEVICE_STATE_RUNTIME_SUSPENDED) ||
        ((device->ops == NULL) || (device->ops->pm_action == NULL))) {
      continue;
    }
    if (device_state != ESY_DEVICE_STATE_ACTIVE) {
      atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_READY);
      return ESY_ERR_STATE;
    }

    atomic_store_u32(&device->state, ESY_DEVICE_STATE_TRANSITIONING);
    esy_err_t ret = device->ops->pm_action(
        device, ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND, ESY_POWER_STATE_ACTIVE);
    if (ret != ESY_OK) {
      if (ret == ESY_ERR_STATE) {
        atomic_store_u32(&device->state, ESY_DEVICE_STATE_FAULTED);
        atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_FAULTED);
        return ret;
      }
      atomic_store_u32(&device->state, ESY_DEVICE_STATE_ACTIVE);

      for (uint32_t j = 0u; j < registry->topology_count; j++) {
        uint32_t rollback_index = registry->static_resources->topology[j];
        esy_device_t *rollback_device =
            registry->static_resources->devices[rollback_index];

        if (registry->static_resources->work[rollback_index] != 2u) {
          continue;
        }
        atomic_store_u32(&rollback_device->state,
                         ESY_DEVICE_STATE_TRANSITIONING);
        esy_err_t rollback_ret = rollback_device->ops->pm_action(
            rollback_device, ESY_DEVICE_PM_ACTION_RUNTIME_RESUME,
            ESY_POWER_STATE_ACTIVE);
        if (rollback_ret != ESY_OK) {
          atomic_store_u32(&rollback_device->state, ESY_DEVICE_STATE_FAULTED);
          atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_FAULTED);
          return ESY_ERR_STATE;
        }
        atomic_store_u32(&rollback_device->state, ESY_DEVICE_STATE_ACTIVE);
      }

      atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_READY);
      return ret;
    }

    atomic_store_u32(&device->state, ESY_DEVICE_STATE_RUNTIME_SUSPENDED);
    registry->static_resources->work[device_index] = 2u;
  }

  atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_READY);
  return ESY_OK;
}

esy_err_t esy_device_registry_get_allowed_state_mask(
    const esy_device_registry_t *registry, uint32_t *out_state_mask) {
  uint32_t allowed_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                                ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE) |
                                ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION);

  if (!registry_is_valid(registry) || (out_state_mask == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (atomic_load_u32(&registry->phase) != DEVICE_REGISTRY_PHASE_READY) {
    return ESY_ERR_STATE;
  }

  for (uint32_t i = 0u; i < registry->device_count; i++) {
    const esy_device_t *device = registry->static_resources->devices[i];

    allowed_state_mask &= device->supported_state_mask;
    if (atomic_load_u32(&device->usage_count) != 0u) {
      allowed_state_mask &= ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                            ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE);
    }
  }

  *out_state_mask = allowed_state_mask;
  return ESY_OK;
}

esy_err_t esy_device_registry_get_deepest_allowed_state(
    const esy_device_registry_t *registry, esy_power_state_t *out_state) {
  uint32_t allowed_state_mask;
  esy_err_t ret;

  if (out_state == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  ret =
      esy_device_registry_get_allowed_state_mask(registry, &allowed_state_mask);
  if (ret != ESY_OK) {
    return ret;
  }
  for (uint32_t state = (uint32_t)ESY_POWER_STATE_RETENTION;; state--) {
    if ((allowed_state_mask & ESY_POWER_STATE_MASK(state)) != 0u) {
      *out_state = (esy_power_state_t)state;
      return ESY_OK;
    }
    if (state == (uint32_t)ESY_POWER_STATE_ACTIVE) {
      break;
    }
  }

  return ESY_ERR_STATE;
}

bool esy_device_registry_is_faulted(const esy_device_registry_t *registry) {
  return registry_is_valid(registry) &&
         (atomic_load_u32(&registry->phase) == DEVICE_REGISTRY_PHASE_FAULTED);
}

esy_err_t esy_device_registry_system_prepare(esy_device_registry_t *registry,
                                             esy_power_state_t state) {
  uint32_t expected = DEVICE_REGISTRY_PHASE_READY;
  esy_err_t ret;

  if (!registry_is_valid(registry) || (state <= ESY_POWER_STATE_ACTIVE) ||
      (state >= ESY_POWER_STATE_COUNT)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!atomic_cas_u32(&registry->phase, &expected,
                      DEVICE_REGISTRY_PHASE_TRANSITIONING)) {
    return ESY_ERR_STATE;
  }

  ret = validate_system_transition(registry, state);
  if (ret != ESY_OK) {
    atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_READY);
    return ret;
  }
  if (state == ESY_POWER_STATE_CPU_IDLE) {
    registry->suspended_state = state;
    atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_PREPARED);
    return ESY_OK;
  }

  registry->suspended_state = state;
  memset(registry->static_resources->work, 0,
         registry->static_resources->capacity *
             sizeof(registry->static_resources->work[0]));
  for (uint32_t i = registry->topology_count; i > 0u; i--) {
    uint32_t device_index = registry->static_resources->topology[i - 1u];
    esy_device_t *device = registry->static_resources->devices[device_index];
    uint32_t previous_state = atomic_load_u32(&device->state);

    registry->static_resources->work[device_index] = previous_state;
    if (previous_state == ESY_DEVICE_STATE_RUNTIME_SUSPENDED) {
      atomic_store_u32(&device->state, ESY_DEVICE_STATE_SYSTEM_SUSPENDED);
      continue;
    }
    if (previous_state != ESY_DEVICE_STATE_ACTIVE) {
      ret = ESY_ERR_STATE;
      goto rollback;
    }

    atomic_store_u32(&device->state, ESY_DEVICE_STATE_TRANSITIONING);
    ret = device->ops->pm_action(device, ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND,
                                 state);
    if (ret != ESY_OK) {
      if (ret == ESY_ERR_STATE) {
        atomic_store_u32(&device->state, ESY_DEVICE_STATE_FAULTED);
        atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_FAULTED);
        return ret;
      }
      atomic_store_u32(&device->state, ESY_DEVICE_STATE_ACTIVE);
      goto rollback;
    }
    atomic_store_u32(&device->state, ESY_DEVICE_STATE_SYSTEM_SUSPENDED);
  }

  atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_PREPARED);
  return ESY_OK;

rollback:
  if (rollback_system_suspend(registry) != ESY_OK) {
    atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_FAULTED);
    return ESY_ERR_STATE;
  }
  registry->suspended_state = ESY_POWER_STATE_ACTIVE;
  atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_READY);
  return ret;
}

esy_err_t esy_device_registry_system_commit_atomic(
    esy_device_registry_t *registry) {
  uint32_t expected = DEVICE_REGISTRY_PHASE_PREPARED;

  if (!registry_is_valid(registry)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!atomic_cas_u32(&registry->phase, &expected,
                      DEVICE_REGISTRY_PHASE_TRANSITIONING)) {
    return ESY_ERR_STATE;
  }
  if ((registry->suspended_state > ESY_POWER_STATE_CPU_IDLE) &&
      !all_references_released(registry)) {
    atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_PREPARED);
    return ESY_ERR_BUSY;
  }
  atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_SUSPENDED);
  return ESY_OK;
}

esy_err_t esy_device_registry_system_abort_atomic(
    esy_device_registry_t *registry) {
  uint32_t expected = DEVICE_REGISTRY_PHASE_PREPARED;

  if (!registry_is_valid(registry)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!atomic_cas_u32(&registry->phase, &expected,
                      DEVICE_REGISTRY_PHASE_ABORTING)) {
    return ESY_ERR_STATE;
  }
  if ((registry->suspended_state > ESY_POWER_STATE_CPU_IDLE) &&
      (recover_system_atomic(registry) != ESY_OK)) {
    atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_FAULTED);
    return ESY_ERR_STATE;
  }
  return ESY_OK;
}

esy_err_t esy_device_registry_system_abort(esy_device_registry_t *registry) {
  uint32_t expected = DEVICE_REGISTRY_PHASE_ABORTING;
  bool early_recovery_done = true;

  if (!registry_is_valid(registry)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!atomic_cas_u32(&registry->phase, &expected,
                      DEVICE_REGISTRY_PHASE_TRANSITIONING)) {
    expected = DEVICE_REGISTRY_PHASE_PREPARED;
    if (!atomic_cas_u32(&registry->phase, &expected,
                        DEVICE_REGISTRY_PHASE_TRANSITIONING)) {
      return ESY_ERR_STATE;
    }
    early_recovery_done = false;
  }
  if ((registry->suspended_state > ESY_POWER_STATE_CPU_IDLE) &&
      ((early_recovery_done ? finish_system_recovery(registry)
                            : rollback_system_suspend(registry)) != ESY_OK)) {
    atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_FAULTED);
    return ESY_ERR_STATE;
  }
  registry->suspended_state = ESY_POWER_STATE_ACTIVE;
  atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_READY);
  return ESY_OK;
}

esy_err_t esy_device_registry_system_resume_atomic(
    esy_device_registry_t *registry) {
  uint32_t expected = DEVICE_REGISTRY_PHASE_SUSPENDED;

  if (!registry_is_valid(registry)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!atomic_cas_u32(&registry->phase, &expected,
                      DEVICE_REGISTRY_PHASE_RESUMING)) {
    return ESY_ERR_STATE;
  }
  if ((registry->suspended_state > ESY_POWER_STATE_CPU_IDLE) &&
      (recover_system_atomic(registry) != ESY_OK)) {
    atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_FAULTED);
    return ESY_ERR_STATE;
  }
  return ESY_OK;
}

esy_err_t esy_device_registry_system_resume(esy_device_registry_t *registry) {
  uint32_t expected = DEVICE_REGISTRY_PHASE_RESUMING;

  if (!registry_is_valid(registry)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (!atomic_cas_u32(&registry->phase, &expected,
                      DEVICE_REGISTRY_PHASE_TRANSITIONING)) {
    return ESY_ERR_STATE;
  }

  esy_err_t ret = ESY_OK;
  if (registry->suspended_state > ESY_POWER_STATE_CPU_IDLE) {
    ret = finish_system_recovery(registry);
  }
  if (ret != ESY_OK) {
    atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_FAULTED);
    return ret;
  }

  registry->suspended_state = ESY_POWER_STATE_ACTIVE;
  atomic_store_u32(&registry->phase, DEVICE_REGISTRY_PHASE_READY);
  return ESY_OK;
}
