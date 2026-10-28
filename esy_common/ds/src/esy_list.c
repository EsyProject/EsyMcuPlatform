/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/ds/esy_list.h"

#include <stddef.h>

esy_err_t esy_list_init(esy_list_t *list) {
  if (!list) {
    return ESY_ERR_INVALID_ARG;
  }

  list->head = NULL;
  list->tail = NULL;
  list->count = 0;

  return ESY_OK;
}

void esy_list_push_back(esy_list_t *list, esy_list_node_t *node) {
  if (!list || !node) {
    return;
  }

  node->next = NULL;
  node->prev = list->tail;
  if (list->tail) {
    list->tail->next = node;
  } else {
    list->head = node;
  }
  list->tail = node;
  ++list->count;
}

void esy_list_push_front(esy_list_t *list, esy_list_node_t *node) {
  if (!list || !node) {
    return;
  }

  node->prev = NULL;
  node->next = list->head;
  if (list->head) {
    list->head->prev = node;
  } else {
    list->tail = node;
  }
  list->head = node;
  ++list->count;
}

void esy_list_remove(esy_list_t *list, esy_list_node_t *node) {
  if (!list || !node || list->count == 0u) {
    return;
  }

  if (node->prev) {
    node->prev->next = node->next;
  } else {
    list->head = node->next;
  }

  if (node->next) {
    node->next->prev = node->prev;
  } else {
    list->tail = node->prev;
  }

  node->prev = NULL;
  node->next = NULL;
  list->count--;
}

void esy_list_clear(esy_list_t *list) {
  if (!list) {
    return;
  }

  esy_list_node_t *current = list->head;
  while (current) {
    esy_list_node_t *next = current->next;

    current->prev = NULL;
    current->next = NULL;

    current = next;
  }

  list->head = NULL;
  list->tail = NULL;
  list->count = 0;
}

uint32_t esy_list_count(const esy_list_t *list) {
  if (!list) {
    return 0u;
  }

  return list->count;
}
