/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>
#include "unity.h"
#include "esy_middleware/event_bus/esy_event_bus.h"
#include "fake_esy_os.h"
#include "fake_esy_os_queue.h"
#include "fake_esy_os_task.h"

#define TEST_SUBSCRIPTIONS 3u
#define TEST_QUEUE_DEPTH 3u
#define TEST_TOPIC_A 3u
#define TEST_TOPIC_B 31u

typedef struct {
  ESY_ALIGNED_STORAGE(data, ESY_EVENT_BUS_QUEUE_BYTES(TEST_QUEUE_DEPTH));
} event_bus_test_queue_t;

typedef struct {
  esy_event_bus_t bus;
  esy_event_bus_subscription_t slots[TEST_SUBSCRIPTIONS];
  event_bus_test_queue_t queues[TEST_SUBSCRIPTIONS];
  esy_event_bus_subscription_t *subscriptions[TEST_SUBSCRIPTIONS];
  esy_err_t hook_unsubscribe_result;
  esy_err_t hook_deinit_result;
  esy_err_t hook_receive_result;
  esy_err_t hook_publish_result;
  esy_event_bus_publish_result_t hook_report;
} event_bus_test_ctx_t;

static event_bus_test_ctx_t s_ctx;

static esy_event_bus_event_t event_for(uint32_t topic, uint8_t value) {
  esy_event_bus_event_t event;
  memset(&event, 0xa5, sizeof(event));
  event.topic = topic;
  event.size = 1u;
  event.data[0] = value;
  return event;
}

static void init_bus(size_t capacity) {
  esy_event_bus_static_resources_t resources = {
      .subscriptions = s_ctx.slots, .subscription_capacity = capacity};
  esy_event_bus_config_t config = {.static_resources = &resources};
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_init(&s_ctx.bus, &config));
}

static esy_event_bus_subscription_config_t subscription_config(size_t index,
    uint32_t mask, uint32_t depth,
    esy_event_bus_subscription_static_resources_t *resources) {
  *resources = (esy_event_bus_subscription_static_resources_t){
      .queue_buffer = s_ctx.queues[index].data.bytes,
      .queue_buffer_size = sizeof(s_ctx.queues[index].data.bytes)};
  return (esy_event_bus_subscription_config_t){.topic_mask = mask,
      .queue_depth = depth, .static_resources = resources};
}

static void subscribe_at(size_t index, uint32_t mask, uint32_t depth) {
  esy_event_bus_subscription_static_resources_t resources;
  esy_event_bus_subscription_config_t config = subscription_config(index, mask,
      depth, &resources);
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_subscribe(&s_ctx.bus, &config,
      &s_ctx.subscriptions[index]));
  TEST_ASSERT_NOT_NULL(s_ctx.subscriptions[index]);
}

static void unsubscribe_at(size_t index) {
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_unsubscribe(s_ctx.subscriptions[index]));
  s_ctx.subscriptions[index] = NULL;
}

static void expect_report(const esy_event_bus_publish_result_t *report,
    size_t matched, size_t delivered, size_t failed, esy_err_t first_error) {
  TEST_ASSERT_EQUAL_UINT64(matched, report->matched);
  TEST_ASSERT_EQUAL_UINT64(delivered, report->delivered);
  TEST_ASSERT_EQUAL_UINT64(failed, report->failed);
  TEST_ASSERT_EQUAL(first_error, report->first_error);
}

static void expect_event(size_t index, uint32_t topic, uint8_t value) {
  esy_event_bus_event_t received;
  memset(&received, 0x5a, sizeof(received));
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_receive(s_ctx.subscriptions[index],
      &received, 0u));
  TEST_ASSERT_EQUAL_UINT32(topic, received.topic);
  TEST_ASSERT_EQUAL_UINT32(1u, received.size);
  TEST_ASSERT_EQUAL_UINT8(value, received.data[0]);
  for (size_t i = 1u; i < sizeof(received.data); ++i) {
    TEST_ASSERT_EQUAL_UINT8(0u, received.data[i]);
  }
}

static void assert_outside_critical(void) {
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_critical_nesting());
}

static void close_first_subscription(void) {
  esy_event_bus_event_t received;
  assert_outside_critical();
  s_ctx.hook_unsubscribe_result = esy_event_bus_unsubscribe(s_ctx.subscriptions[0]);
  s_ctx.hook_receive_result = esy_event_bus_receive(s_ctx.subscriptions[0],
      &received, 0u);
  TEST_ASSERT_EQUAL_UINT32(0u,
      fake_esy_os_queue_get_observations()->delete_count);
}

static void close_bus_during_publication(void) {
  esy_event_bus_event_t event = event_for(TEST_TOPIC_A, 0xf0u);
  assert_outside_critical();
  s_ctx.hook_deinit_result = esy_event_bus_deinit(&s_ctx.bus);
  s_ctx.hook_publish_result = esy_event_bus_publish(&s_ctx.bus, &event,
      &s_ctx.hook_report);
}

static void nested_publication(void) {
  esy_event_bus_event_t event = event_for(TEST_TOPIC_A, 0xbbu);
  assert_outside_critical();
  s_ctx.hook_publish_result = esy_event_bus_publish_from_isr(&s_ctx.bus,
      &event, &s_ctx.hook_report, NULL);
}

static void arm_nested_publication(void) {
  assert_outside_critical();
  fake_esy_os_queue_set_send_hook(nested_publication);
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  memset(s_ctx.slots, 0xa5, sizeof(s_ctx.slots));
  fake_esy_os_reset();
  fake_esy_os_task_reset();
  fake_esy_os_queue_reset();
}

void tearDown(void) {
  fake_esy_os_queue_set_send_hook(NULL);
  fake_esy_os_queue_set_receive_hook(NULL);
  fake_esy_os_task_set_critical_exit_hook(NULL);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_critical_nesting());
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_task_get_create_static_count());
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_get_state()->malloc_call_count);
  TEST_ASSERT_FALSE(fake_esy_os_get_state()->last_isr_yield_requested);
}

void test_fanout_copies_to_independent_queues_and_filters_topics(void) {
  init_bus(TEST_SUBSCRIPTIONS);
  subscribe_at(0u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  subscribe_at(1u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A) |
      ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_B), TEST_QUEUE_DEPTH);
  subscribe_at(2u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_B), TEST_QUEUE_DEPTH);
  esy_event_bus_event_t event = event_for(TEST_TOPIC_A, 0x11u);
  esy_event_bus_publish_result_t report;
  fake_esy_os_queue_set_send_hook(assert_outside_critical);
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish(&s_ctx.bus, &event, &report));
  expect_report(&report, 2u, 2u, 0u, ESY_OK);
  memset(&event, 0xcc, sizeof(event));
  expect_event(0u, TEST_TOPIC_A, 0x11u);
  expect_event(1u, TEST_TOPIC_A, 0x11u);
  event = event_for(TEST_TOPIC_B, 0x22u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish(&s_ctx.bus, &event, &report));
  expect_report(&report, 2u, 2u, 0u, ESY_OK);
  expect_event(1u, TEST_TOPIC_B, 0x22u);
  expect_event(2u, TEST_TOPIC_B, 0x22u);
  TEST_ASSERT_EQUAL_UINT32(0u,
      fake_esy_os_queue_get_observations()->last_send_timeout_ms);
}

void test_full_first_queue_does_not_prevent_later_subscriber_delivery(void) {
  init_bus(2u);
  subscribe_at(0u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), 1u);
  subscribe_at(1u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  esy_event_bus_event_t event = event_for(TEST_TOPIC_A, 1u);
  esy_event_bus_publish_result_t report;
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish(&s_ctx.bus, &event, &report));
  event.data[0] = 2u;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_event_bus_publish(&s_ctx.bus, &event, &report));
  expect_report(&report, 2u, 1u, 1u, ESY_ERR_TIMEOUT);
  expect_event(0u, TEST_TOPIC_A, 1u);
  expect_event(1u, TEST_TOPIC_A, 1u);
  expect_event(1u, TEST_TOPIC_A, 2u);
}

void test_isr_publish_ors_wakeups_and_never_yields(void) {
  init_bus(2u);
  subscribe_at(0u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  subscribe_at(1u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  esy_event_bus_event_t event = event_for(TEST_TOPIC_A, 7u);
  esy_event_bus_publish_result_t report;
  bool need_yield = false;
  fake_esy_os_queue_set_isr_yield_on_call(1u);
  fake_esy_os_queue_set_send_hook(assert_outside_critical);
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish_from_isr(&s_ctx.bus, &event,
      &report, &need_yield));
  TEST_ASSERT_TRUE(need_yield);
  expect_report(&report, 2u, 2u, 0u, ESY_OK);
  TEST_ASSERT_EQUAL_UINT32(2u,
      fake_esy_os_queue_get_observations()->send_from_isr_count);
  need_yield = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish_from_isr(&s_ctx.bus, &event,
      &report, &need_yield));
  TEST_ASSERT_FALSE(need_yield);
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish_from_isr(&s_ctx.bus, &event,
      &report, NULL));
}

void test_isr_partial_failure_preserves_a_previous_wakeup(void) {
  init_bus(2u);
  subscribe_at(0u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  subscribe_at(1u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), 1u);
  esy_event_bus_event_t event = event_for(TEST_TOPIC_A, 9u);
  esy_event_bus_publish_result_t report;
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish(&s_ctx.bus, &event, &report));
  fake_esy_os_queue_set_isr_yield_on_call(1u);
  bool need_yield = false;
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_event_bus_publish_from_isr(&s_ctx.bus,
      &event, &report, &need_yield));
  TEST_ASSERT_TRUE(need_yield);
  expect_report(&report, 2u, 1u, 1u, ESY_ERR_TIMEOUT);
}

void test_no_matches_succeeds_without_os_sends(void) {
  init_bus(TEST_SUBSCRIPTIONS);
  esy_event_bus_event_t event = event_for(TEST_TOPIC_A, 0u);
  esy_event_bus_publish_result_t report;
  memset(&report, 0xa5, sizeof(report));
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish(&s_ctx.bus, &event, &report));
  expect_report(&report, 0u, 0u, 0u, ESY_OK);
  subscribe_at(0u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_B), TEST_QUEUE_DEPTH);
  bool need_yield = true;
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish_from_isr(&s_ctx.bus, &event,
      &report, &need_yield));
  TEST_ASSERT_FALSE(need_yield);
  expect_report(&report, 0u, 0u, 0u, ESY_OK);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_queue_get_observations()->send_count);
}

void test_queue_errors_are_reported_and_all_subscribers_are_attempted(void) {
  init_bus(2u);
  subscribe_at(0u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  subscribe_at(1u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  esy_event_bus_event_t event = event_for(TEST_TOPIC_A, 0u);
  esy_event_bus_publish_result_t report;
  fake_esy_os_queue_set_send_result(ESY_ERR_FAIL);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_event_bus_publish(&s_ctx.bus, &event, &report));
  expect_report(&report, 2u, 0u, 2u, ESY_ERR_FAIL);
  TEST_ASSERT_EQUAL_UINT32(2u, fake_esy_os_queue_get_observations()->send_count);
  fake_esy_os_queue_set_send_result(ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish(&s_ctx.bus, &event, &report));
}

void test_inflight_send_pins_subscription_until_unsubscribe_retry(void) {
  init_bus(2u);
  subscribe_at(0u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  subscribe_at(1u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  esy_event_bus_event_t event = event_for(TEST_TOPIC_A, 0x33u);
  esy_event_bus_publish_result_t report;
  fake_esy_os_queue_set_send_hook(close_first_subscription);
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish(&s_ctx.bus, &event, &report));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.hook_unsubscribe_result);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, s_ctx.hook_receive_result);
  expect_report(&report, 2u, 2u, 0u, ESY_OK);
  unsubscribe_at(0u);
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_os_queue_get_observations()->delete_count);
  event.data[0] = 0x44u;
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish(&s_ctx.bus, &event, &report));
  expect_report(&report, 1u, 1u, 0u, ESY_OK);
  expect_event(1u, TEST_TOPIC_A, 0x33u);
  expect_event(1u, TEST_TOPIC_A, 0x44u);
}

void test_inflight_receive_pins_queue_and_can_finish_after_closure(void) {
  init_bus(1u);
  subscribe_at(0u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  esy_event_bus_event_t event = event_for(TEST_TOPIC_A, 0x66u);
  esy_event_bus_publish_result_t report;
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish(&s_ctx.bus, &event, &report));
  fake_esy_os_queue_set_receive_hook(close_first_subscription);
  esy_event_bus_event_t received;
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_receive(s_ctx.subscriptions[0],
      &received, 17u));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.hook_unsubscribe_result);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, s_ctx.hook_receive_result);
  TEST_ASSERT_EQUAL_UINT8(0x66u, received.data[0]);
  TEST_ASSERT_EQUAL_UINT32(17u,
      fake_esy_os_queue_get_observations()->last_receive_timeout_ms);
  unsubscribe_at(0u);
  TEST_ASSERT_EQUAL_UINT32(1u, fake_esy_os_queue_get_observations()->delete_count);
}

void test_closing_bus_rejects_new_publications_but_pins_admitted_one(void) {
  init_bus(2u);
  subscribe_at(0u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  subscribe_at(1u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  esy_event_bus_event_t event = event_for(TEST_TOPIC_A, 0x55u);
  esy_event_bus_publish_result_t report;
  fake_esy_os_queue_set_send_hook(close_bus_during_publication);
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish(&s_ctx.bus, &event, &report));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.hook_deinit_result);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, s_ctx.hook_publish_result);
  expect_report(&s_ctx.hook_report, 0u, 0u, 0u, ESY_OK);
  expect_report(&report, 2u, 2u, 0u, ESY_OK);
  expect_event(0u, TEST_TOPIC_A, 0x55u);
  expect_event(1u, TEST_TOPIC_A, 0x55u);
  unsubscribe_at(0u);
  unsubscribe_at(1u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_deinit(&s_ctx.bus));
}

void test_admitted_publisher_alone_prevents_bus_teardown(void) {
  init_bus(1u);
  esy_event_bus_event_t event = event_for(TEST_TOPIC_A, 0u);
  esy_event_bus_publish_result_t report;
  fake_esy_os_task_set_critical_exit_hook(close_bus_during_publication);
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish(&s_ctx.bus, &event, &report));
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, s_ctx.hook_deinit_result);
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, s_ctx.hook_publish_result);
  expect_report(&report, 0u, 0u, 0u, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_deinit(&s_ctx.bus));
}

void test_nested_task_and_isr_publishers_keep_each_queue_fifo(void) {
  init_bus(2u);
  subscribe_at(0u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  subscribe_at(1u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  esy_event_bus_event_t event = event_for(TEST_TOPIC_A, 0xaau);
  esy_event_bus_publish_result_t report;
  fake_esy_os_queue_set_send_hook(arm_nested_publication);
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish(&s_ctx.bus, &event, &report));
  TEST_ASSERT_EQUAL(ESY_OK, s_ctx.hook_publish_result);
  expect_report(&report, 2u, 2u, 0u, ESY_OK);
  expect_report(&s_ctx.hook_report, 2u, 2u, 0u, ESY_OK);
  expect_event(0u, TEST_TOPIC_A, 0xaau);
  expect_event(0u, TEST_TOPIC_A, 0xbbu);
  expect_event(1u, TEST_TOPIC_A, 0xbbu);
  expect_event(1u, TEST_TOPIC_A, 0xaau);
}

void test_deinit_closes_admission_until_subscriptions_are_removed(void) {
  init_bus(1u);
  subscribe_at(0u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  TEST_ASSERT_EQUAL(ESY_ERR_BUSY, esy_event_bus_deinit(&s_ctx.bus));
  esy_event_bus_event_t event = event_for(TEST_TOPIC_A, 0u);
  esy_event_bus_publish_result_t report;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_event_bus_publish(&s_ctx.bus, &event, &report));
  esy_event_bus_subscription_static_resources_t resources;
  esy_event_bus_subscription_config_t config = subscription_config(1u,
      ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), 1u, &resources);
  esy_event_bus_subscription_t *subscription = &s_ctx.slots[0];
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_event_bus_subscribe(&s_ctx.bus, &config,
      &subscription));
  TEST_ASSERT_NULL(subscription);
  unsubscribe_at(0u);
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_deinit(&s_ctx.bus));
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_deinit(&s_ctx.bus));
  init_bus(1u);
  subscribe_at(0u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish(&s_ctx.bus, &event, &report));
  expect_event(0u, TEST_TOPIC_A, 0u);
}

void test_subscribe_failure_and_exhausted_slots_can_be_retried(void) {
  init_bus(1u);
  esy_event_bus_subscription_static_resources_t resources;
  esy_event_bus_subscription_config_t config = subscription_config(0u,
      ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH, &resources);
  esy_event_bus_subscription_t *subscription = &s_ctx.slots[0];
  fake_esy_os_queue_set_create_result(ESY_ERR_FAIL);
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_event_bus_subscribe(&s_ctx.bus, &config,
      &subscription));
  TEST_ASSERT_NULL(subscription);
  fake_esy_os_queue_set_create_result(ESY_OK);
  subscribe_at(0u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  config = subscription_config(1u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A),
      TEST_QUEUE_DEPTH, &resources);
  TEST_ASSERT_EQUAL(ESY_ERR_NO_MEM, esy_event_bus_subscribe(&s_ctx.bus, &config,
      &subscription));
  TEST_ASSERT_NULL(subscription);
  unsubscribe_at(0u);
  subscribe_at(1u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_B), TEST_QUEUE_DEPTH);
  esy_event_bus_event_t event = event_for(TEST_TOPIC_B, 0x77u);
  esy_event_bus_publish_result_t report;
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish(&s_ctx.bus, &event, &report));
  expect_event(1u, TEST_TOPIC_B, 0x77u);
}

void test_init_accepts_arbitrary_backing_slots_and_rejects_repeated_or_copied_handle(void) {
  init_bus(TEST_SUBSCRIPTIONS);
  esy_event_bus_static_resources_t resources = {
      .subscriptions = s_ctx.slots, .subscription_capacity = TEST_SUBSCRIPTIONS};
  esy_event_bus_config_t config = {.static_resources = &resources};
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_event_bus_init(&s_ctx.bus, &config));
  esy_event_bus_t copy = s_ctx.bus;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_event_bus_init(&copy, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_event_bus_deinit(&copy));
  esy_event_bus_event_t event = event_for(TEST_TOPIC_A, 0u);
  esy_event_bus_publish_result_t report;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_event_bus_publish(&copy, &event, &report));
  subscribe_at(0u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish(&s_ctx.bus, &event, &report));
}

void test_invalid_bus_resources_fail_without_retaining_state(void) {
  esy_event_bus_static_resources_t resources = {
      .subscriptions = s_ctx.slots, .subscription_capacity = TEST_SUBSCRIPTIONS};
  esy_event_bus_config_t config = {.static_resources = &resources};
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_init(NULL, &config));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_init(&s_ctx.bus, NULL));
  config.static_resources = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_init(&s_ctx.bus, &config));
  config.static_resources = &resources;
  resources.subscriptions = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_init(&s_ctx.bus, &config));
  resources.subscriptions = s_ctx.slots;
  resources.subscription_capacity = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_init(&s_ctx.bus, &config));
  resources.subscription_capacity = SIZE_MAX / sizeof(s_ctx.slots[0]) + 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_init(&s_ctx.bus, &config));
  resources.subscription_capacity = TEST_SUBSCRIPTIONS;
  resources.subscriptions = (esy_event_bus_subscription_t *)
      (void *)((uint8_t *)s_ctx.slots + 1u);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_init(&s_ctx.bus, &config));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_queue_get_observations()->create_count);
  init_bus(TEST_SUBSCRIPTIONS);
}

void test_invalid_subscription_configurations_do_not_consume_a_slot(void) {
  init_bus(1u);
  esy_event_bus_subscription_static_resources_t resources;
  esy_event_bus_subscription_config_t config = subscription_config(0u,
      ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH, &resources);
  esy_event_bus_subscription_t *subscription = &s_ctx.slots[0];
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_subscribe(NULL, &config,
      &subscription));
  TEST_ASSERT_NULL(subscription);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_subscribe(&s_ctx.bus, NULL,
      &subscription));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_subscribe(&s_ctx.bus, &config,
      NULL));
  config.topic_mask = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_subscribe(&s_ctx.bus, &config,
      &subscription));
  config.topic_mask = ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A);
  config.queue_depth = 0u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_subscribe(&s_ctx.bus, &config,
      &subscription));
  config.queue_depth = TEST_QUEUE_DEPTH;
  config.static_resources = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_subscribe(&s_ctx.bus, &config,
      &subscription));
  config.static_resources = &resources;
  resources.queue_buffer = NULL;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_subscribe(&s_ctx.bus, &config,
      &subscription));
  resources.queue_buffer = s_ctx.queues[0].data.bytes;
  resources.queue_buffer_size = ESY_EVENT_BUS_QUEUE_BYTES(TEST_QUEUE_DEPTH) - 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_subscribe(&s_ctx.bus, &config,
      &subscription));
  resources.queue_buffer_size = sizeof(s_ctx.queues[0].data.bytes);
  resources.queue_buffer = s_ctx.queues[0].data.bytes + 1u;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_subscribe(&s_ctx.bus, &config,
      &subscription));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_queue_get_observations()->create_count);
  subscribe_at(0u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
}

void test_invalid_publications_reset_reports_and_isr_wakeup(void) {
  init_bus(1u);
  esy_event_bus_event_t event = event_for(TEST_TOPIC_A, 0u);
  esy_event_bus_publish_result_t report;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_publish(NULL, &event, &report));
  expect_report(&report, 0u, 0u, 0u, ESY_OK);
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_publish(&s_ctx.bus, NULL,
      &report));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_publish(&s_ctx.bus, &event,
      NULL));
  event.topic = ESY_EVENT_BUS_TOPIC_COUNT;
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_publish(&s_ctx.bus, &event,
      &report));
  event.topic = TEST_TOPIC_A;
  event.size = ESY_EVENT_BUS_PAYLOAD_BYTES + 1u;
  bool need_yield = true;
  memset(&report, 0xa5, sizeof(report));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_publish_from_isr(&s_ctx.bus,
      &event, &report, &need_yield));
  TEST_ASSERT_FALSE(need_yield);
  expect_report(&report, 0u, 0u, 0u, ESY_OK);
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_queue_get_observations()->send_count);
}

void test_receive_preserves_output_on_failure_and_passes_finite_wait_to_os(void) {
  init_bus(1u);
  subscribe_at(0u, ESY_EVENT_BUS_TOPIC_BIT(TEST_TOPIC_A), TEST_QUEUE_DEPTH);
  esy_event_bus_event_t received;
  memset(&received, 0x5a, sizeof(received));
  esy_event_bus_event_t before = received;
  fake_esy_os_queue_set_receive_hook(assert_outside_critical);
  TEST_ASSERT_EQUAL(ESY_ERR_TIMEOUT, esy_event_bus_receive(s_ctx.subscriptions[0],
      &received, 23u));
  TEST_ASSERT_EQUAL_MEMORY(&before, &received, sizeof(received));
  TEST_ASSERT_EQUAL_UINT32(23u,
      fake_esy_os_queue_get_observations()->last_receive_timeout_ms);
  fake_esy_os_queue_set_receive_result(ESY_ERR_FAIL);
  TEST_ASSERT_EQUAL(ESY_ERR_FAIL, esy_event_bus_receive(s_ctx.subscriptions[0],
      &received, 0u));
  TEST_ASSERT_EQUAL_MEMORY(&before, &received, sizeof(received));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_receive(s_ctx.subscriptions[0],
      &received, UINT32_MAX));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_receive(NULL, &received, 0u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_receive(s_ctx.subscriptions[0],
      NULL, 0u));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_unsubscribe(NULL));
  TEST_ASSERT_EQUAL(ESY_ERR_INVALID_ARG, esy_event_bus_deinit(NULL));
}

void test_zero_and_maximum_payload_lengths_are_copied_without_truncation(void) {
  init_bus(1u);
  subscribe_at(0u, ESY_EVENT_BUS_TOPIC_BIT(0u), TEST_QUEUE_DEPTH);
  esy_event_bus_event_t event = event_for(0u, 0u);
  event.size = 0u;
  esy_event_bus_publish_result_t report;
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish(&s_ctx.bus, &event, &report));
  esy_event_bus_event_t received;
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_receive(s_ctx.subscriptions[0], &received,
      0u));
  TEST_ASSERT_EQUAL_UINT32(0u, received.size);
  for (size_t i = 0u; i < sizeof(received.data); ++i) {
    TEST_ASSERT_EQUAL_UINT8(0u, received.data[i]);
  }
  event.size = sizeof(event.data);
  for (size_t i = 0u; i < sizeof(event.data); ++i) event.data[i] = (uint8_t)i;
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_publish(&s_ctx.bus, &event, &report));
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_receive(s_ctx.subscriptions[0], &received,
      0u));
  TEST_ASSERT_EQUAL_MEMORY(&event, &received, sizeof(event));
}

void test_uninitialized_bus_rejects_operations_without_creating_resources(void) {
  esy_event_bus_event_t event = event_for(0u, 0u);
  esy_event_bus_publish_result_t report;
  bool need_yield = true;
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_event_bus_publish_from_isr(&s_ctx.bus,
      &event, &report, &need_yield));
  TEST_ASSERT_FALSE(need_yield);
  expect_report(&report, 0u, 0u, 0u, ESY_OK);
  esy_event_bus_subscription_static_resources_t resources;
  esy_event_bus_subscription_config_t config = subscription_config(0u,
      ESY_EVENT_BUS_TOPIC_BIT(0u), TEST_QUEUE_DEPTH, &resources);
  esy_event_bus_subscription_t *subscription = &s_ctx.slots[0];
  TEST_ASSERT_EQUAL(ESY_ERR_STATE, esy_event_bus_subscribe(&s_ctx.bus, &config,
      &subscription));
  TEST_ASSERT_NULL(subscription);
  TEST_ASSERT_EQUAL(ESY_OK, esy_event_bus_deinit(&s_ctx.bus));
  TEST_ASSERT_EQUAL_UINT32(0u, fake_esy_os_queue_get_observations()->create_count);
}
