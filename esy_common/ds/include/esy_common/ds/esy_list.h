/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_list.h
 * @brief Caller-owned doubly linked list.
 *
 * ## Overview
 * This module provides a lightweight doubly linked list with caller-owned node
 * storage and basic operations:
 * - initialization
 * - push front / push back
 * - remove node
 * - clear list
 * - count nodes
 *
 * ## Ownership model
 * - Users provide and retain ownership of @ref esy_list_node_t storage.
 * - The list does not allocate or free node memory.
 * - The optional `data` field may point to a user object or be left NULL.
 * - The optional `data` object is always owned by the caller.
 * - esy_list_clear() only unlinks nodes; it preserves each `data` pointer.
 *   The caller handles any object/resource cleanup outside the container.
 *
 * ## Contract model
 * - NULL list/node parameters are handled defensively and return a documented
 *   error or no-op sentinel.
 * - Node membership is a caller contract. Removing a node that is not currently
 *   linked into the specified list is undefined because the list does not scan
 *   for membership in order to keep remove O(1).
 *
 * ## Concurrency
 * - This container is NOT thread-safe.
 * - If used in multi-thread or ISR contexts, external synchronization
 *   (mutex / critical section) is required.
 */

#ifndef ESY_COMMON_DS_ESY_LIST_H
#define ESY_COMMON_DS_ESY_LIST_H

#include <stdint.h>
#include <stdbool.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Caller-owned list node.
 *
 * The node can either carry a user pointer in @ref data or be embedded inside
 * a larger user object.
 *
 * Typical usage:
 * @code
 * typedef struct {
 *   int value;
 *   esy_list_node_t node;
 * } my_object_t;
 * @endcode
 */
typedef struct esy_list_node {
  struct esy_list_node *next; /**< Next node in list. */
  struct esy_list_node *prev; /**< Previous node in list. */
  void *data;                 /**< Optional user data pointer; may be NULL. */
} esy_list_node_t;

/**
 * @brief List control structure.
 *
 * Holds head/tail pointers and a cached node count.
 */
typedef struct {
  esy_list_node_t *head; /**< Head node, or NULL when the list is empty. */
  esy_list_node_t *tail; /**< Tail node, or NULL when the list is empty. */
  uint32_t count;        /**< Number of nodes in the list. */
} esy_list_t;

/**
 * @brief Initialize a linked list.
 *
 * Sets the list to an empty state and resets the node count.
 * List storage may contain arbitrary initial bytes; init reads no old fields.
 * Repeated init discards head/tail/count without unlinking former nodes or
 * releasing their data. Stop all users and finish all node/data borrows first;
 * clear/remove linked nodes before reinit if those nodes will be reused.
 * A NULL argument returns INVALID_ARG without writing storage. Only a
 * successfully initialized list may be passed to the other list operations.
 *
 * @param[out] list List object (must not be NULL).
 * @retval ESY_OK               Success.
 * @retval ESY_ERR_INVALID_ARG  Invalid argument (list is NULL).
 */
extern esy_err_t esy_list_init(esy_list_t *list);

/**
 * @brief Insert a node at the tail of the list (O(1)).
 *
 * The caller must ensure that @p node is not currently part of any list
 * (its `next`/`prev` must not be in use).
 *
 * @param[in,out] list List object (must not be NULL).
 * @param[in]     node Node to insert (must not be NULL).
 */
extern void esy_list_push_back(esy_list_t *list, esy_list_node_t *node);

/**
 * @brief Insert a node at the head of the list (O(1)).
 *
 * The caller must ensure that @p node is not currently part of any list.
 *
 * @param[in,out] list List object (must not be NULL).
 * @param[in]     node Node to insert (must not be NULL).
 */
extern void esy_list_push_front(esy_list_t *list, esy_list_node_t *node);

/**
 * @brief Remove a specific node from the list (O(1)).
 *
 * If @p node is not part of @p list, behavior is undefined.
 * Head/tail pointers are updated correctly when removing boundary nodes.
 *
 * @param[in,out] list List object (must not be NULL).
 * @param[in]     node Node to remove (must not be NULL).
 */
extern void esy_list_remove(esy_list_t *list, esy_list_node_t *node);

/**
 * @brief Unlink all nodes without releasing caller-owned storage or resources.
 *
 * Resets every node's next/prev links and empties the list. Each node's data
 * pointer is unchanged. Nodes may be inserted again after this call.
 * Requires a successfully initialized list with live node storage. Serialize
 * with every node/list user and finish node/data borrows before clearing.
 * This unlinks nodes but performs no callback or external-resource cleanup.
 *
 * @note
 * - The node memory itself is NOT allocated or freed by this module.
 * - After this call, the list is empty.
 *
 * @param[in,out] list List object (NULL is a no-op).
 */
extern void esy_list_clear(esy_list_t *list);

/**
 * @brief Get the number of nodes in the list.
 *
 * @param[in] list List object (must not be NULL).
 * @return Number of nodes (0 if list is empty).
 */
extern uint32_t esy_list_count(const esy_list_t *list);

#ifdef __cplusplus
}
#endif

#endif /* ESY_COMMON_DS_ESY_LIST_H */
