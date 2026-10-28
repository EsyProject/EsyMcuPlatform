/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_common/smf/esy_smf.h"

#include <string.h>

#include "unity.h"

#define TEST_TRACE_CAPACITY 64u

enum {
  ROOT,
  BRANCH,
  LEAF,
  SIBLING,
  OTHER_ROOT,
  OTHER_LEAF,
  TEST_STATE_COUNT
};

enum { ENTRY, RUN, EXIT, TEST_PHASE_COUNT };

typedef struct {
  esy_smf_t machine;
  esy_smf_state_t states[TEST_STATE_COUNT];
  esy_smf_config_t config;
  esy_err_t results[TEST_PHASE_COUNT][TEST_STATE_COUNT];
  esy_smf_decision_t decisions[TEST_STATE_COUNT];
  uint32_t trace[TEST_TRACE_CAPACITY];
  esy_smf_state_id_t observed_current[TEST_TRACE_CAPACITY];
  const void *events[TEST_STATE_COUNT];
  uint32_t trace_count;
  uint32_t reentry_checks;
  bool check_reentry;
} smf_test_ctx_t;

static smf_test_ctx_t s_ctx;

static esy_err_t action(void *user, uint32_t id, uint32_t phase) {
  smf_test_ctx_t *ctx = user;
  esy_smf_status_t status = ESY_SMF_UNINITIALIZED;
  esy_smf_state_id_t current = ESY_SMF_STATE_NONE;

  TEST_ASSERT_EQUAL_PTR(&s_ctx, ctx);
  TEST_ASSERT_LESS_THAN_UINT32(TEST_TRACE_CAPACITY, ctx->trace_count);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_get_state(&ctx->machine, &current));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_get_status(&ctx->machine, &status));
  TEST_ASSERT_NOT_EQUAL(ESY_SMF_UNINITIALIZED, status);
  ctx->trace[ctx->trace_count] = phase * TEST_STATE_COUNT + id;
  ctx->observed_current[ctx->trace_count] = current;
  ++ctx->trace_count;

  if (ctx->check_reentry) {
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY,
                           esy_smf_init(&ctx->machine, &ctx->config));
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_smf_start(&ctx->machine, ROOT));
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_smf_run(&ctx->machine, NULL));
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY,
                           esy_smf_transition(&ctx->machine, OTHER_ROOT));
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_smf_stop(&ctx->machine));
    TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY, esy_smf_deinit(&ctx->machine));
    ++ctx->reentry_checks;
  }
  return ctx->results[phase][id];
}

static esy_err_t run_action(void *user, const void *event,
                            esy_smf_decision_t *decision, uint32_t id) {
  smf_test_ctx_t *ctx = user;

  TEST_ASSERT_NOT_NULL(decision);
  TEST_ASSERT_EQUAL(ESY_SMF_HANDLED, decision->action);
  TEST_ASSERT_EQUAL_UINT32(ESY_SMF_STATE_NONE, decision->target);
  ctx->events[id] = event;
  *decision = ctx->decisions[id];
  return action(user, id, RUN);
}

#define TEST_CALLBACKS(name, id)                                              \
  static esy_err_t name##_entry(void *user) { return action(user, id, ENTRY); } \
  static esy_err_t name##_exit(void *user) { return action(user, id, EXIT); }   \
  static esy_err_t name##_run(void *user, const void *event,                    \
                              esy_smf_decision_t *decision) {                 \
    return run_action(user, event, decision, id);                              \
  }

TEST_CALLBACKS(root, ROOT)
TEST_CALLBACKS(branch, BRANCH)
TEST_CALLBACKS(leaf, LEAF)
TEST_CALLBACKS(sibling, SIBLING)
TEST_CALLBACKS(other_root, OTHER_ROOT)
TEST_CALLBACKS(other_leaf, OTHER_LEAF)

static const esy_smf_state_t k_states[TEST_STATE_COUNT] = {
    [ROOT] = {ESY_SMF_STATE_NONE, ESY_SMF_STATE_NONE, root_entry, root_run,
              root_exit},
    [BRANCH] = {ESY_SMF_STATE_NONE, ESY_SMF_STATE_NONE, branch_entry, branch_run,
                branch_exit},
    [LEAF] = {ESY_SMF_STATE_NONE, ESY_SMF_STATE_NONE, leaf_entry, leaf_run,
              leaf_exit},
    [SIBLING] = {ESY_SMF_STATE_NONE, ESY_SMF_STATE_NONE, sibling_entry,
                 sibling_run, sibling_exit},
    [OTHER_ROOT] = {ESY_SMF_STATE_NONE, ESY_SMF_STATE_NONE, other_root_entry,
                    other_root_run, other_root_exit},
    [OTHER_LEAF] = {ESY_SMF_STATE_NONE, ESY_SMF_STATE_NONE, other_leaf_entry,
                    other_leaf_run, other_leaf_exit},
};

static uint32_t trace_item(uint32_t phase, uint32_t state) {
  return phase * TEST_STATE_COUNT + state;
}

static void assert_trace(const uint32_t *expected, uint32_t count) {
  TEST_ASSERT_EQUAL_UINT32(count, s_ctx.trace_count);
  if (count != 0u) TEST_ASSERT_EQUAL_UINT32_ARRAY(expected, s_ctx.trace, count);
}

static void assert_machine(esy_smf_status_t expected_status,
                            esy_smf_state_id_t expected_state) {
  esy_smf_status_t status = ESY_SMF_UNINITIALIZED;
  esy_smf_state_id_t current = 123u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_get_status(&s_ctx.machine, &status));
  TEST_ASSERT_EQUAL(expected_status, status);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_get_state(&s_ctx.machine, &current));
  TEST_ASSERT_EQUAL_UINT32(expected_state, current);
}

static void configure_hierarchy(void) {
  s_ctx.states[ROOT].initial = BRANCH;
  s_ctx.states[BRANCH].parent = ROOT;
  s_ctx.states[BRANCH].initial = LEAF;
  s_ctx.states[LEAF].parent = BRANCH;
  s_ctx.states[SIBLING].parent = BRANCH;
  s_ctx.states[OTHER_ROOT].initial = OTHER_LEAF;
  s_ctx.states[OTHER_LEAF].parent = OTHER_ROOT;
}

static void initialize(void) {
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                         esy_smf_init(&s_ctx.machine, &s_ctx.config));
}

static void start_hierarchy(void) {
  configure_hierarchy();
  initialize();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_start(&s_ctx.machine, ROOT));
  s_ctx.trace_count = 0u;
}

static void assert_invalid_configuration(void) {
  unsigned char unchanged[sizeof(s_ctx.machine)];

  memcpy(unchanged, &s_ctx.machine, sizeof(unchanged));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                         esy_smf_init(&s_ctx.machine, &s_ctx.config));
  TEST_ASSERT_EQUAL_MEMORY(unchanged, &s_ctx.machine, sizeof(unchanged));
}

void setUp(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  memcpy(s_ctx.states, k_states, sizeof(k_states));
  s_ctx.config.states = s_ctx.states;
  s_ctx.config.state_count = TEST_STATE_COUNT;
  s_ctx.config.user = &s_ctx;
  for (uint32_t id = 0u; id < TEST_STATE_COUNT; ++id) {
    s_ctx.decisions[id].action = ESY_SMF_HANDLED;
    s_ctx.decisions[id].target = ESY_SMF_STATE_NONE;
  }
}

void tearDown(void) {}

void test_null_arguments_are_rejected_and_query_outputs_are_preserved(void) {
  esy_smf_state_id_t state = 123u;
  esy_smf_status_t status = ESY_SMF_FAULTED;

  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                         esy_smf_init(NULL, &s_ctx.config));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                         esy_smf_init(&s_ctx.machine, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_smf_start(NULL, ROOT));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_smf_run(NULL, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_smf_transition(NULL, ROOT));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_smf_stop(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_smf_deinit(NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG, esy_smf_get_state(NULL, &state));
  TEST_ASSERT_EQUAL_UINT32(123u, state);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                         esy_smf_get_state(&s_ctx.machine, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                         esy_smf_get_status(NULL, &status));
  TEST_ASSERT_EQUAL(ESY_SMF_FAULTED, status);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                         esy_smf_get_status(&s_ctx.machine, NULL));
}

void test_uninitialized_machine_allows_status_only(void) {
  esy_smf_status_t status = ESY_SMF_FAULTED;
  esy_smf_state_id_t state = 123u;

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_get_status(&s_ctx.machine, &status));
  TEST_ASSERT_EQUAL(ESY_SMF_UNINITIALIZED, status);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                         esy_smf_get_state(&s_ctx.machine, &state));
  TEST_ASSERT_EQUAL_UINT32(123u, state);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_smf_start(&s_ctx.machine, ROOT));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_smf_run(&s_ctx.machine, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                         esy_smf_transition(&s_ctx.machine, ROOT));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_smf_stop(&s_ctx.machine));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_smf_deinit(&s_ctx.machine));
  assert_trace(NULL, 0u);
}

void test_init_rejects_empty_or_sentinel_sized_tables(void) {
  s_ctx.config.states = NULL;
  assert_invalid_configuration();
  s_ctx.config.states = s_ctx.states;
  s_ctx.config.state_count = 0u;
  assert_invalid_configuration();
  s_ctx.config.state_count = UINT32_MAX;
  assert_invalid_configuration();
}

void test_init_rejects_out_of_range_parent_and_initial_links(void) {
  s_ctx.states[LEAF].parent = TEST_STATE_COUNT;
  assert_invalid_configuration();
  s_ctx.states[LEAF].parent = ESY_SMF_STATE_NONE;
  s_ctx.states[ROOT].initial = TEST_STATE_COUNT;
  assert_invalid_configuration();
}

void test_init_rejects_self_parent_and_multi_state_parent_cycles(void) {
  s_ctx.states[ROOT].parent = ROOT;
  assert_invalid_configuration();
  s_ctx.states[ROOT].parent = LEAF;
  s_ctx.states[LEAF].parent = BRANCH;
  s_ctx.states[BRANCH].parent = ROOT;
  assert_invalid_configuration();
}

void test_init_rejects_initial_links_to_self_siblings_or_grandchildren(void) {
  s_ctx.states[ROOT].initial = ROOT;
  assert_invalid_configuration();
  s_ctx.states[ROOT].initial = LEAF;
  s_ctx.states[LEAF].parent = BRANCH;
  s_ctx.states[BRANCH].parent = ROOT;
  assert_invalid_configuration();
  s_ctx.states[ROOT].initial = BRANCH;
  s_ctx.states[BRANCH].initial = SIBLING;
  s_ctx.states[SIBLING].parent = ROOT;
  assert_invalid_configuration();
}

void test_init_copies_configuration_without_executing_callbacks(void) {
  initialize();
  s_ctx.config.user = NULL;
  s_ctx.config.states = NULL;
  s_ctx.config.state_count = 0u;
  assert_machine(ESY_SMF_READY, ESY_SMF_STATE_NONE);
  assert_trace(NULL, 0u);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_start(&s_ctx.machine, ROOT));
  assert_machine(ESY_SMF_RUNNING, ROOT);
}

void test_repeated_init_retains_live_configuration(void) {
  esy_smf_config_t replacement = s_ctx.config;

  initialize();
  replacement.user = NULL;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                         esy_smf_init(&s_ctx.machine, &replacement));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_start(&s_ctx.machine, ROOT));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                         esy_smf_init(&s_ctx.machine, &replacement));
  assert_machine(ESY_SMF_RUNNING, ROOT);
}

void test_ready_stop_is_idempotent_and_deinit_allows_reinitialization(void) {
  esy_smf_status_t status;

  initialize();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_stop(&s_ctx.machine));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_stop(&s_ctx.machine));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_smf_run(&s_ctx.machine, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                         esy_smf_transition(&s_ctx.machine, ROOT));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_deinit(&s_ctx.machine));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_get_status(&s_ctx.machine, &status));
  TEST_ASSERT_EQUAL(ESY_SMF_UNINITIALIZED, status);
  TEST_ASSERT_NULL(s_ctx.machine.config.states);
  TEST_ASSERT_NULL(s_ctx.machine.config.user);
  TEST_ASSERT_EQUAL_UINT32(0u, s_ctx.machine.config.state_count);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_smf_deinit(&s_ctx.machine));
  initialize();
  assert_machine(ESY_SMF_READY, ESY_SMF_STATE_NONE);
  assert_trace(NULL, 0u);
}

void test_invalid_start_does_not_enter_states_or_change_ready_status(void) {
  initialize();
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                         esy_smf_start(&s_ctx.machine, TEST_STATE_COUNT));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                         esy_smf_start(&s_ctx.machine, ESY_SMF_STATE_NONE));
  assert_machine(ESY_SMF_READY, ESY_SMF_STATE_NONE);
  assert_trace(NULL, 0u);
}

void test_flat_machine_runs_transitions_and_stops_in_order(void) {
  const uint32_t expected[] = {
      trace_item(ENTRY, ROOT), trace_item(RUN, ROOT), trace_item(EXIT, ROOT),
      trace_item(ENTRY, OTHER_ROOT), trace_item(EXIT, OTHER_ROOT)};
  const uint32_t event = 42u;

  initialize();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_start(&s_ctx.machine, ROOT));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_smf_start(&s_ctx.machine, ROOT));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_smf_deinit(&s_ctx.machine));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_run(&s_ctx.machine, &event));
  TEST_ASSERT_EQUAL_PTR(&event, s_ctx.events[ROOT]);
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                         esy_smf_transition(&s_ctx.machine, OTHER_ROOT));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_stop(&s_ctx.machine));
  assert_machine(ESY_SMF_READY, ESY_SMF_STATE_NONE);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_start(&s_ctx.machine, OTHER_ROOT));
  assert_machine(ESY_SMF_RUNNING, OTHER_ROOT);
}

void test_null_callbacks_and_null_user_support_polling(void) {
  const esy_smf_state_t state = {
      .parent = ESY_SMF_STATE_NONE, .initial = ESY_SMF_STATE_NONE};
  const esy_smf_config_t config = {&state, 1u, NULL};

  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_init(&s_ctx.machine, &config));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_start(&s_ctx.machine, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_run(&s_ctx.machine, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_transition(&s_ctx.machine, 0u));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_stop(&s_ctx.machine));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_deinit(&s_ctx.machine));
  assert_trace(NULL, 0u);
}

void test_start_descends_initial_children_with_outermost_first_entry(void) {
  const uint32_t expected[] = {trace_item(ENTRY, ROOT), trace_item(ENTRY, BRANCH),
                               trace_item(ENTRY, LEAF)};
  const uint32_t observed[] = {ESY_SMF_STATE_NONE, ROOT, BRANCH};

  configure_hierarchy();
  initialize();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_start(&s_ctx.machine, ROOT));
  assert_machine(ESY_SMF_RUNNING, LEAF);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
  TEST_ASSERT_EQUAL_UINT32_ARRAY(observed, s_ctx.observed_current, 3u);
}

void test_start_from_nested_state_enters_ancestors_then_its_initial_chain(void) {
  const uint32_t expected[] = {trace_item(ENTRY, ROOT), trace_item(ENTRY, BRANCH),
                               trace_item(ENTRY, LEAF)};

  configure_hierarchy();
  initialize();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_start(&s_ctx.machine, BRANCH));
  assert_machine(ESY_SMF_RUNNING, LEAF);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
}

void test_explicit_leaf_start_ignores_different_ancestor_initial_child(void) {
  const uint32_t expected[] = {trace_item(ENTRY, ROOT), trace_item(ENTRY, BRANCH),
                               trace_item(ENTRY, SIBLING)};

  configure_hierarchy();
  initialize();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_start(&s_ctx.machine, SIBLING));
  assert_machine(ESY_SMF_RUNNING, SIBLING);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
}

void test_composite_without_initial_child_can_be_active(void) {
  configure_hierarchy();
  s_ctx.states[BRANCH].initial = ESY_SMF_STATE_NONE;
  initialize();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_start(&s_ctx.machine, ROOT));
  assert_machine(ESY_SMF_RUNNING, BRANCH);
}

void test_stop_exits_innermost_first_and_queries_report_state_being_exited(void) {
  const uint32_t expected[] = {trace_item(EXIT, LEAF), trace_item(EXIT, BRANCH),
                               trace_item(EXIT, ROOT)};
  const uint32_t observed[] = {LEAF, BRANCH, ROOT};

  start_hierarchy();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_stop(&s_ctx.machine));
  assert_machine(ESY_SMF_READY, ESY_SMF_STATE_NONE);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
  TEST_ASSERT_EQUAL_UINT32_ARRAY(observed, s_ctx.observed_current, 3u);
}

void test_sibling_transition_retains_common_ancestors(void) {
  const uint32_t expected[] = {trace_item(EXIT, LEAF), trace_item(ENTRY, SIBLING)};

  start_hierarchy();
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                         esy_smf_transition(&s_ctx.machine, SIBLING));
  assert_machine(ESY_SMF_RUNNING, SIBLING);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
}

void test_transition_between_roots_unwinds_old_chain_and_descends_target(void) {
  const uint32_t expected[] = {
      trace_item(EXIT, LEAF), trace_item(EXIT, BRANCH), trace_item(EXIT, ROOT),
      trace_item(ENTRY, OTHER_ROOT), trace_item(ENTRY, OTHER_LEAF)};

  start_hierarchy();
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                         esy_smf_transition(&s_ctx.machine, OTHER_ROOT));
  assert_machine(ESY_SMF_RUNNING, OTHER_LEAF);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
}

void test_leaf_self_transition_exits_and_reenters_only_leaf(void) {
  const uint32_t expected[] = {trace_item(EXIT, LEAF), trace_item(ENTRY, LEAF)};

  start_hierarchy();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_transition(&s_ctx.machine, LEAF));
  assert_machine(ESY_SMF_RUNNING, LEAF);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
}

void test_transition_to_ancestor_reenters_it_then_follows_initial_children(void) {
  const uint32_t expected[] = {
      trace_item(EXIT, LEAF), trace_item(EXIT, BRANCH),
      trace_item(ENTRY, BRANCH), trace_item(ENTRY, LEAF)};

  start_hierarchy();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_transition(&s_ctx.machine, BRANCH));
  assert_machine(ESY_SMF_RUNNING, LEAF);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
}

void test_transition_from_active_ancestor_to_descendant_reenters_ancestor(void) {
  const uint32_t expected[] = {trace_item(EXIT, BRANCH), trace_item(ENTRY, BRANCH),
                               trace_item(ENTRY, SIBLING)};

  configure_hierarchy();
  s_ctx.states[BRANCH].initial = ESY_SMF_STATE_NONE;
  initialize();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_start(&s_ctx.machine, BRANCH));
  s_ctx.trace_count = 0u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                         esy_smf_transition(&s_ctx.machine, SIBLING));
  assert_machine(ESY_SMF_RUNNING, SIBLING);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
}

void test_invalid_owner_transition_retains_running_chain_without_callbacks(void) {
  start_hierarchy();
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                         esy_smf_transition(&s_ctx.machine, TEST_STATE_COUNT));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                         esy_smf_transition(&s_ctx.machine, ESY_SMF_STATE_NONE));
  assert_machine(ESY_SMF_RUNNING, LEAF);
  assert_trace(NULL, 0u);
}

void test_handled_event_does_not_call_parent_and_ignored_target_is_allowed(void) {
  const uint32_t expected[] = {trace_item(RUN, LEAF)};

  start_hierarchy();
  s_ctx.decisions[LEAF].target = TEST_STATE_COUNT;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_run(&s_ctx.machine, NULL));
  TEST_ASSERT_NULL(s_ctx.events[LEAF]);
  assert_machine(ESY_SMF_RUNNING, LEAF);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
}

void test_propagation_forwards_same_event_leaf_first_and_may_pass_root(void) {
  const uint32_t expected[] = {trace_item(RUN, LEAF), trace_item(RUN, BRANCH),
                               trace_item(RUN, ROOT)};
  const uint32_t event = 73u;

  start_hierarchy();
  s_ctx.decisions[LEAF].action = ESY_SMF_PROPAGATE;
  s_ctx.decisions[BRANCH].action = ESY_SMF_PROPAGATE;
  s_ctx.decisions[ROOT].action = ESY_SMF_PROPAGATE;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_run(&s_ctx.machine, &event));
  TEST_ASSERT_EQUAL_PTR(&event, s_ctx.events[LEAF]);
  TEST_ASSERT_EQUAL_PTR(&event, s_ctx.events[BRANCH]);
  TEST_ASSERT_EQUAL_PTR(&event, s_ctx.events[ROOT]);
  assert_machine(ESY_SMF_RUNNING, LEAF);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
}

void test_missing_run_callbacks_propagate_to_parent(void) {
  const uint32_t expected[] = {trace_item(RUN, ROOT)};

  configure_hierarchy();
  s_ctx.states[LEAF].run = NULL;
  s_ctx.states[BRANCH].run = NULL;
  initialize();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_start(&s_ctx.machine, ROOT));
  s_ctx.trace_count = 0u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_run(&s_ctx.machine, NULL));
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
}

void test_leaf_run_decision_performs_one_transition_without_running_target(void) {
  const uint32_t expected[] = {trace_item(RUN, LEAF), trace_item(EXIT, LEAF),
                               trace_item(ENTRY, SIBLING)};

  start_hierarchy();
  s_ctx.decisions[LEAF].action = ESY_SMF_TRANSITION;
  s_ctx.decisions[LEAF].target = SIBLING;
  s_ctx.decisions[SIBLING].action = ESY_SMF_TRANSITION;
  s_ctx.decisions[SIBLING].target = OTHER_ROOT;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_run(&s_ctx.machine, NULL));
  assert_machine(ESY_SMF_RUNNING, SIBLING);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
}

void test_parent_run_transition_uses_handler_as_source_and_reenters_it(void) {
  const uint32_t expected[] = {
      trace_item(RUN, LEAF), trace_item(RUN, BRANCH),
      trace_item(EXIT, LEAF), trace_item(EXIT, BRANCH),
      trace_item(ENTRY, BRANCH), trace_item(ENTRY, SIBLING)};

  start_hierarchy();
  s_ctx.decisions[LEAF].action = ESY_SMF_PROPAGATE;
  s_ctx.decisions[BRANCH].action = ESY_SMF_TRANSITION;
  s_ctx.decisions[BRANCH].target = SIBLING;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_run(&s_ctx.machine, NULL));
  assert_machine(ESY_SMF_RUNNING, SIBLING);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
}

void test_parent_self_transition_reenters_parent_and_its_initial_chain(void) {
  const uint32_t expected[] = {
      trace_item(RUN, LEAF), trace_item(RUN, BRANCH),
      trace_item(EXIT, LEAF), trace_item(EXIT, BRANCH),
      trace_item(ENTRY, BRANCH), trace_item(ENTRY, LEAF)};

  start_hierarchy();
  s_ctx.decisions[LEAF].action = ESY_SMF_PROPAGATE;
  s_ctx.decisions[BRANCH].action = ESY_SMF_TRANSITION;
  s_ctx.decisions[BRANCH].target = BRANCH;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_run(&s_ctx.machine, NULL));
  assert_machine(ESY_SMF_RUNNING, LEAF);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
}

void test_initial_entry_failure_faults_without_owning_failed_state(void) {
  const uint32_t expected[] = {trace_item(ENTRY, ROOT)};

  initialize();
  s_ctx.results[ENTRY][ROOT] = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_smf_start(&s_ctx.machine, ROOT));
  assert_machine(ESY_SMF_FAULTED, ESY_SMF_STATE_NONE);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_stop(&s_ctx.machine));
  assert_machine(ESY_SMF_READY, ESY_SMF_STATE_NONE);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
}

void test_partial_start_failure_retains_only_successfully_entered_ancestors(void) {
  const uint32_t expected[] = {
      trace_item(ENTRY, ROOT), trace_item(ENTRY, BRANCH), trace_item(ENTRY, LEAF),
      trace_item(EXIT, BRANCH), trace_item(EXIT, ROOT)};

  configure_hierarchy();
  initialize();
  s_ctx.results[ENTRY][LEAF] = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_smf_start(&s_ctx.machine, ROOT));
  assert_machine(ESY_SMF_FAULTED, BRANCH);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_smf_run(&s_ctx.machine, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_smf_start(&s_ctx.machine, ROOT));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE,
                         esy_smf_transition(&s_ctx.machine, OTHER_ROOT));
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_STATE, esy_smf_deinit(&s_ctx.machine));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_stop(&s_ctx.machine));
  assert_machine(ESY_SMF_READY, ESY_SMF_STATE_NONE);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
}

void test_run_error_faults_without_executing_requested_transition(void) {
  const uint32_t expected[] = {trace_item(RUN, LEAF)};

  start_hierarchy();
  s_ctx.results[RUN][LEAF] = ESY_ERR_TIMEOUT;
  s_ctx.decisions[LEAF].action = ESY_SMF_TRANSITION;
  s_ctx.decisions[LEAF].target = SIBLING;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_TIMEOUT, esy_smf_run(&s_ctx.machine, NULL));
  assert_machine(ESY_SMF_FAULTED, LEAF);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_stop(&s_ctx.machine));
  assert_machine(ESY_SMF_READY, ESY_SMF_STATE_NONE);
}

void test_invalid_run_action_faults_with_chain_preserved(void) {
  start_hierarchy();
  s_ctx.decisions[LEAF].action = (esy_smf_action_t)99;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                         esy_smf_run(&s_ctx.machine, NULL));
  assert_machine(ESY_SMF_FAULTED, LEAF);
}

void test_invalid_run_transition_target_faults_without_exiting_states(void) {
  const uint32_t expected[] = {trace_item(RUN, LEAF)};

  start_hierarchy();
  s_ctx.decisions[LEAF].action = ESY_SMF_TRANSITION;
  s_ctx.decisions[LEAF].target = ESY_SMF_STATE_NONE;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_INVALID_ARG,
                         esy_smf_run(&s_ctx.machine, NULL));
  assert_machine(ESY_SMF_FAULTED, LEAF);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
}

void test_transition_exit_failure_retains_failed_state_and_stops_before_entry(void) {
  const uint32_t expected[] = {
      trace_item(EXIT, LEAF), trace_item(EXIT, BRANCH),
      trace_item(EXIT, BRANCH), trace_item(EXIT, ROOT)};

  start_hierarchy();
  s_ctx.results[EXIT][BRANCH] = ESY_ERR_BUSY;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_BUSY,
                         esy_smf_transition(&s_ctx.machine, OTHER_ROOT));
  assert_machine(ESY_SMF_FAULTED, BRANCH);
  s_ctx.results[EXIT][BRANCH] = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_stop(&s_ctx.machine));
  assert_machine(ESY_SMF_READY, ESY_SMF_STATE_NONE);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
}

void test_transition_entry_failure_retains_new_successfully_entered_chain(void) {
  const uint32_t expected[] = {
      trace_item(EXIT, LEAF), trace_item(EXIT, BRANCH), trace_item(EXIT, ROOT),
      trace_item(ENTRY, OTHER_ROOT), trace_item(ENTRY, OTHER_LEAF),
      trace_item(EXIT, OTHER_ROOT)};

  start_hierarchy();
  s_ctx.results[ENTRY][OTHER_LEAF] = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO,
                         esy_smf_transition(&s_ctx.machine, OTHER_ROOT));
  assert_machine(ESY_SMF_FAULTED, OTHER_ROOT);
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_stop(&s_ctx.machine));
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
}

void test_transition_first_entry_failure_leaves_no_entered_state(void) {
  start_hierarchy();
  s_ctx.results[ENTRY][OTHER_ROOT] = ESY_ERR_FAIL;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_FAIL,
                         esy_smf_transition(&s_ctx.machine, OTHER_ROOT));
  assert_machine(ESY_SMF_FAULTED, ESY_SMF_STATE_NONE);
  s_ctx.trace_count = 0u;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_stop(&s_ctx.machine));
  assert_trace(NULL, 0u);
}

void test_stop_failure_can_be_retried_without_repeating_successful_exits(void) {
  const uint32_t expected[] = {
      trace_item(EXIT, LEAF), trace_item(EXIT, BRANCH), trace_item(EXIT, BRANCH),
      trace_item(EXIT, BRANCH), trace_item(EXIT, ROOT)};

  start_hierarchy();
  s_ctx.results[EXIT][BRANCH] = ESY_ERR_IO;
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_smf_stop(&s_ctx.machine));
  assert_machine(ESY_SMF_FAULTED, BRANCH);
  TEST_ASSERT_EQUAL_INT32(ESY_ERR_IO, esy_smf_stop(&s_ctx.machine));
  assert_machine(ESY_SMF_FAULTED, BRANCH);
  s_ctx.results[EXIT][BRANCH] = ESY_OK;
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_stop(&s_ctx.machine));
  assert_machine(ESY_SMF_READY, ESY_SMF_STATE_NONE);
  assert_trace(expected, sizeof(expected) / sizeof(expected[0]));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_deinit(&s_ctx.machine));
}

void test_entry_run_and_exit_allow_queries_but_reject_all_mutating_reentry(void) {
  s_ctx.check_reentry = true;
  initialize();
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_start(&s_ctx.machine, ROOT));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_run(&s_ctx.machine, NULL));
  TEST_ASSERT_EQUAL_INT32(ESY_OK,
                         esy_smf_transition(&s_ctx.machine, OTHER_ROOT));
  TEST_ASSERT_EQUAL_INT32(ESY_OK, esy_smf_stop(&s_ctx.machine));
  TEST_ASSERT_EQUAL_UINT32(5u, s_ctx.reentry_checks);
  assert_machine(ESY_SMF_READY, ESY_SMF_STATE_NONE);
}
