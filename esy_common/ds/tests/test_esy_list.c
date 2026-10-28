/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/ds/esy_list.h"

#include <stdint.h>

#include "unity.h"

void setUp(void) {}

void tearDown(void) {}

void test_esy_list_init_and_defensive_helpers_are_safe(void) {
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_list_init(NULL));

  esy_list_push_back(NULL, NULL);
  esy_list_push_front(NULL, NULL);
  esy_list_remove(NULL, NULL);
  esy_list_clear(NULL);
  TEST_ASSERT_EQUAL_UINT32(0u, esy_list_count(NULL));
}

void test_esy_list_push_remove_and_count(void) {
  esy_list_t list;
  esy_list_node_t first = {0};
  esy_list_node_t second = {0};
  esy_list_node_t third = {0};

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_list_init(&list));
  esy_list_push_back(&list, &second);
  esy_list_push_front(&list, &first);
  esy_list_push_back(&list, &third);

  TEST_ASSERT_EQUAL_PTR(&first, list.head);
  TEST_ASSERT_EQUAL_PTR(&third, list.tail);
  TEST_ASSERT_EQUAL_UINT32(3u, esy_list_count(&list));

  esy_list_remove(&list, &second);
  TEST_ASSERT_EQUAL_PTR(&first, list.head);
  TEST_ASSERT_EQUAL_PTR(&third, list.tail);
  TEST_ASSERT_EQUAL_PTR(&third, first.next);
  TEST_ASSERT_EQUAL_PTR(&first, third.prev);
  TEST_ASSERT_NULL(second.next);
  TEST_ASSERT_NULL(second.prev);
  TEST_ASSERT_EQUAL_UINT32(2u, esy_list_count(&list));

  esy_list_remove(&list, &first);
  esy_list_remove(&list, &third);
  TEST_ASSERT_NULL(list.head);
  TEST_ASSERT_NULL(list.tail);
  TEST_ASSERT_EQUAL_UINT32(0u, esy_list_count(&list));
}

void test_esy_list_clear_unlinks_nodes_and_preserves_data(void) {
  esy_list_t list;
  esy_list_node_t first = {.data = (void *)1};
  esy_list_node_t second = {.data = (void *)2};

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_list_init(&list));
  esy_list_push_back(&list, &first);
  esy_list_push_back(&list, &second);

  esy_list_clear(&list);
  TEST_ASSERT_NULL(list.head);
  TEST_ASSERT_NULL(list.tail);
  TEST_ASSERT_EQUAL_UINT32(0u, list.count);
  TEST_ASSERT_NULL(first.next);
  TEST_ASSERT_NULL(first.prev);
  TEST_ASSERT_NULL(second.next);
  TEST_ASSERT_NULL(second.prev);
  TEST_ASSERT_EQUAL_PTR((void *)1, first.data);
  TEST_ASSERT_EQUAL_PTR((void *)2, second.data);
}

void test_esy_list_clear_allows_reinserting_caller_owned_node(void) {
  esy_list_t list;
  esy_list_node_t node = {0};

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_list_init(&list));
  esy_list_push_back(&list, &node);
  esy_list_clear(&list);

  TEST_ASSERT_NULL(list.head);
  TEST_ASSERT_NULL(list.tail);
  TEST_ASSERT_EQUAL_UINT32(0u, list.count);
  TEST_ASSERT_NULL(node.next);
  TEST_ASSERT_NULL(node.prev);
  esy_list_push_front(&list, &node);
  TEST_ASSERT_EQUAL_PTR(&node, list.head);
  TEST_ASSERT_EQUAL_PTR(&node, list.tail);
  TEST_ASSERT_EQUAL_UINT32(1u, list.count);
  esy_list_clear(&list);
  TEST_ASSERT_EQUAL_UINT32(0u, list.count);
}

void test_esy_list_clear_preserves_null_data_and_is_repeatable(void) {
  esy_list_t list;
  esy_list_node_t first = {.data = (void *)1};
  esy_list_node_t middle = {0};
  esy_list_node_t last = {.data = (void *)2};

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_list_init(&list));
  esy_list_push_back(&list, &first);
  esy_list_push_back(&list, &middle);
  esy_list_push_back(&list, &last);
  esy_list_clear(&list);
  TEST_ASSERT_NULL(list.head);
  TEST_ASSERT_NULL(list.tail);
  TEST_ASSERT_EQUAL_UINT32(0u, list.count);
  TEST_ASSERT_NULL(middle.next);
  TEST_ASSERT_NULL(middle.prev);
  TEST_ASSERT_NULL(middle.data);
  TEST_ASSERT_EQUAL_PTR((void *)1, first.data);
  TEST_ASSERT_EQUAL_PTR((void *)2, last.data);

  esy_list_clear(&list);
  TEST_ASSERT_NULL(list.head);
  TEST_ASSERT_NULL(list.tail);
  TEST_ASSERT_EQUAL_UINT32(0u, list.count);
}
