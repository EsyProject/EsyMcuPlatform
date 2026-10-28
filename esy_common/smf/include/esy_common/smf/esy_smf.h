/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_SMF_H
#define ESY_SMF_H

#include <stdbool.h>
#include <stdint.h>

#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_smf.h
 * @brief Caller-driven flat and hierarchical state machines without allocation.
 *
 * One serialized task or baremetal owner controls each machine. Calls and
 * callbacks must not overlap across execution contexts; this API is not ISR
 * safe and provides no synchronization. Callbacks execute synchronously in the
 * caller's context. Mutating calls from a callback return ESY_ERR_BUSY; request
 * a transition through the run decision instead. Queries are allowed there.
 *
 * State tables remain immutable, and tables, callback code and mutable user
 * data remain valid until successful deinit. Event data is borrowed for one run. The
 * core owns no timer, event queue, task, hardware or power-management resource.
 */

/** @brief State-table index; valid IDs are smaller than state_count. */
typedef uint32_t esy_smf_state_id_t;

/** @brief No parent, no initial child, or no currently entered state. */
#define ESY_SMF_STATE_NONE UINT32_MAX

/** @brief Outcome selected by a run callback. */
typedef enum {
  ESY_SMF_HANDLED = 0,    /**< Consume the event without a transition. */
  ESY_SMF_PROPAGATE = 1,  /**< Offer the same event to the parent. */
  ESY_SMF_TRANSITION = 2  /**< Perform one external transition and end this run. */
} esy_smf_action_t;

/** @brief Run output, initialized by the core to HANDLED and STATE_NONE. */
typedef struct {
  esy_smf_action_t action;     /**< Event handling or transition action. */
  esy_smf_state_id_t target;   /**< Required only for TRANSITION. */
} esy_smf_decision_t;

/**
 * @brief Enter or exit a state in the owner's calling context.
 * @param[in,out] user Borrowed config user pointer, which may be NULL.
 * @return ESY_OK on success; any other result faults the machine.
 * @note Failure must be atomic: failed entry releases its partial resources;
 * failed exit retains the entered state's resources so stop can retry it.
 * Successful exit releases that state's resources exactly once. The callback
 * must have a bounded execution time appropriate to the owner's context.
 */
typedef esy_err_t (*esy_smf_state_fn_t)(void *user);

/**
 * @brief Process one event at the current leaf or one of its ancestors.
 * @param[in,out] user Borrowed config user pointer, which may be NULL.
 * @param[in] event Opaque borrowed event; NULL is valid for polling.
 * @param[out] decision Initialized output; target is ignored unless TRANSITION.
 * @return ESY_OK on success; any other result faults the machine, retaining
 * the currently entered state chain for stop. No transition is then executed.
 * @note A missing run callback propagates automatically. A root propagation
 * ends successfully without a transition. A transition is sourced from the
 * callback's state, including when that callback belongs to a parent.
 */
typedef esy_err_t (*esy_smf_run_fn_t)(void *user, const void *event,
                                     esy_smf_decision_t *decision);

/** @brief Immutable state description; use STATE_NONE explicitly for no link. */
typedef struct {
  esy_smf_state_id_t parent;   /**< Parent index or STATE_NONE for a root. */
  esy_smf_state_id_t initial;  /**< Direct initial child index or STATE_NONE. */
  esy_smf_state_fn_t entry;    /**< Optional entry action. */
  esy_smf_run_fn_t run;        /**< Optional event handler. */
  esy_smf_state_fn_t exit;     /**< Optional exit action. */
} esy_smf_state_t;

/** @brief Borrowed state table and user data, copied by init. */
typedef struct {
  const esy_smf_state_t *states; /**< Nonempty caller-owned immutable table. */
  uint32_t state_count;         /**< Table length, from 1 through UINT32_MAX-1. */
  void *user;                   /**< Optional caller-owned callback context. */
} esy_smf_config_t;

/** @brief Observable lifecycle status. */
typedef enum {
  ESY_SMF_UNINITIALIZED = 0, /**< Zero-initialized or successfully deinitialized. */
  ESY_SMF_READY = 1,         /**< Initialized, with no entered states. */
  ESY_SMF_RUNNING = 2,       /**< Started; run and transition are allowed. */
  ESY_SMF_FAULTED = 3        /**< Only stop, queries and eventual deinit allowed. */
} esy_smf_status_t;

/**
 * @brief Caller-owned machine; initialize with {0} before first init.
 *
 * Fields are public only to permit static allocation and must not be modified
 * by users after init. Never clear a live or faulted machine to bypass stop.
 * Every translation unit must use the same concrete context layout. Rebuild
 * all consumers after changing that layout.
 */
typedef struct {
  esy_smf_config_t config;      /**< Borrowed table and user context. */
  esy_smf_state_id_t current;   /**< Deepest successfully entered state. */
  esy_smf_status_t status;      /**< Lifecycle status. */
  bool busy;                    /**< Reentrant mutation guard, not a lock. */
} esy_smf_t;

/**
 * @brief Validate and initialize a zero-initialized or deinitialized machine.
 * @param[in,out] machine Caller-owned storage initialized with {0}.
 * @param[in] config Nonempty table with acyclic, in-range parent links and
 * initial links to direct children. A composite without an initial child can
 * itself be the active state. Multiple independent roots are supported.
 * @retval ESY_OK Initialized without executing callbacks.
 * @retval ESY_ERR_INVALID_ARG Invalid pointers, size, links or a parent cycle.
 * @retval ESY_ERR_STATE Already initialized; ownership remains unchanged.
 * @retval ESY_ERR_BUSY Called reentrantly from a callback.
 * @note Failed init leaves the object unchanged. Validation takes O(N*D) time
 * for N states and maximum depth D, with O(1) auxiliary storage.
 */
esy_err_t esy_smf_init(esy_smf_t *machine, const esy_smf_config_t *config);

/**
 * @brief Enter an initial state's ancestors and its default-child chain.
 * @param[in,out] machine Initialized READY machine.
 * @param[in] initial Valid state-table index.
 * @retval ESY_OK Started, with outermost-first entry callbacks.
 * @retval ESY_ERR_INVALID_ARG NULL machine or invalid initial index.
 * @retval ESY_ERR_STATE Machine is not READY.
 * @retval ESY_ERR_BUSY Reentrant mutation.
 * @return Entry callback error on failure, with status FAULTED. Previously
 * entered ancestors remain owned and must be unwound by stop.
 */
esy_err_t esy_smf_start(esy_smf_t *machine, esy_smf_state_id_t initial);

/**
 * @brief Process one event, leaf-first, with at most one transition.
 * @param[in,out] machine RUNNING machine controlled by its serialized owner.
 * @param[in] event Optional borrowed event, valid until this call returns.
 * @retval ESY_OK Event handled or propagated beyond the root.
 * @retval ESY_ERR_INVALID_ARG NULL machine or invalid callback decision/target.
 * @retval ESY_ERR_STATE Machine is not RUNNING.
 * @retval ESY_ERR_BUSY Reentrant mutation.
 * @return Callback error unchanged. Callback errors and malformed decisions
 * fault the machine. Use stop for cleanup before restarting it.
 * @note At most D run, D exit and D entry callbacks execute. Framework work is
 * O(D*D), with no recursion or allocation; callback execution time is additional.
 * There is no clock or required polling rate; the owner supplies events and any
 * application-specific timing policy. Event pointers are never retained.
 */
esy_err_t esy_smf_run(esy_smf_t *machine, const void *event);

/**
 * @brief Perform an owner-requested external transition from the active state.
 * @param[in,out] machine RUNNING machine.
 * @param[in] target Valid state-table index; initial children are followed.
 * @retval ESY_OK Transition completed.
 * @retval ESY_ERR_INVALID_ARG NULL machine or invalid target; no state changes.
 * @retval ESY_ERR_STATE Machine is not RUNNING.
 * @retval ESY_ERR_BUSY Reentrant mutation, including calls from callbacks.
 * @return Entry/exit callback error unchanged, with status FAULTED.
 * @note Exit is innermost-first and entry is outermost-first. Siblings retain
 * their common ancestors. Self transitions and transitions between an ancestor
 * and its descendant exit and reenter that ancestor. For a run decision, this
 * rule uses the handler state as the source. Initial descent executes no run
 * actions. Work and callback bounds are the same as run, without run callbacks.
 */
esy_err_t esy_smf_transition(esy_smf_t *machine, esy_smf_state_id_t target);

/**
 * @brief Exit all entered states, including after a callback failure.
 * @param[in,out] machine Initialized machine; READY is an idempotent success.
 * @retval ESY_OK All exits completed; machine is READY and may be started again.
 * @retval ESY_ERR_INVALID_ARG NULL machine.
 * @retval ESY_ERR_STATE Uninitialized machine.
 * @retval ESY_ERR_BUSY Reentrant mutation.
 * @return Exit error unchanged. The machine remains FAULTED at the failed exit;
 * retry stop after correcting the cause. Successful exits are not repeated.
 * @note Stop runs at most D exit callbacks. It does not deinitialize user data.
 */
esy_err_t esy_smf_stop(esy_smf_t *machine);

/**
 * @brief Release borrowed references after successful stop, executing no action.
 * @param[in,out] machine READY machine; success restores the {0} state.
 * @retval ESY_OK Deinitialized; storage may be reused or released.
 * @retval ESY_ERR_INVALID_ARG NULL machine.
 * @retval ESY_ERR_STATE Not READY; ownership remains unchanged.
 * @retval ESY_ERR_BUSY Reentrant mutation.
 */
esy_err_t esy_smf_deinit(esy_smf_t *machine);

/**
 * @brief Query the deepest successfully entered state, including while faulted.
 * @param[in] machine Initialized machine, including inside a callback.
 * @param[out] state Current index, or STATE_NONE when no states are entered.
 * @retval ESY_OK Output written.
 * @retval ESY_ERR_INVALID_ARG NULL pointer; output unchanged.
 * @retval ESY_ERR_STATE Uninitialized machine; output unchanged.
 * @note During entry, reports the previously entered ancestor; during exit,
 * reports the state being exited. Serialized owner only, not a concurrent query.
 */
esy_err_t esy_smf_get_state(const esy_smf_t *machine, esy_smf_state_id_t *state);

/**
 * @brief Query status, including UNINITIALIZED on zero-initialized storage.
 * @param[in] machine Zero-initialized or initialized machine.
 * @param[out] status Current lifecycle status.
 * @retval ESY_OK Output written.
 * @retval ESY_ERR_INVALID_ARG NULL pointer; output unchanged.
 * @note Serialized owner only; callback queries are allowed.
 */
esy_err_t esy_smf_get_status(const esy_smf_t *machine, esy_smf_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* ESY_SMF_H */
