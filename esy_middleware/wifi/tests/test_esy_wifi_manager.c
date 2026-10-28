/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/wifi/esy_wifi_manager.h"
#include "esy_middleware/wifi/esy_wifi_manager_device.h"
#include "fake_esy_hal_wifi.h"
#include "unity.h"

#include <string.h>

typedef struct {
  esy_wifi_manager_t manager;
  esy_wifi_manager_t other;
  esy_wifi_manager_device_binding_t binding;
  esy_wifi_manager_device_binding_t other_binding;
  esy_device_registry_t registry;
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 2u);
  esy_device_registry_static_resources_t resources;
  esy_wifi_manager_config_t config;
  esy_device_registration_config_t device_config;
  fake_esy_hal_wifi_state_t *fake;
} wifi_test_ctx_t;
static wifi_test_ctx_t s_ctx;

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_hal_wifi_reset();
  s_ctx.fake = fake_esy_hal_wifi_state();
  esy_wifi_manager_default_cfg(&s_ctx.config);
  s_ctx.config.sta_config.ssid = "test";
  s_ctx.config.sta_config.password = "secret";
  s_ctx.device_config.name = "wifi";
  s_ctx.device_config.supported_state_mask =
      ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
      ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION);
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&s_ctx.resources, s_ctx.storage);
  esy_device_registry_config_t config = {.static_resources = &s_ctx.resources};
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&s_ctx.registry, &config));
}

void tearDown(void) {
  (void)esy_device_registry_system_resume(&s_ctx.registry);
  (void)esy_device_registry_deinit(&s_ctx.registry);
  if (s_ctx.binding.manager != NULL) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_unregister_device(&s_ctx.binding));
  }
  if (s_ctx.other_binding.manager != NULL) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_unregister_device(&s_ctx.other_binding));
  }
  if (s_ctx.manager.initialized) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_deinit(&s_ctx.manager));
  }
  if (s_ctx.other.initialized) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_deinit(&s_ctx.other));
  }
}

static void initialize(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_init(&s_ctx.manager, &s_ctx.config));
}

static void bind_device(bool start) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_register_device(
      &s_ctx.binding, &s_ctx.manager, &s_ctx.registry, &s_ctx.device_config));
  if (start) TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&s_ctx.registry));
}

static void connect(void) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_request_connect(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_process(&s_ctx.manager));
}

void test_first_init_accepts_uninitialized_storage_and_copies_credentials(void) {
  esy_wifi_manager_t fresh;
  char ssid[] = "original";
  char password[] = "password";
  s_ctx.config.sta_config.ssid = ssid;
  s_ctx.config.sta_config.password = password;
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_init(&fresh, &s_ctx.config));
  memset(ssid, 'X', sizeof(ssid) - 1u);
  memset(password, 'X', sizeof(password) - 1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_request_connect(&fresh));
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_process(&fresh));
  TEST_ASSERT_EQUAL_STRING("original", s_ctx.fake->last_config.ssid);
  TEST_ASSERT_EQUAL_STRING("password", s_ctx.fake->last_config.password);
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_deinit(&fresh));
}

void test_invalid_init_leaves_storage_unchanged_and_allows_retry(void) {
  esy_wifi_manager_t fresh;
  unsigned char before[sizeof(fresh)];
  memset(&fresh, 0xA5, sizeof(fresh));
  memcpy(before, &fresh, sizeof(fresh));
  s_ctx.config.sta_config.ssid = "";
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_wifi_manager_init(&fresh, &s_ctx.config));
  TEST_ASSERT_EQUAL_MEMORY(before, &fresh, sizeof(fresh));
  s_ctx.config.sta_config.ssid = "valid";
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_init(&fresh, &s_ctx.config));
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_deinit(&fresh));
}

void test_live_reinit_and_second_owner_are_rejected_without_losing_binding(void) {
  initialize(); bind_device(false);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_wifi_manager_init(&s_ctx.manager, &s_ctx.config));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_wifi_manager_init(&s_ctx.other, &s_ctx.config));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_wifi_manager_deinit(&s_ctx.manager));
  TEST_ASSERT_EQUAL_PTR(&s_ctx.binding.device, s_ctx.manager.device);
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_unregister_device(&s_ctx.binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_deinit(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_wifi_manager_deinit(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_init(&s_ctx.other, &s_ctx.config));
}

void test_reusing_live_binding_or_manager_keeps_graph_unchanged(void) {
  initialize(); bind_device(false);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_wifi_manager_register_device(
      &s_ctx.binding, &s_ctx.other, &s_ctx.registry, &s_ctx.device_config));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_wifi_manager_register_device(
      &s_ctx.binding, &s_ctx.manager, &s_ctx.registry, &s_ctx.device_config));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_wifi_manager_register_device(
      &s_ctx.other_binding, &s_ctx.manager, &s_ctx.registry, &s_ctx.device_config));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.registry.device_count);
  TEST_ASSERT_EQUAL_PTR(&s_ctx.manager, s_ctx.binding.manager);
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_unregister_device(&s_ctx.binding));
  bind_device(false);
}

void test_registration_failure_can_retry_and_unregister_failure_retains_binding(void) {
  initialize();
  s_ctx.device_config.name = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_wifi_manager_register_device(
      &s_ctx.binding, &s_ctx.manager, &s_ctx.registry, &s_ctx.device_config));
  TEST_ASSERT_NULL(s_ctx.binding.manager);
  TEST_ASSERT_NULL(s_ctx.manager.device);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.registry.device_count);
  s_ctx.device_config.name = "wifi";
  bind_device(true);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_wifi_manager_unregister_device(&s_ctx.binding));
  TEST_ASSERT_EQUAL_PTR(&s_ctx.binding.device, s_ctx.manager.device);
}

void test_registry_teardown_detaches_binding_and_allows_new_lifecycle(void) {
  initialize(); bind_device(true); connect();
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&s_ctx.registry));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_wifi_manager_deinit(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_unregister_device(&s_ctx.binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_deinit(&s_ctx.manager));
  initialize();
}

void test_request_survives_runtime_suspend_and_resume(void) {
  initialize(); bind_device(true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_runtime_suspend_idle(&s_ctx.registry));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.fake->deinit_calls);
  connect();
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.fake->start_calls);
  TEST_ASSERT_TRUE(esy_wifi_manager_is_connected(&s_ctx.manager));
}

void test_connected_manager_survives_system_suspend_resume(void) {
  initialize(); bind_device(true); connect();
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_runtime_suspend_idle(&s_ctx.registry));
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.fake->deinit_calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_prepare(
      &s_ctx.registry, ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_FALSE(esy_wifi_manager_is_connected(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_commit_atomic(&s_ctx.registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_resume_atomic(&s_ctx.registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_resume(&s_ctx.registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_process(&s_ctx.manager));
  TEST_ASSERT_TRUE(esy_wifi_manager_is_connected(&s_ctx.manager));
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.fake->start_calls);
}

void test_system_abort_and_new_request_restore_connection_intent(void) {
  initialize(); bind_device(true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_prepare(
      &s_ctx.registry, ESY_POWER_STATE_RETENTION));
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_request_connect(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_abort(&s_ctx.registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_process(&s_ctx.manager));
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.fake->start_calls);
}

void test_repeated_request_does_not_restart_in_progress_attempt(void) {
  initialize();
  s_ctx.fake->process_result = ESY_ERR_BUSY;
  connect(); connect();
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.fake->start_calls);
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.fake->process_calls);
  TEST_ASSERT_EQUAL(ESY_WIFI_MANAGER_STATE_CONNECTING, esy_wifi_manager_get_state(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_get_last_error(&s_ctx.manager));
}

void test_backend_start_contention_defers_connection_and_stop_cancels_the_retry(void) {
  initialize();
  bind_device(true);
  s_ctx.fake->start_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_request_connect(&s_ctx.manager));
  for (uint32_t attempt = 0u; attempt < 3u; ++attempt) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_process(&s_ctx.manager));
    TEST_ASSERT_EQUAL(ESY_WIFI_MANAGER_STATE_CONNECTING,
                      esy_wifi_manager_get_state(&s_ctx.manager));
    TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_get_last_error(&s_ctx.manager));
    TEST_ASSERT_FALSE(s_ctx.manager.connect_started);
    uint32_t usage;
    TEST_ASSERT_EQUAL(ESY_OK,
        esy_device_get_usage_count(&s_ctx.binding.device, &usage));
    TEST_ASSERT_EQUAL_UINT32(0u, usage);
  }
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.fake->process_calls);
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_stop(&s_ctx.manager));
  s_ctx.fake->start_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_process(&s_ctx.manager));
  TEST_ASSERT_EQUAL_UINT32(3u, s_ctx.fake->start_calls);
  TEST_ASSERT_FALSE(esy_wifi_manager_is_connected(&s_ctx.manager));

  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_request_connect(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_process(&s_ctx.manager));
  TEST_ASSERT_EQUAL_UINT32(4u, s_ctx.fake->start_calls);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.fake->process_calls);
  TEST_ASSERT_TRUE(esy_wifi_manager_is_connected(&s_ctx.manager));
}

void test_delayed_ipv4_and_address_change_are_refreshed(void) {
  esy_wifi_ipv4_addr_t address = {.octets = {9u, 9u, 9u, 9u}};
  initialize(); s_ctx.fake->address_result = ESY_ERR_STATE; connect();
  TEST_ASSERT_TRUE(esy_wifi_manager_is_connected(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_wifi_manager_get_last_error(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_wifi_manager_get_ipv4_addr(&s_ctx.manager, &address));
  TEST_ASSERT_EQUAL_UINT8(9u, address.octets[0]);
  s_ctx.fake->address_result = ESY_OK;
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_process(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_get_ipv4_addr(&s_ctx.manager, &address));
  TEST_ASSERT_EQUAL_MEMORY(&s_ctx.fake->address, &address, sizeof(address));
  s_ctx.fake->address.octets[3] = 42u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_process(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_get_ipv4_addr(&s_ctx.manager, &address));
  TEST_ASSERT_EQUAL_UINT8(42u, address.octets[3]);
  TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.fake->start_calls);
}

void test_address_io_error_and_link_loss_are_reported_without_stale_address(void) {
  esy_wifi_ipv4_addr_t address;
  initialize(); connect();
  s_ctx.fake->address_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_wifi_manager_process(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_wifi_manager_get_last_error(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_wifi_manager_get_ipv4_addr(&s_ctx.manager, &address));
  s_ctx.fake->connected = false; s_ctx.fake->process_result = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_process(&s_ctx.manager));
  TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.fake->start_calls);
  TEST_ASSERT_FALSE(esy_wifi_manager_is_connected(&s_ctx.manager));
}

void test_device_acquisition_errors_are_recorded_and_recoverable(void) {
  initialize(); bind_device(false);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_wifi_manager_process(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_wifi_manager_get_last_error(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_wifi_manager_stop(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&s_ctx.registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_process(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_get_last_error(&s_ctx.manager));
}

void test_start_and_process_failure_allow_cleanup_and_retry(void) {
  initialize(); s_ctx.fake->start_result = ESY_ERR_IO;
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_request_connect(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_wifi_manager_process(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_WIFI_MANAGER_STATE_FAILED, esy_wifi_manager_get_state(&s_ctx.manager));
  s_ctx.fake->start_result = ESY_OK; s_ctx.fake->process_result = ESY_ERR_TIMEOUT;
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_wifi_manager_process(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_wifi_manager_get_last_error(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_deinit(&s_ctx.manager));
  initialize(); s_ctx.fake->process_result = ESY_OK; connect();
}

void test_stop_releases_activity_and_allows_fresh_connection(void) {
  esy_wifi_ipv4_addr_t address;
  initialize(); bind_device(true); connect();
  TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_stop(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_WIFI_MANAGER_STATE_IDLE, esy_wifi_manager_get_state(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_wifi_manager_get_ipv4_addr(&s_ctx.manager, &address));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_runtime_suspend_idle(&s_ctx.registry));
  connect(); TEST_ASSERT_EQUAL_UINT32(2u, s_ctx.fake->start_calls);
}

void test_config_boundaries_and_auth_validation(void) {
  char ssid[ESY_WIFI_SSID_MAX_LENGTH + 2u];
  char password[ESY_WIFI_PASSWORD_MAX_LENGTH + 2u];
  memset(ssid, 's', sizeof(ssid)); ssid[sizeof(ssid) - 1u] = '\0';
  s_ctx.config.sta_config.ssid = ssid;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_wifi_manager_init(&s_ctx.manager, &s_ctx.config));
  ssid[ESY_WIFI_SSID_MAX_LENGTH] = '\0';
  memset(password, 'p', sizeof(password)); password[sizeof(password) - 1u] = '\0';
  s_ctx.config.sta_config.password = password;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_wifi_manager_init(&s_ctx.manager, &s_ctx.config));
  password[ESY_WIFI_PASSWORD_MAX_LENGTH] = '\0';
  initialize(); TEST_ASSERT_EQUAL(ESY_OK, esy_wifi_manager_deinit(&s_ctx.manager));
  s_ctx.config.sta_config.auth = (esy_wifi_auth_t)99;
  TEST_ASSERT_EQUAL(ESY_ERR_NOT_SUPPORTED, esy_wifi_manager_init(&s_ctx.manager, &s_ctx.config));
  s_ctx.config.sta_config.auth = ESY_WIFI_AUTH_OPEN;
  s_ctx.config.sta_config.password = ""; initialize();
}

void test_defaults_null_and_preinit_contracts(void) {
  esy_wifi_ipv4_addr_t address;
  esy_wifi_manager_default_cfg(NULL);
  TEST_ASSERT_EQUAL_UINT32(30000u, s_ctx.config.sta_config.connect_timeout_ms);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_wifi_manager_init(NULL, &s_ctx.config));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_wifi_manager_init(&s_ctx.manager, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_wifi_manager_request_connect(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_wifi_manager_request_connect(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_wifi_manager_process(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_wifi_manager_process(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_wifi_manager_stop(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_wifi_manager_stop(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_wifi_manager_deinit(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_wifi_manager_deinit(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_WIFI_MANAGER_STATE_FAILED, esy_wifi_manager_get_state(NULL));
  TEST_ASSERT_FALSE(esy_wifi_manager_is_connected(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_wifi_manager_get_ipv4_addr(NULL, &address));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_wifi_manager_get_ipv4_addr(&s_ctx.manager, NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_wifi_manager_get_last_error(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_wifi_manager_get_last_error(&s_ctx.manager));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_wifi_manager_unregister_device(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_wifi_manager_unregister_device(&s_ctx.binding));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_wifi_manager_register_device(
      NULL, &s_ctx.manager, &s_ctx.registry, &s_ctx.device_config));
}
