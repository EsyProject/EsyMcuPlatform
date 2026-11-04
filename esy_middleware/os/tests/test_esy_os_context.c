/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "esy_middleware/os/esy_os_context.h"
#include "native_test_support.h"

void setUp(void) { native_test_support_reset(); }
void tearDown(void) {}

int xTaskGetSchedulerState(void) {
  return native_test_support_get()->scheduler_state;
}

void test_blocking_control_requires_a_running_scheduler(void) {
  TEST_ASSERT_FALSE(esy_os_context_can_block());
  native_test_support_get()->scheduler_state = taskSCHEDULER_RUNNING;
  TEST_ASSERT_TRUE(esy_os_context_can_block());
  native_test_support_get()->scheduler_state = taskSCHEDULER_SUSPENDED;
  TEST_ASSERT_FALSE(esy_os_context_can_block());
}

void test_blocking_control_rejects_isr_and_masked_interrupts(void) {
  native_test_support_get()->scheduler_state = taskSCHEDULER_RUNNING;
  native_test_support_get()->isr = true;
  TEST_ASSERT_FALSE(esy_os_context_can_block());
  native_test_support_get()->isr = false;
  TEST_ASSERT_TRUE(esy_os_context_can_block());
  uint32_t token = esy_os_context_irq_save();
  TEST_ASSERT_FALSE(esy_os_context_can_block());
  esy_os_context_irq_restore(token);
  TEST_ASSERT_TRUE(esy_os_context_can_block());
}

void test_context_uses_native_isr_and_configured_core_count(void) {
  TEST_ASSERT_FALSE(esy_os_context_is_in_isr());
  native_test_support_get()->isr = true;
  TEST_ASSERT_TRUE(esy_os_context_is_in_isr());
  TEST_ASSERT_EQUAL_UINT8(2u, esy_os_context_core_count());
}

void test_nested_local_irq_tokens_restore_the_previous_level(void) {
  uint32_t outer = esy_os_context_irq_save();
  uint32_t inner = esy_os_context_irq_save();
  TEST_ASSERT_EQUAL_UINT32(0u, outer);
  TEST_ASSERT_EQUAL_UINT32(7u, inner);
  esy_os_context_irq_restore(inner);
  TEST_ASSERT_EQUAL_UINT32(7u, native_test_support_get()->local_irq_mask);
  esy_os_context_irq_restore(outer);
  TEST_ASSERT_EQUAL_UINT32(0u, native_test_support_get()->local_irq_mask);
  esy_os_context_irq_disable();
  TEST_ASSERT_EQUAL_UINT32(7u, native_test_support_get()->local_irq_mask);
  esy_os_context_irq_enable();
  TEST_ASSERT_EQUAL_UINT32(0u, native_test_support_get()->local_irq_mask);
}
