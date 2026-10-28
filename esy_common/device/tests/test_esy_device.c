/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/device/esy_device.h"

#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "unity.h"

#define TEST_DEVICE_CAPACITY 8u
#define TEST_EVENT_CAPACITY  32u

typedef struct {
  uint32_t id;
  esy_device_pm_action_t action;
  esy_device_state_t state;
} test_event_t;

typedef struct {
  uint32_t id;
  esy_err_t runtime_suspend_result;
  esy_err_t runtime_resume_result;
  esy_err_t system_suspend_result;
  esy_err_t system_resume_atomic_result;
  esy_err_t system_resume_result;
  uint32_t runtime_idle_checks;
  bool runtime_idle;
  bool block_runtime_resume;
} test_device_ctx_t;

typedef struct {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, TEST_DEVICE_CAPACITY);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry;
  esy_device_t devices[TEST_DEVICE_CAPACITY];
  test_device_ctx_t contexts[TEST_DEVICE_CAPACITY];
  test_event_t events[TEST_EVENT_CAPACITY];
  uint32_t event_count;
  uint32_t atomic_resume_ids[TEST_EVENT_CAPACITY];
  uint32_t atomic_resume_count;
  pthread_mutex_t resume_mutex;
  pthread_cond_t resume_condition;
  bool resume_entered;
  bool resume_released;
  esy_err_t get_result;
} device_test_ctx_t;

static device_test_ctx_t s_ctx;

static esy_err_t test_pm_action(esy_device_t *device,
                                esy_device_pm_action_t action,
                                esy_power_state_t system_state) {
  test_device_ctx_t *ctx = device->driver_ctx;

  (void)system_state;
  if (s_ctx.event_count < TEST_EVENT_CAPACITY) {
    s_ctx.events[s_ctx.event_count].id = ctx->id;
    s_ctx.events[s_ctx.event_count].action = action;
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(
        device, &s_ctx.events[s_ctx.event_count].state));
    s_ctx.event_count++;
  }

  switch (action) {
    case ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND:
      return ctx->runtime_suspend_result;
    case ESY_DEVICE_PM_ACTION_RUNTIME_RESUME:
      if (ctx->block_runtime_resume) {
        assert(pthread_mutex_lock(&s_ctx.resume_mutex) == 0);
        s_ctx.resume_entered = true;
        assert(pthread_cond_signal(&s_ctx.resume_condition) == 0);
        while (!s_ctx.resume_released) {
          assert(pthread_cond_wait(&s_ctx.resume_condition,
                                   &s_ctx.resume_mutex) == 0);
        }
        assert(pthread_mutex_unlock(&s_ctx.resume_mutex) == 0);
      }
      return ctx->runtime_resume_result;
    case ESY_DEVICE_PM_ACTION_SYSTEM_SUSPEND:
      return ctx->system_suspend_result;
    case ESY_DEVICE_PM_ACTION_SYSTEM_RESUME:
      return ctx->system_resume_result;
    default:
      return ESY_ERR_INVALID_ARG;
  }
}

static bool test_is_runtime_idle(const esy_device_t *device) {
  test_device_ctx_t *ctx = device->driver_ctx;
  ++ctx->runtime_idle_checks;
  return ctx->runtime_idle;
}

static esy_err_t test_system_resume_atomic(esy_device_t *device,
                                           esy_power_state_t system_state) {
  test_device_ctx_t *ctx = device->driver_ctx;

  (void)system_state;
  if (s_ctx.atomic_resume_count < TEST_EVENT_CAPACITY) {
    s_ctx.atomic_resume_ids[s_ctx.atomic_resume_count] = ctx->id;
    s_ctx.atomic_resume_count++;
  }
  return ctx->system_resume_atomic_result;
}

static const esy_device_ops_t k_test_ops = {
    .pm_action = test_pm_action,
    .system_resume_atomic = test_system_resume_atomic,
    .is_runtime_idle = test_is_runtime_idle,
};

static uint32_t all_states(void) {
  return ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
         ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE) |
         ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION);
}

static void init_registry(void) {
  esy_device_registry_config_t config;

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&s_ctx.resources, s_ctx.storage);
  config.static_resources = &s_ctx.resources;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_init(&s_ctx.registry, &config));
}

static void register_device(uint32_t index, const char *name,
                            esy_device_t *const *dependencies,
                            uint32_t dependency_count,
                            uint32_t supported_state_mask) {
  esy_device_config_t config = {
      .name = name,
      .ops = &k_test_ops,
      .driver_ctx = &s_ctx.contexts[index],
      .dependencies = dependencies,
      .dependency_count = dependency_count,
      .supported_state_mask = supported_state_mask,
  };

  s_ctx.contexts[index].id = index + 1u;
  s_ctx.contexts[index].runtime_idle = true;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_register(&s_ctx.registry, &s_ctx.devices[index], &config));
}

void setUp(void) {
  memset(&s_ctx.storage, 0, sizeof(s_ctx.storage));
  memset(&s_ctx.resources, 0, sizeof(s_ctx.resources));
  memset(&s_ctx.registry, 0, sizeof(s_ctx.registry));
  memset(s_ctx.devices, 0, sizeof(s_ctx.devices));
  memset(s_ctx.contexts, 0, sizeof(s_ctx.contexts));
  memset(s_ctx.events, 0, sizeof(s_ctx.events));
  memset(s_ctx.atomic_resume_ids, 0, sizeof(s_ctx.atomic_resume_ids));
  s_ctx.event_count = 0u;
  s_ctx.atomic_resume_count = 0u;
  s_ctx.resume_entered = false;
  s_ctx.resume_released = false;
  s_ctx.get_result = ESY_ERR_STATE;
}

void tearDown(void) { (void)esy_device_registry_deinit(&s_ctx.registry); }

void test_zero_initialized_objects_support_first_use_and_deinit_reinit(void) {
  esy_device_registry_t registry = {0};
  esy_device_t device = {0};
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  const esy_device_registry_config_t registry_config = {
      .static_resources = &resources,
  };
  const esy_device_config_t device_config = {
      .name = "local-device",
      .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE),
  };
  uint32_t usage_count;
  esy_device_state_t state;

  memset(&storage, 0xA5, sizeof(storage));
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  for (uint32_t cycle = 0u; cycle < 2u; cycle++) {
    TEST_ASSERT_EQUAL_INT32(
        ESY_OK, esy_device_registry_init(&registry, &registry_config));
    TEST_ASSERT_NULL(resources.devices[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, resources.topology[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, resources.work[0]);
    TEST_ASSERT_EQUAL_INT32(
        ESY_OK, esy_device_register(&registry, &device, &device_config));
    TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_get_state(&device, &state));
    TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_ACTIVE, state);
    TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&registry));
    TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_get(&device));
    TEST_ASSERT_EQUAL_INT32(
        ESY_OK, esy_device_get_usage_count(&device, &usage_count));
    TEST_ASSERT_EQUAL_UINT32(1u, usage_count);
    TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_put(&device));
    TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_deinit(&registry));
    TEST_ASSERT_FALSE(esy_device_registry_is_ready(&registry));
    TEST_ASSERT_NULL(registry.static_resources);
    TEST_ASSERT_NULL(device.registry);
    TEST_ASSERT_NULL(device.name);
    TEST_ASSERT_EQUAL_UINT32(0u, device.magic);
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_device_get(&device));
  }
}

void test_failed_first_registry_init_can_retry_and_repeated_init_preserves_graph(void) {
  esy_device_registry_t registry = {0};
  esy_device_t device = {0};
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  const esy_device_registry_config_t config = {
      .static_resources = &resources,
  };
  const esy_device_config_t device_config = {
      .name = "preserved-device",
      .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE),
  };

  memset(&storage, 0xA5, sizeof(storage));
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  resources.capacity = 0u;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_device_registry_init(&registry, &config));
  TEST_ASSERT_NULL(registry.static_resources);
  TEST_ASSERT_EQUAL_UINT32(0u, registry.magic);
  TEST_ASSERT_EACH_EQUAL_HEX8(0xA5, &storage, sizeof(storage));

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_init(&registry, &config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_register(&registry, &device, &device_config));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_device_registry_init(&registry, &config));
  TEST_ASSERT_EQUAL_PTR(&resources, registry.static_resources);
  TEST_ASSERT_EQUAL_UINT32(1u, registry.device_count);
  TEST_ASSERT_EQUAL_PTR(&device, resources.devices[0]);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_deinit(&registry));
}

void test_failed_registration_retries_and_successful_unregister_allows_reuse(void) {
  esy_device_registry_t registry = {0};
  esy_device_t device = {0};
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  const esy_device_registry_config_t registry_config = {
      .static_resources = &resources,
  };
  esy_device_config_t config = {
      .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE),
  };

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_device_register(&registry, &device, &config));
  TEST_ASSERT_NULL(device.registry);
  TEST_ASSERT_EQUAL_UINT32(0u, device.magic);
  TEST_ASSERT_EQUAL_UINT32(0u, registry.device_count);

  config.name = "first-registration";
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_register(&registry, &device, &config));
  config.name = "second-registration";
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_device_register(&registry, &device, &config));
  TEST_ASSERT_EQUAL_STRING("first-registration", device.name);
  TEST_ASSERT_EQUAL_UINT32(1u, registry.device_count);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_unregister(&device));
  TEST_ASSERT_NULL(device.registry);
  TEST_ASSERT_EQUAL_UINT32(0u, registry.device_count);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_register(&registry, &device, &config));
  TEST_ASSERT_EQUAL_STRING("second-registration", device.name);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_unregister(&device));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_deinit(&registry));
}

void test_busy_registry_deinit_retains_objects_until_reference_release(void) {
  esy_device_registry_t registry = {0};
  esy_device_t device = {0};
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 1u);
  esy_device_registry_static_resources_t resources;
  const esy_device_registry_config_t registry_config = {
      .static_resources = &resources,
  };
  const esy_device_config_t device_config = {
      .name = "busy-device",
      .supported_state_mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE),
  };
  uint32_t usage_count;

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&resources, storage);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_register(&registry, &device, &device_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_get(&device));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_device_registry_deinit(&registry));
  TEST_ASSERT_TRUE(esy_device_registry_is_ready(&registry));
  TEST_ASSERT_EQUAL_PTR(&registry, device.registry);
  TEST_ASSERT_EQUAL_PTR(&device, resources.devices[0]);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_get_usage_count(&device, &usage_count));
  TEST_ASSERT_EQUAL_UINT32(1u, usage_count);
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE, esy_device_registry_init(&registry, &registry_config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_put(&device));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_deinit(&registry));
  TEST_ASSERT_NULL(device.registry);
  TEST_ASSERT_NULL(registry.static_resources);
}

void test_public_queries_and_transitions_reject_invalid_state(void) {
  uint32_t usage_count;
  esy_device_state_t device_state;
  esy_power_state_t power_state;

  TEST_ASSERT_FALSE(esy_device_registry_is_faulted(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_device_put(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_device_put_from_isr(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_device_get_usage_count(NULL, &usage_count));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_device_get_state(NULL, &device_state));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_device_registry_runtime_suspend_idle(NULL));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_device_registry_get_deepest_allowed_state(NULL, &power_state));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_device_registry_system_prepare(NULL, ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_device_registry_system_resume_atomic(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_device_registry_system_resume(NULL));

  init_registry();
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE,
      esy_device_registry_get_deepest_allowed_state(&s_ctx.registry, &power_state));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE, esy_device_registry_runtime_suspend_idle(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_device_registry_system_prepare(&s_ctx.registry, ESY_POWER_STATE_ACTIVE));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE, esy_device_registry_system_resume_atomic(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_device_registry_system_resume(&s_ctx.registry));
}

void test_registry_accepts_child_before_parent_and_builds_topology(void) {
  esy_device_t *child_dependencies[] = {&s_ctx.devices[0]};

  init_registry();
  register_device(1u, "child", child_dependencies, 1u, all_states());
  register_device(0u, "parent", NULL, 0u, all_states());

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&s_ctx.registry));
  TEST_ASSERT_EQUAL_PTR(&s_ctx.devices[0],
                        s_ctx.resources.devices[s_ctx.resources.topology[0]]);
  TEST_ASSERT_EQUAL_PTR(&s_ctx.devices[1],
                        s_ctx.resources.devices[s_ctx.resources.topology[1]]);
}

void test_registry_rejects_dependency_cycle(void) {
  esy_device_t *a_dependencies[] = {&s_ctx.devices[1]};
  esy_device_t *b_dependencies[] = {&s_ctx.devices[0]};

  init_registry();
  register_device(0u, "a", a_dependencies, 1u, all_states());
  register_device(1u, "b", b_dependencies, 1u, all_states());

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_device_registry_start(&s_ctx.registry));
}

void test_registry_rejects_dependency_from_another_registry(void) {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(other_storage, 1u);
  esy_device_registry_static_resources_t other_resources;
  esy_device_registry_t other_registry = {0};
  esy_device_registry_config_t other_config;
  esy_device_t other_device = {0};
  esy_device_t *dependencies[] = {&other_device};
  esy_device_config_t device_config = {
      .name = "other",
      .ops = &k_test_ops,
      .driver_ctx = &s_ctx.contexts[0],
      .supported_state_mask = all_states(),
  };

  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&other_resources, other_storage);
  other_config.static_resources = &other_resources;
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_init(&other_registry, &other_config));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK,
      esy_device_register(&other_registry, &other_device, &device_config));

  init_registry();
  register_device(0u, "child", dependencies, 1u, all_states());
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_device_registry_start(&s_ctx.registry));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_deinit(&other_registry));
}

void test_device_get_and_put_propagate_dependency_references(void) {
  esy_device_t *dependencies[] = {&s_ctx.devices[0]};
  uint32_t usage_count;

  init_registry();
  register_device(0u, "parent", NULL, 0u, all_states());
  register_device(1u, "child", dependencies, 1u, all_states());
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&s_ctx.registry));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_get(&s_ctx.devices[1]));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_get_usage_count(&s_ctx.devices[0], &usage_count));
  TEST_ASSERT_EQUAL_UINT32(1u, usage_count);
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_get_usage_count(&s_ctx.devices[1], &usage_count));
  TEST_ASSERT_EQUAL_UINT32(1u, usage_count);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_put(&s_ctx.devices[1]));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_get_usage_count(&s_ctx.devices[0], &usage_count));
  TEST_ASSERT_EQUAL_UINT32(0u, usage_count);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_device_put(&s_ctx.devices[1]));
}

void test_runtime_suspend_is_child_first_and_get_resumes_parent_first(void) {
  esy_device_t *dependencies[] = {&s_ctx.devices[0]};
  esy_device_state_t state;

  init_registry();
  register_device(0u, "parent", NULL, 0u, all_states());
  register_device(1u, "child", dependencies, 1u, all_states());
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&s_ctx.registry));

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_runtime_suspend_idle(&s_ctx.registry));
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.event_count);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.events[0].id);
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_PM_ACTION_RUNTIME_SUSPEND,
                        s_ctx.events[0].action);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.events[1].id);

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_get(&s_ctx.devices[1]));
  TEST_ASSERT_EQUAL_UINT32(4u, s_ctx.event_count);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.events[2].id);
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_PM_ACTION_RUNTIME_RESUME,
                        s_ctx.events[2].action);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.events[3].id);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_get_state(&s_ctx.devices[1], &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_ACTIVE, state);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_put(&s_ctx.devices[1]));
}

static void *get_device_on_worker(void *device) {
  s_ctx.get_result = esy_device_get(device);
  return NULL;
}

void test_get_protects_in_flight_runtime_resume_from_idle_suspend(void) {
  pthread_t worker;
  struct timespec deadline;
  uint32_t usage_count = 0u;
  esy_err_t suspend_result = ESY_ERR_STATE;
  esy_err_t usage_result = ESY_ERR_STATE;
  int wait_result = 0;
  bool resume_entered;

  init_registry();
  register_device(0u, "resuming", NULL, 0u, all_states());
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_runtime_suspend_idle(&s_ctx.registry));
  s_ctx.contexts[0].block_runtime_resume = true;
  TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&s_ctx.resume_mutex, NULL));
  TEST_ASSERT_EQUAL_INT(0, pthread_cond_init(&s_ctx.resume_condition, NULL));
  TEST_ASSERT_EQUAL_INT(TIME_UTC, timespec_get(&deadline, TIME_UTC));
  deadline.tv_sec += 5;
  TEST_ASSERT_EQUAL_INT(0, pthread_create(&worker, NULL, get_device_on_worker,
                                         &s_ctx.devices[0]));
  assert(pthread_mutex_lock(&s_ctx.resume_mutex) == 0);
  while (!s_ctx.resume_entered && wait_result == 0) {
    wait_result = pthread_cond_timedwait(&s_ctx.resume_condition,
                                         &s_ctx.resume_mutex, &deadline);
  }
  resume_entered = s_ctx.resume_entered;
  assert(pthread_mutex_unlock(&s_ctx.resume_mutex) == 0);

  /* The getter is inside its resume callback while the PM owner scans. */
  if (resume_entered) {
    usage_result = esy_device_get_usage_count(&s_ctx.devices[0], &usage_count);
    suspend_result = esy_device_registry_runtime_suspend_idle(&s_ctx.registry);
  }

  /* Release and join before Unity assertions so failures cannot strand it. */
  assert(pthread_mutex_lock(&s_ctx.resume_mutex) == 0);
  s_ctx.resume_released = true;
  assert(pthread_cond_signal(&s_ctx.resume_condition) == 0);
  assert(pthread_mutex_unlock(&s_ctx.resume_mutex) == 0);
  TEST_ASSERT_EQUAL_INT(0, pthread_join(worker, NULL));
  TEST_ASSERT_EQUAL_INT(0, pthread_cond_destroy(&s_ctx.resume_condition));
  TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&s_ctx.resume_mutex));

  TEST_ASSERT_TRUE(resume_entered);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, suspend_result);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, usage_result);
  TEST_ASSERT_EQUAL_UINT32(1u, usage_count);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, s_ctx.get_result);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.event_count);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.contexts[0].runtime_idle_checks);
  TEST_ASSERT_TRUE(esy_device_registry_is_ready(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_put(&s_ctx.devices[0]));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_runtime_suspend_idle(&s_ctx.registry));
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.event_count);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.contexts[0].runtime_idle_checks);
}

void test_failed_runtime_resume_releases_device_and_dependency_references(void) {
  esy_device_t *dependencies[] = {&s_ctx.devices[0]};
  uint32_t usage_count;

  init_registry();
  register_device(0u, "parent", NULL, 0u, all_states());
  register_device(1u, "child", dependencies, 1u, all_states());
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_runtime_suspend_idle(&s_ctx.registry));
  s_ctx.contexts[1].runtime_resume_result = ESY_ERR_IO;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_device_get(&s_ctx.devices[1]));
  TEST_ASSERT_TRUE(esy_device_registry_is_faulted(&s_ctx.registry));
  for (uint32_t i = 0u; i < 2u; i++) {
    TEST_ASSERT_EQUAL_INT32(
        ESY_OK, esy_device_get_usage_count(&s_ctx.devices[i], &usage_count));
    TEST_ASSERT_EQUAL_UINT32(0u, usage_count);
  }
}

void test_runtime_suspend_failure_rolls_back_parent_first(void) {
  esy_device_t *dependencies[] = {&s_ctx.devices[0]};
  esy_device_state_t state;

  init_registry();
  register_device(0u, "parent", NULL, 0u, all_states());
  register_device(1u, "child", dependencies, 1u, all_states());
  s_ctx.contexts[0].runtime_suspend_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&s_ctx.registry));

  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_IO, esy_device_registry_runtime_suspend_idle(&s_ctx.registry));
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.event_count);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.events[0].id);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.events[1].id);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.events[2].id);
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_PM_ACTION_RUNTIME_RESUME,
                        s_ctx.events[2].action);
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_TRANSITIONING, s_ctx.events[2].state);
  for (uint32_t i = 0u; i < 2u; ++i) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&s_ctx.devices[i], &state));
    TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_ACTIVE, state);
  }
  TEST_ASSERT_TRUE(esy_device_registry_is_ready(&s_ctx.registry));
  TEST_ASSERT_FALSE(esy_device_registry_is_faulted(&s_ctx.registry));
}

void test_runtime_suspend_skips_device_with_persistent_activity(void) {
  esy_device_state_t state;

  init_registry();
  register_device(0u, "persistent", NULL, 0u, all_states());
  s_ctx.contexts[0].runtime_idle = false;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&s_ctx.registry));

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_runtime_suspend_idle(&s_ctx.registry));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.event_count);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_get_state(&s_ctx.devices[0], &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_ACTIVE, state);
}

void test_runtime_idle_checks_wait_for_reference_release_and_protect_dependencies(
    void) {
  esy_device_t *parent_dependencies[] = {&s_ctx.devices[0]};
  esy_device_t *child_dependencies[] = {&s_ctx.devices[1]};
  esy_device_state_t state;

  init_registry();
  register_device(0u, "root", NULL, 0u, all_states());
  register_device(1u, "parent", parent_dependencies, 1u, all_states());
  register_device(2u, "child", child_dependencies, 1u, all_states());
  register_device(3u, "unrelated", NULL, 0u, all_states());
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&s_ctx.registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get(&s_ctx.devices[2]));

  TEST_ASSERT_EQUAL(
      ESY_OK, esy_device_registry_runtime_suspend_idle(&s_ctx.registry));
  for (uint32_t i = 0u; i < 3u; ++i) {
    TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.contexts[i].runtime_idle_checks);
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&s_ctx.devices[i], &state));
    TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_ACTIVE, state);
  }
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.contexts[3].runtime_idle_checks);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.event_count);
  TEST_ASSERT_EQUAL_UINT32(4u, s_ctx.events[0].id);

  TEST_ASSERT_EQUAL(ESY_OK, esy_device_put(&s_ctx.devices[2]));
  TEST_ASSERT_EQUAL(
      ESY_OK, esy_device_registry_runtime_suspend_idle(&s_ctx.registry));
  for (uint32_t i = 0u; i < 3u; ++i) {
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.contexts[i].runtime_idle_checks);
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&s_ctx.devices[i], &state));
    TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_RUNTIME_SUSPENDED, state);
  }
  TEST_ASSERT_EQUAL_UINT32(4u, s_ctx.event_count);
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.events[1].id);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.events[2].id);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.events[3].id);
}

void test_persistent_idle_check_preserves_zero_reference_dependencies(void) {
  esy_device_t *dependencies[] = {&s_ctx.devices[0]};

  init_registry();
  register_device(0u, "parent", NULL, 0u, all_states());
  register_device(1u, "persistent-child", dependencies, 1u, all_states());
  s_ctx.contexts[1].runtime_idle = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&s_ctx.registry));

  TEST_ASSERT_EQUAL(
      ESY_OK, esy_device_registry_runtime_suspend_idle(&s_ctx.registry));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.contexts[0].runtime_idle_checks);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.contexts[1].runtime_idle_checks);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.event_count);

  s_ctx.contexts[1].runtime_idle = true;
  TEST_ASSERT_EQUAL(
      ESY_OK, esy_device_registry_runtime_suspend_idle(&s_ctx.registry));
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.contexts[1].runtime_idle_checks);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.event_count);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.events[0].id);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.events[1].id);
}

void test_system_suspend_and_resume_follow_dependency_order(void) {
  esy_device_t *dependencies[] = {&s_ctx.devices[0]};

  init_registry();
  register_device(0u, "parent", NULL, 0u, all_states());
  register_device(1u, "child", dependencies, 1u, all_states());
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&s_ctx.registry));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_system_prepare(
                                      &s_ctx.registry, ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.events[0].id);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.events[1].id);

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_system_commit_atomic(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_system_resume_atomic(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_system_resume(&s_ctx.registry));
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.atomic_resume_count);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.atomic_resume_ids[0]);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.atomic_resume_ids[1]);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.events[2].id);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.events[3].id);
}

void test_system_suspend_failure_rolls_back_suspended_children(void) {
  esy_device_t *dependencies[] = {&s_ctx.devices[0]};
  esy_device_state_t state;

  init_registry();
  register_device(0u, "parent", NULL, 0u, all_states());
  register_device(1u, "child", dependencies, 1u, all_states());
  s_ctx.contexts[0].system_suspend_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&s_ctx.registry));

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO,
                          esy_device_registry_system_prepare(
                              &s_ctx.registry, ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.event_count);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.events[2].id);
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_PM_ACTION_SYSTEM_RESUME, s_ctx.events[2].action);
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_TRANSITIONING, s_ctx.events[2].state);
  for (uint32_t i = 0u; i < 2u; ++i) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&s_ctx.devices[i], &state));
    TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_ACTIVE, state);
  }
  TEST_ASSERT_TRUE(esy_device_registry_is_ready(&s_ctx.registry));
  TEST_ASSERT_FALSE(esy_device_registry_is_faulted(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_device_registry_system_resume(&s_ctx.registry));
}

void test_active_reference_allows_idle_but_blocks_retention(void) {
  esy_power_state_t state;

  init_registry();
  register_device(0u, "device", NULL, 0u, all_states());
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_get(&s_ctx.devices[0]));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_get_deepest_allowed_state(
                                      &s_ctx.registry, &state));
  TEST_ASSERT_EQUAL_INT(ESY_POWER_STATE_CPU_IDLE, state);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_system_prepare(
                                      &s_ctx.registry, ESY_POWER_STATE_CPU_IDLE));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_system_commit_atomic(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_system_resume_atomic(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_system_resume(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY,
                          esy_device_registry_system_prepare(
                              &s_ctx.registry, ESY_POWER_STATE_RETENTION));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_put_from_isr(&s_ctx.devices[0]));
}

void test_unsupported_state_fails_closed(void) {
  const uint32_t idle_only = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                             ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE);
  esy_power_state_t state;

  init_registry();
  register_device(0u, "idle-only", NULL, 0u, idle_only);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&s_ctx.registry));

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_get_deepest_allowed_state(
                                      &s_ctx.registry, &state));
  TEST_ASSERT_EQUAL_INT(ESY_POWER_STATE_CPU_IDLE, state);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_NOT_SUPPORTED,
                          esy_device_registry_system_prepare(
                              &s_ctx.registry, ESY_POWER_STATE_RETENTION));
}

void test_capability_selection_intersects_non_contiguous_masks(void) {
  const uint32_t retention_only =
      ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
      ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION);
  const uint32_t idle_only = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
                             ESY_POWER_STATE_MASK(ESY_POWER_STATE_CPU_IDLE);
  esy_power_state_t state;
  uint32_t mask;

  init_registry();
  register_device(0u, "retention", NULL, 0u, retention_only);
  register_device(1u, "idle", NULL, 0u, idle_only);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&s_ctx.registry));

  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_get_allowed_state_mask(&s_ctx.registry, &mask));
  TEST_ASSERT_EQUAL_UINT32(ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE), mask);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_get_deepest_allowed_state(
                                      &s_ctx.registry, &state));
  TEST_ASSERT_EQUAL_INT(ESY_POWER_STATE_ACTIVE, state);
}

void test_automatic_selection_returns_deepest_resumable_state(void) {
  esy_power_state_t state;

  init_registry();
  register_device(0u, "all", NULL, 0u, all_states());
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_get_deepest_allowed_state(
                                      &s_ctx.registry, &state));
  TEST_ASSERT_EQUAL_INT(ESY_POWER_STATE_RETENTION, state);
}

void test_system_prepare_preserves_runtime_suspended_device_state(void) {
  esy_device_state_t state;

  init_registry();
  register_device(0u, "device", NULL, 0u, all_states());
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_runtime_suspend_idle(&s_ctx.registry));
  s_ctx.event_count = 0u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_system_prepare(
                                      &s_ctx.registry, ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_system_commit_atomic(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(
      ESY_OK, esy_device_registry_system_resume_atomic(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_system_resume(&s_ctx.registry));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.event_count);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_get_state(&s_ctx.devices[0], &state));
  TEST_ASSERT_EQUAL_INT(ESY_DEVICE_STATE_RUNTIME_SUSPENDED, state);
}

void test_atomic_commit_rechecks_late_usage_reference(void) {
  init_registry();
  register_device(0u, "device", NULL, 0u, all_states());
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_system_prepare(
                                      &s_ctx.registry, ESY_POWER_STATE_RETENTION));
  atomic_store(&s_ctx.devices[0].usage_count, 1u);
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_BUSY, esy_device_registry_system_commit_atomic(&s_ctx.registry));
  atomic_store(&s_ctx.devices[0].usage_count, 0u);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_system_abort_atomic(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                          esy_device_registry_system_abort(&s_ctx.registry));
}

void test_unregister_rejects_dependency_and_invalidates_device(void) {
  esy_device_t *dependencies[] = {&s_ctx.devices[0]};

  init_registry();
  register_device(0u, "parent", NULL, 0u, all_states());
  register_device(1u, "child", dependencies, 1u, all_states());

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_device_unregister(&s_ctx.devices[0]));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_unregister(&s_ctx.devices[1]));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_unregister(&s_ctx.devices[0]));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_device_get(&s_ctx.devices[0]));
}

void test_registry_lifecycle_validates_storage_capacity_and_busy_deinit(void) {
  esy_device_registry_config_t invalid_config = {0};
  esy_device_config_t invalid_device = {0};

  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_device_registry_init(&s_ctx.registry, &invalid_config));

  init_registry();
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_device_register(&s_ctx.registry, &s_ctx.devices[0], &invalid_device));
  register_device(0u, "device", NULL, 0u, all_states());
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_device_registry_start(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_get(&s_ctx.devices[0]));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY,
                          esy_device_registry_deinit(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_put(&s_ctx.devices[0]));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_deinit(&s_ctx.registry));
}

void test_registry_public_api_rejects_invalid_handles_and_outputs(void) {
  TEST_ASSERT_FALSE(esy_device_registry_is_ready(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_device_registry_start(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_device_registry_deinit(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_device_unregister(NULL));

  init_registry();
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE,
      esy_device_registry_init(&s_ctx.registry,
                               &(esy_device_registry_config_t){
                                   .static_resources = &s_ctx.resources,
                               }));
  register_device(0u, "device", NULL, 0u, all_states());
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_device_get_usage_count(&s_ctx.devices[0], NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                          esy_device_get_state(&s_ctx.devices[0], NULL));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_INVALID_ARG,
      esy_device_registry_get_allowed_state_mask(&s_ctx.registry, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_device_registry_start(&s_ctx.registry));
  TEST_ASSERT_TRUE(esy_device_registry_is_ready(&s_ctx.registry));
  TEST_ASSERT_EQUAL_INT32(
      ESY_ERR_STATE,
      esy_device_register(
          &s_ctx.registry, &s_ctx.devices[1],
          &(esy_device_config_t){
              .name = "late",
              .ops = &k_test_ops,
              .driver_ctx = &s_ctx.contexts[1],
              .supported_state_mask = all_states(),
          }));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                          esy_device_unregister(&s_ctx.devices[0]));
}

void test_runtime_suspend_unrecoverable_callback_faults_registry(void) {
  esy_device_state_t state;
  init_registry();
  register_device(0u, "faulting", NULL, 0u, all_states());
  s_ctx.contexts[0].runtime_suspend_result = ESY_ERR_STATE;
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&s_ctx.registry));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_device_registry_runtime_suspend_idle(&s_ctx.registry));
  TEST_ASSERT_TRUE(esy_device_registry_is_faulted(&s_ctx.registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&s_ctx.devices[0], &state));
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_FAULTED, state);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_device_get(&s_ctx.devices[0]));
  TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
}

void test_system_suspend_unrecoverable_callback_faults_registry(void) {
  esy_device_state_t state;
  init_registry();
  register_device(0u, "faulting", NULL, 0u, all_states());
  s_ctx.contexts[0].system_suspend_result = ESY_ERR_STATE;
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&s_ctx.registry));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_device_registry_system_prepare(
      &s_ctx.registry, ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_TRUE(esy_device_registry_is_faulted(&s_ctx.registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&s_ctx.devices[0], &state));
  TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_FAULTED, state);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_device_get(&s_ctx.devices[0]));
  TEST_ASSERT_EQUAL(1u, s_ctx.event_count);
}
