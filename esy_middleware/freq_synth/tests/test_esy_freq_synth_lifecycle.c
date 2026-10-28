/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_freq_synth_internal.h"

#include "esy_middleware/freq_synth/esy_adf4351_device.h"
#include "esy_middleware/freq_synth/esy_adf4372_device.h"
#include "esy_middleware/freq_synth/esy_adf4351.h"
#include "esy_middleware/freq_synth/esy_adf4372.h"
#include "esy_hal/esy_hal_gpio_pm.h"
#include "fake_esy_hal_gpio.h"
#include "fake_esy_hal_gpio_pm.h"
#include "fake_esy_hal_spi.h"
#include "unity.h"
#include <string.h>

typedef struct {
  ESY_DEVICE_REGISTRY_STATIC_STORAGE(storage, 3u);
  esy_device_registry_static_resources_t resources;
  esy_device_registry_t registry;
  esy_device_t parent;
  esy_device_t *dependencies[1];
  esy_spi_t spi;
  esy_gpio_t control;
  esy_gpio_t ce;
  esy_gpio_state_t sleep;
  esy_gpio_pm_pin_t pins[1];
  esy_device_gpio_pm_profile_t profile;
  esy_freq_synth_device_config_t registration;
  esy_freq_synth_device_binding_t binding;
  esy_freq_synth_t synth;
} lifecycle_test_ctx_t;
static lifecycle_test_ctx_t s_ctx;

static esy_err_t parent_pm(esy_device_t *device, esy_device_pm_action_t action,
                           esy_power_state_t state) {
  (void)device;
  (void)action;
  (void)state;
  return ESY_OK;
}
static const esy_device_ops_t k_parent_ops = {.pm_action = parent_pm};

static void init_chip(bool adf4372, bool enabled) {
  esy_gpio_config_t gpio;
  if (esy_hal_gpio_is_initialized(&s_ctx.control)) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&s_ctx.control));
  }
  if (esy_hal_gpio_is_initialized(&s_ctx.ce)) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_deinit(&s_ctx.ce));
  }

  esy_hal_gpio_default_cfg(&gpio);
  gpio.pin = 5u;
  gpio.mode = ESY_GPIO_MODE_OUTPUT;
  gpio.output_value = adf4372;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_init(&s_ctx.control, &gpio));
  gpio.pin = 6u;
  gpio.output_value = false;
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_init(&s_ctx.ce, &gpio));
  if (adf4372) {
    esy_adf4372_config_t cfg;
    const uint8_t revision = 0x0au;
    esy_adf4372_default_config(&cfg);
    cfg.spi = &s_ctx.spi;
    cfg.cs_gpio = &s_ctx.control;
    cfg.ce_gpio = &s_ctx.ce;
    cfg.initial_output_enabled = enabled;
    fake_esy_hal_spi_set_rx_data(&s_ctx.spi, &revision, 1u);
    TEST_ASSERT_EQUAL(ESY_OK, esy_adf4372_init(&s_ctx.synth, &cfg));
  } else {
    esy_adf4351_config_t cfg;
    esy_adf4351_default_config(&cfg);
    cfg.spi = &s_ctx.spi;
    cfg.le_gpio = &s_ctx.control;
    cfg.ce_gpio = &s_ctx.ce;
    cfg.initial_output_enabled = enabled;
    TEST_ASSERT_EQUAL(ESY_OK, esy_adf4351_init(&s_ctx.synth, &cfg));
  }
}

static esy_err_t register_chip(bool adf4372) {
  return adf4372 ? esy_adf4372_register_device(&s_ctx.binding, &s_ctx.synth,
      &s_ctx.registry, &s_ctx.registration) :
      esy_adf4351_register_device(&s_ctx.binding, &s_ctx.synth,
      &s_ctx.registry, &s_ctx.registration);
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  fake_esy_hal_gpio_reset();
  fake_esy_hal_gpio_pm_reset();
  fake_esy_hal_spi_reset();
  ESY_DEVICE_REGISTRY_BIND_STATIC_RESOURCES(&s_ctx.resources, s_ctx.storage);
  const esy_device_registry_config_t cfg = {.static_resources = &s_ctx.resources};
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_init(&s_ctx.registry, &cfg));
  const uint32_t mask = ESY_POWER_STATE_MASK(ESY_POWER_STATE_ACTIVE) |
      ESY_POWER_STATE_MASK(ESY_POWER_STATE_RETENTION);
  const esy_device_config_t parent = {
      .name = "spi", .ops = &k_parent_ops, .supported_state_mask = mask};
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_register(&s_ctx.registry, &s_ctx.parent, &parent));
  fake_esy_hal_spi_set_device(&s_ctx.spi, &s_ctx.parent);
  fake_esy_hal_spi_set_pm_write_hook(&s_ctx.spi, esy_hal_spi_write_blocking);
  s_ctx.dependencies[0] = &s_ctx.parent;
  s_ctx.sleep.mode = ESY_GPIO_MODE_OUTPUT;
  s_ctx.sleep.output_value = false;
  s_ctx.pins[0].gpio = &s_ctx.ce;
  s_ctx.pins[0].idle_state = &s_ctx.sleep;
  s_ctx.pins[0].sleep_state = &s_ctx.sleep;
  s_ctx.profile.pins = s_ctx.pins;
  s_ctx.profile.pin_count = 1u;
  s_ctx.registration.registration.name = "synth";
  s_ctx.registration.registration.dependencies = s_ctx.dependencies;
  s_ctx.registration.registration.dependency_count = 1u;
  s_ctx.registration.registration.supported_state_mask = mask;
  s_ctx.registration.gpio_pm_profile = &s_ctx.profile;
}
void tearDown(void) {}

void test_started_graph_teardown_allows_typed_unbind_and_chip_reinit(void) {
  for (unsigned chip = 0u; chip < 2u; ++chip) {
    setUp();
    init_chip(chip != 0u, false);
    TEST_ASSERT_EQUAL(ESY_OK, register_chip(chip != 0u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&s_ctx.registry));
    /* RF outputs were disabled at init; no asynchronous work is outstanding. */
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_deinit(&s_ctx.registry));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_pm_select(&s_ctx.profile,
        &s_ctx.binding, ESY_GPIO_PM_STATE_IDLE));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_freq_synth_unregister_device(&s_ctx.binding));
    TEST_ASSERT_FALSE(s_ctx.binding.registered);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.binding.device, esy_freq_synth_runtime(&s_ctx.synth)->device);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.binding, fake_esy_hal_gpio_get_pm_owner(&s_ctx.ce));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_pm_select(&s_ctx.profile,
        &s_ctx.binding, ESY_GPIO_PM_STATE_DEFAULT));
    TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_unregister_device(&s_ctx.binding));
    TEST_ASSERT_NULL(esy_freq_synth_runtime(&s_ctx.synth)->device);
    TEST_ASSERT_NULL(fake_esy_hal_gpio_get_pm_owner(&s_ctx.ce));
    /* This fixture owns its raw parent binding and retires it separately. */
    fake_esy_hal_spi_set_device(&s_ctx.spi, NULL);
    TEST_ASSERT_EQUAL(ESY_OK, chip != 0u ? esy_adf4372_deinit(&s_ctx.synth) :
        esy_adf4351_deinit(&s_ctx.synth));
    TEST_ASSERT_FALSE(esy_freq_synth_is_attached(&s_ctx.synth));
    init_chip(chip != 0u, false);
    TEST_ASSERT_TRUE(esy_freq_synth_is_attached(&s_ctx.synth));
    TEST_ASSERT_EQUAL(ESY_OK, chip != 0u ? esy_adf4372_deinit(&s_ctx.synth) :
        esy_adf4351_deinit(&s_ctx.synth));
  }
}

void test_typed_registration_rejects_wrong_backend_and_null_dependencies(void) {
  for (unsigned chip = 0u; chip < 2u; chip++) {
    setUp();
    init_chip(chip != 0u, false);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, register_chip(chip == 0u));
    s_ctx.registration.registration.dependencies = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, register_chip(chip != 0u));
    TEST_ASSERT_NULL(esy_freq_synth_runtime(&s_ctx.synth)->device);
    TEST_ASSERT_NULL(fake_esy_hal_gpio_get_pm_owner(&s_ctx.ce));
  }
}

void test_live_binding_reuse_is_rejected_without_changing_graph(void) {
  init_chip(false, false);
  TEST_ASSERT_EQUAL(ESY_OK, register_chip(false));
  const esy_freq_synth_device_binding_t before = s_ctx.binding;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, register_chip(false));
  TEST_ASSERT_EQUAL_MEMORY(&before, &s_ctx.binding, sizeof(before));
  TEST_ASSERT_EQUAL(2u, s_ctx.registry.device_count);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_adf4351_deinit(&s_ctx.synth));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_unregister_device(&s_ctx.binding));
  TEST_ASSERT_EQUAL(ESY_OK, esy_adf4351_deinit(&s_ctx.synth));
}

void test_typed_registration_rejects_a_foreign_complete_ops_table(void) {
  for (unsigned chip = 0u; chip < 2u; chip++) {
    setUp();
    init_chip(chip != 0u, false);
    /* A separate immutable table is a different provider, even when it copies
     * every function pointer. Build a fresh foreign handle; never replace the
     * operations table of the live native handle. */
    const esy_freq_synth_vtable_t foreign_ops = *s_ctx.synth.ops;
    esy_freq_synth_t foreign = {0};
    foreign.ops = &foreign_ops;
    foreign.ctx = s_ctx.synth.ctx;
    const size_t writes = fake_esy_hal_spi_get_tx_count(&s_ctx.spi);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, chip != 0u ?
        esy_adf4372_register_device(&s_ctx.binding, &foreign, &s_ctx.registry,
                                   &s_ctx.registration) :
        esy_adf4351_register_device(&s_ctx.binding, &foreign, &s_ctx.registry,
                                   &s_ctx.registration));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, chip != 0u ?
        esy_adf4372_deinit(&foreign) : esy_adf4351_deinit(&foreign));
    TEST_ASSERT_EQUAL(writes, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
    TEST_ASSERT_NULL(s_ctx.binding.synth);
    TEST_ASSERT_NULL(fake_esy_hal_gpio_get_pm_owner(&s_ctx.ce));
    esy_freq_synth_detach(&foreign);
    TEST_ASSERT_EQUAL(ESY_OK, register_chip(chip != 0u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_unregister_device(&s_ctx.binding));
    TEST_ASSERT_EQUAL(ESY_OK, chip != 0u ? esy_adf4372_deinit(&s_ctx.synth) :
        esy_adf4351_deinit(&s_ctx.synth));
  }
}

void test_registration_failure_releases_claims_and_allows_retry(void) {
  for (unsigned chip = 0u; chip < 2u; chip++) {
    setUp();
    init_chip(chip != 0u, false);
    s_ctx.registration.registration.name = NULL;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, register_chip(chip != 0u));
    TEST_ASSERT_NULL(esy_freq_synth_runtime(&s_ctx.synth)->device);
    TEST_ASSERT_NULL(s_ctx.binding.synth);
    TEST_ASSERT_NULL(fake_esy_hal_gpio_get_pm_owner(&s_ctx.ce));
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.registry.device_count);
    s_ctx.registration.registration.name = "synth";
    TEST_ASSERT_EQUAL(ESY_OK, register_chip(chip != 0u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_unregister_device(&s_ctx.binding));
    TEST_ASSERT_EQUAL(ESY_OK, chip != 0u ? esy_adf4372_deinit(&s_ctx.synth) :
        esy_adf4351_deinit(&s_ctx.synth));
  }
}

void test_failed_registration_and_failed_gpio_release_retain_owner_until_unregister_retry(void) {
  for (unsigned chip = 0u; chip < 2u; chip++) {
    setUp();
    init_chip(chip != 0u, false);
    s_ctx.registration.registration.name = NULL;
    fake_esy_hal_gpio_pm_fail_release_once(ESY_ERR_IO);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, register_chip(chip != 0u));
    TEST_ASSERT_FALSE(s_ctx.binding.registered);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.synth, s_ctx.binding.synth);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.binding.device, esy_freq_synth_runtime(&s_ctx.synth)->device);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.binding, fake_esy_hal_gpio_get_pm_owner(&s_ctx.ce));
    TEST_ASSERT_EQUAL_UINT32(1u, s_ctx.registry.device_count);
    const size_t writes = fake_esy_hal_spi_get_tx_count(&s_ctx.spi);
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_freq_synth_set_frequency_hz(&s_ctx.synth, 1000000000u));
    TEST_ASSERT_EQUAL(writes, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, chip != 0u ? esy_adf4372_deinit(&s_ctx.synth) :
        esy_adf4351_deinit(&s_ctx.synth));
    s_ctx.registration.registration.name = "synth";
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, register_chip(chip != 0u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_unregister_device(&s_ctx.binding));
    TEST_ASSERT_NULL(s_ctx.binding.synth);
    TEST_ASSERT_NULL(esy_freq_synth_runtime(&s_ctx.synth)->device);
    TEST_ASSERT_NULL(fake_esy_hal_gpio_get_pm_owner(&s_ctx.ce));
    /* PM reset cancels an unused injection without releasing the live claim. */
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_pm_claim(&s_ctx.profile, &s_ctx));
    fake_esy_hal_gpio_pm_fail_release_once(ESY_ERR_IO);
    fake_esy_hal_gpio_pm_reset();
    TEST_ASSERT_EQUAL_PTR(&s_ctx, fake_esy_hal_gpio_get_pm_owner(&s_ctx.ce));
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_pm_release(&s_ctx.profile, &s_ctx));
    TEST_ASSERT_EQUAL(ESY_OK, register_chip(chip != 0u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_unregister_device(&s_ctx.binding));
    TEST_ASSERT_EQUAL(ESY_OK, chip != 0u ? esy_adf4372_deinit(&s_ctx.synth) :
        esy_adf4351_deinit(&s_ctx.synth));
  }
}

void test_competing_gpio_claim_and_missing_spi_dependency_reject_without_ownership_change(void) {
  for (unsigned chip = 0u; chip < 2u; chip++) {
    setUp();
    init_chip(chip != 0u, false);
    s_ctx.registration.registration.dependency_count = 0u;
    TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, register_chip(chip != 0u));
    TEST_ASSERT_NULL(fake_esy_hal_gpio_get_pm_owner(&s_ctx.ce));
    s_ctx.registration.registration.dependency_count = 1u;
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_pm_claim(&s_ctx.profile, &s_ctx));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, register_chip(chip != 0u));
    TEST_ASSERT_EQUAL_PTR(&s_ctx, fake_esy_hal_gpio_get_pm_owner(&s_ctx.ce));
    TEST_ASSERT_NULL(s_ctx.binding.synth);
    TEST_ASSERT_NULL(esy_freq_synth_runtime(&s_ctx.synth)->device);
    TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_pm_release(&s_ctx.profile, &s_ctx));
    esy_freq_synth_runtime(&s_ctx.synth)->faulted = true;
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, register_chip(chip != 0u));
    TEST_ASSERT_NULL(fake_esy_hal_gpio_get_pm_owner(&s_ctx.ce));
    esy_freq_synth_runtime(&s_ctx.synth)->faulted = false;
    TEST_ASSERT_EQUAL(ESY_OK, register_chip(chip != 0u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_unregister_device(&s_ctx.binding));
    TEST_ASSERT_EQUAL(ESY_OK, chip != 0u ? esy_adf4372_deinit(&s_ctx.synth) :
        esy_adf4351_deinit(&s_ctx.synth));
  }
}

void test_unregister_retries_gpio_release_after_graph_removal(void) {
  init_chip(false, false);
  TEST_ASSERT_EQUAL(ESY_OK, register_chip(false));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_pm_select(&s_ctx.profile,
      &s_ctx.binding, ESY_GPIO_PM_STATE_IDLE));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_freq_synth_unregister_device(&s_ctx.binding));
  TEST_ASSERT_FALSE(s_ctx.binding.registered);
  TEST_ASSERT_EQUAL(1u, s_ctx.registry.device_count);
  TEST_ASSERT_EQUAL_PTR(&s_ctx.binding, fake_esy_hal_gpio_get_pm_owner(&s_ctx.ce));
  TEST_ASSERT_EQUAL(ESY_OK, esy_hal_gpio_pm_select(&s_ctx.profile,
      &s_ctx.binding, ESY_GPIO_PM_STATE_DEFAULT));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_unregister_device(&s_ctx.binding));
  TEST_ASSERT_NULL(esy_freq_synth_runtime(&s_ctx.synth)->device);
  TEST_ASSERT_NULL(fake_esy_hal_gpio_get_pm_owner(&s_ctx.ce));
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_unregister_device(&s_ctx.binding));
}

void test_resume_restores_active_ce_even_when_gpio_default_is_low(void) {
  for (unsigned chip = 0u; chip < 2u; chip++) {
    setUp();
    init_chip(chip != 0u, true);
    TEST_ASSERT_EQUAL(ESY_OK, register_chip(chip != 0u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&s_ctx.registry));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_prepare(&s_ctx.registry, ESY_POWER_STATE_RETENTION));
    TEST_ASSERT_FALSE(fake_esy_hal_gpio_get_written_value(6u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_commit_atomic(&s_ctx.registry));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_resume_atomic(&s_ctx.registry));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_resume(&s_ctx.registry));
    TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(6u));
    esy_freq_synth_info_value_t info;
    TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_get_info(&s_ctx.synth, ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED, &info));
    TEST_ASSERT_EQUAL(1u, info.u8);
  }
}

void test_adf4372_system_suspend_and_resume_preserve_aux_output_enable(void) {
  init_chip(true, true);
  TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_set_output_enable(
      &s_ctx.synth, ESY_FREQ_SYNTH_PORT_AUX, true));
  TEST_ASSERT_EQUAL(ESY_OK, register_chip(true));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&s_ctx.registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_prepare(
      &s_ctx.registry, ESY_POWER_STATE_RETENTION));
  uint8_t frame[3];
  TEST_ASSERT_EQUAL(sizeof(frame), fake_esy_hal_spi_copy_last_tx(
      &s_ctx.spi, frame, sizeof(frame)));
  TEST_ASSERT_EQUAL_HEX8(0x72u, frame[1]);
  TEST_ASSERT_EQUAL_HEX8(0u, frame[2] & 0x08u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_commit_atomic(&s_ctx.registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_resume_atomic(&s_ctx.registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_resume(&s_ctx.registry));
  TEST_ASSERT_EQUAL(sizeof(frame), fake_esy_hal_spi_copy_last_tx(
      &s_ctx.spi, frame, sizeof(frame)));
  TEST_ASSERT_EQUAL_HEX8(0x72u, frame[1]);
  TEST_ASSERT_EQUAL_HEX8(0x08u, frame[2] & 0x08u);
  TEST_ASSERT_FALSE(esy_freq_synth_runtime(&s_ctx.synth)->faulted);
}

void test_resume_gpio_failure_faults_handle_and_preserves_binding_ownership(void) {
  for (unsigned chip = 0u; chip < 2u; chip++) {
    setUp();
    init_chip(chip != 0u, true);
    TEST_ASSERT_EQUAL(ESY_OK, register_chip(chip != 0u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&s_ctx.registry));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_prepare(
        &s_ctx.registry, ESY_POWER_STATE_RETENTION));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_commit_atomic(&s_ctx.registry));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_resume_atomic(&s_ctx.registry));
    fake_esy_hal_gpio_fail_apply_after(0u, ESY_ERR_IO, false);
    TEST_ASSERT_EQUAL(ESY_ERR_IO, esy_device_registry_system_resume(&s_ctx.registry));
    TEST_ASSERT_TRUE(esy_freq_synth_runtime(&s_ctx.synth)->faulted);
    TEST_ASSERT_TRUE(esy_device_registry_is_faulted(&s_ctx.registry));
    TEST_ASSERT_EQUAL_PTR(&s_ctx.binding.device, esy_freq_synth_runtime(&s_ctx.synth)->device);
    TEST_ASSERT_EQUAL_PTR(&s_ctx.binding, fake_esy_hal_gpio_get_pm_owner(&s_ctx.ce));
    TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_freq_synth_set_frequency_hz(
        &s_ctx.synth, 1000000000u));
    TEST_ASSERT_EQUAL(ESY_ERR_BUSY, chip != 0u ? esy_adf4372_deinit(&s_ctx.synth) :
        esy_adf4351_deinit(&s_ctx.synth));
  }
}

void test_suspend_write_error_rolls_back_or_faults_graph_if_recovery_fails(void) {
  for (unsigned chip = 0u; chip < 2u; chip++) {
    for (unsigned persistent = 0u; persistent < 2u; persistent++) {
      setUp();
      init_chip(chip != 0u, true);
      TEST_ASSERT_EQUAL(ESY_OK, register_chip(chip != 0u));
      TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&s_ctx.registry));
      if (persistent) fake_esy_hal_spi_set_write_blocking_result(ESY_ERR_IO);
      else fake_esy_hal_spi_fail_write_after(0u, ESY_ERR_IO);
      TEST_ASSERT_EQUAL(persistent ? ESY_ERR_STATE : ESY_ERR_IO,
          esy_device_registry_system_prepare(&s_ctx.registry, ESY_POWER_STATE_RETENTION));
      TEST_ASSERT_EQUAL(persistent != 0u, esy_device_registry_is_faulted(&s_ctx.registry));
      TEST_ASSERT_EQUAL(persistent != 0u, esy_freq_synth_runtime(&s_ctx.synth)->faulted);
      esy_device_state_t state;
      TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&s_ctx.binding.device, &state));
      TEST_ASSERT_EQUAL(persistent ? ESY_DEVICE_STATE_FAULTED : ESY_DEVICE_STATE_ACTIVE, state);
      if (!persistent) {
        esy_freq_synth_info_value_t info;
        TEST_ASSERT_EQUAL(ESY_OK, esy_freq_synth_get_info(&s_ctx.synth, ESY_FREQ_SYNTH_INFO_OUTPUT_ENABLED, &info));
        TEST_ASSERT_EQUAL(1u, info.u8);
      }
    }
  }
}

void test_adf4372_direct_apis_resume_child_and_release_references(void) {
  init_chip(true, false);
  TEST_ASSERT_EQUAL(ESY_OK, register_chip(true));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&s_ctx.registry));
  for (unsigned fast = 0u; fast < 2u; fast++) {
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_runtime_suspend_idle(&s_ctx.registry));
    esy_adf4372_vco_calibration_t calibration = {.vco_core = 4u, .vco_bias = 5u};
    const uint8_t rx[] = {2u, 4u, 0x7au, 5u};
    fake_esy_hal_spi_set_rx_data(&s_ctx.spi, rx, sizeof(rx));
    TEST_ASSERT_EQUAL(ESY_OK, fast ? esy_adf4372_set_frequency_fast(&s_ctx.synth,
        5001000000ULL, &calibration) : esy_adf4372_read_vco_calibration(&s_ctx.synth, &calibration));
    esy_device_state_t state;
    uint32_t refs;
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_state(&s_ctx.binding.device, &state));
    TEST_ASSERT_EQUAL(ESY_DEVICE_STATE_ACTIVE, state);
    TEST_ASSERT_TRUE(fake_esy_hal_gpio_get_written_value(6u));
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&s_ctx.binding.device, &refs));
    TEST_ASSERT_EQUAL(0u, refs);
    TEST_ASSERT_EQUAL(ESY_OK, esy_device_get_usage_count(&s_ctx.parent, &refs));
    TEST_ASSERT_EQUAL(0u, refs);
  }
}

void test_adf4372_direct_apis_do_no_io_while_graph_is_prepared(void) {
  init_chip(true, false);
  TEST_ASSERT_EQUAL(ESY_OK, register_chip(true));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_start(&s_ctx.registry));
  TEST_ASSERT_EQUAL(ESY_OK, esy_device_registry_system_prepare(&s_ctx.registry, ESY_POWER_STATE_RETENTION));
  fake_esy_hal_spi_reset();
  esy_adf4372_vco_calibration_t calibration = {.vco_core = 4u, .vco_bias = 5u};
  TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_adf4372_read_vco_calibration(&s_ctx.synth, &calibration));
  TEST_ASSERT_NOT_EQUAL(ESY_OK, esy_adf4372_set_frequency_fast(&s_ctx.synth, 5001000000ULL, &calibration));
  TEST_ASSERT_EQUAL(0u, fake_esy_hal_spi_get_tx_count(&s_ctx.spi));
}

void test_adf_backends_share_readonly_tables_across_instance_lifetimes(void) {
  for (unsigned chip = 0u; chip < 2u; ++chip) {
    setUp();
    init_chip(chip != 0u, false);
    const esy_freq_synth_vtable_t *ops = s_ctx.synth.ops;
    TEST_ASSERT_NOT_NULL(ops);
    TEST_ASSERT_EQUAL(ESY_OK, chip != 0u ? esy_adf4372_deinit(&s_ctx.synth) :
        esy_adf4351_deinit(&s_ctx.synth));
    TEST_ASSERT_NULL(s_ctx.synth.ops);
    init_chip(chip != 0u, false);
    TEST_ASSERT_EQUAL_PTR(ops, s_ctx.synth.ops);
    TEST_ASSERT_EQUAL(ESY_OK, chip != 0u ? esy_adf4372_deinit(&s_ctx.synth) :
        esy_adf4351_deinit(&s_ctx.synth));
  }
}
