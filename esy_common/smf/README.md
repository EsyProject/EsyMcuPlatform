<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# State Machine Framework

## Purpose

The state machine framework (SMF) provides caller-driven flat and hierarchical
state machines with entry, run and exit actions. It centralizes transition
ordering and parent event propagation while leaving states, events and policy
with the owning module. The core is pure C11, with no OS, HAL, board or provider
dependency. Module-specific state machines are independent of this framework.

## Public Surface

`include/esy_common/smf/esy_smf.h` declares:

- `esy_smf_state_t`: an immutable table entry containing optional callbacks,
  a parent index and an optional direct initial child;
- `esy_smf_config_t` and caller-owned `esy_smf_t`;
- `esy_smf_init`, `esy_smf_start`, `esy_smf_run`, `esy_smf_transition`,
  `esy_smf_stop` and `esy_smf_deinit`;
- state and lifecycle queries, including recovery after callback failures.

State IDs are indices into the table. Explicitly use `ESY_SMF_STATE_NONE` for
absent parent and initial links; zero is a valid state index.

## Integration

Select `set(ENABLE_ESY_COMMON_SMF ON)` in the consuming project's selected
feature file before adding EsyMcuPlatform. Link `esy_common_smf` or its alias
`Esy::common_smf`. The `esy_common` / `Esy::common` aggregate also links the
enabled module. False or unset selection excludes the module. The platform
does not declare a feature default or a cache override.

A common-only consumer can add the `esy_common` directory directly after
selecting the feature. No OS or hardware provider is needed. Optional
`esy_opt_default` compiler settings are private when that target exists.

This complete C11/C++17 example starts at a parent, follows its initial child,
then transitions between siblings while keeping the parent entered:

```c
#include "esy_common/smf/esy_smf.h"

enum { SESSION, WAITING, WORKING, STATE_COUNT };
typedef struct { unsigned sessions; } example_t;

static esy_err_t enter_session(void *user) {
  example_t *example = (example_t *)user;
  ++example->sessions;
  return ESY_OK;
}

static esy_err_t leave_session(void *user) {
  example_t *example = (example_t *)user;
  --example->sessions;
  return ESY_OK;
}

static esy_err_t waiting_run(void *user, const void *event,
                             esy_smf_decision_t *decision) {
  (void)user;
  if (event != NULL) {
    decision->action = ESY_SMF_TRANSITION;
    decision->target = WORKING;
  }
  return ESY_OK;
}

static const esy_smf_state_t k_states[] = {
  {ESY_SMF_STATE_NONE, WAITING, enter_session, NULL, leave_session},
  {SESSION, ESY_SMF_STATE_NONE, NULL, waiting_run, NULL},
  {SESSION, ESY_SMF_STATE_NONE, NULL, NULL, NULL}
};

int main(void) {
#ifdef __cplusplus
  esy_smf_t machine = {};
#else
  esy_smf_t machine = {0};
#endif
  example_t example = {0u};
  const esy_smf_config_t config = {k_states, STATE_COUNT, &example};
  esy_smf_state_id_t state = ESY_SMF_STATE_NONE;
  esy_smf_status_t status = ESY_SMF_UNINITIALIZED;
  const unsigned event = 1u;
  if (esy_smf_init(&machine, &config) != ESY_OK) return 1;
  if (esy_smf_start(&machine, SESSION) != ESY_OK) return 2;
  if (esy_smf_get_state(&machine, &state) != ESY_OK ||
      state != WAITING || example.sessions != 1u) return 3;
  if (esy_smf_run(&machine, &event) != ESY_OK) return 4;
  if (esy_smf_get_state(&machine, &state) != ESY_OK ||
      state != WORKING || example.sessions != 1u) return 5;
  if (esy_smf_transition(&machine, WAITING) != ESY_OK) return 6;
  if (esy_smf_run(&machine, NULL) != ESY_OK) return 7;
  if (esy_smf_stop(&machine) != ESY_OK || example.sessions != 0u) return 8;
  if (esy_smf_get_status(&machine, &status) != ESY_OK ||
      status != ESY_SMF_READY) return 9;
  if (esy_smf_deinit(&machine) != ESY_OK) return 10;
  return 0;
}
```

For a flat machine, every state has `ESY_SMF_STATE_NONE` as its parent and
initial child. A task or baremetal main loop calls `run` when it has an event
or polling work; SMF imposes no polling period. The example uses callbacks that
cannot fail. Production callers must check every result and retain the machine,
table and user data until cleanup succeeds.

## Contracts

- A single serialized owner performs all calls and callbacks. The core is not
  ISR safe and contains no lock. Synchronous callback time is part of the
  owner's budget. Mutating reentry from any callback returns `ESY_ERR_BUSY`;
  queries there are allowed. Use the run decision to request a transition.
- Entry proceeds outermost first; exit proceeds innermost first. Start and
  transition follow the target's initial-child chain without executing run
  actions. An initial link must name a direct child. Multiple roots are allowed;
  a composite without an initial child can itself be the active state.
- Run visits the active state first. A missing handler or `ESY_SMF_PROPAGATE`
  offers the same event to the parent. `ESY_SMF_HANDLED` ends dispatch. A
  transition ends dispatch immediately, with at most one transition per run.
- All transitions are external. Siblings retain their common ancestors. A
  transition to self or between an ancestor and descendant exits and reenters
  the ancestor. A parent's run decision is sourced from that parent, so a
  parent-to-descendant transition reenters the parent and its selected child
  path. An explicit owner transition is sourced from the active state.
- Init requires `{0}` (or equivalent zero initialization), validates every link
  and rejects parent cycles. Repeated init preserves ownership and returns
  `ESY_ERR_STATE`. Failed init leaves the supplied machine unchanged.
- Any non-success callback result or malformed run decision faults the machine.
  Stop unwinds the deepest successfully entered state chain. Failed entry must
  release its own partial resources; failed exit must retain its state's
  resources so that stop can retry. Already successful exits are not repeated.
  Error propagation is synchronous, with no automatic rollback or retry.
- A faulted machine rejects start, run and transition. After successful stop it
  is READY and may restart or deinitialize. READY stop is idempotent. Successful
  deinit releases borrowed references and restores the zero-initialized state.
  Never reset storage to bypass a failed stop.
- Entry callbacks observe the previously entered ancestor through the state
  query; exit callbacks observe the state being exited. Queries are serialized
  owner operations, including during callbacks, rather than concurrent reads.

## Resource Model

The machine, immutable state table and callback user object belong to the caller
and remain valid until successful deinit. Multiple machines may share an
immutable table and hold separate user contexts. Events are borrowed only until
`run` returns. There is no heap, hidden singleton, recursion, timer, task, queue,
hardware handle or device power-management registration.

For N states and maximum ancestry depth D, validation takes O(N*D) time.
Dispatch and transitions use O(D*D) framework work and O(1) auxiliary storage.
A run executes at most D run callbacks, D exit callbacks and D entry callbacks;
stop executes at most D exits. Callback cost is additional. Application owners
set their own practical table/depth limits and callback budgets.

SMF does not inspect time. Timeout clocks, units, wrap behavior and cancellation
belong to the module supplying events. OTA execution policy likewise belongs
to the task or main-loop owner; this module creates no independently running
work and does not hold a device reference or power lock.

## Verification

From this module's `tests` directory, run `ceedling test:all`. From the
EsyMcuPlatform repository root, run:

```sh
python3 -B esy_common/smf/tests/test_cmake_consumer.py
tools/checks/check_esy_common_contracts.sh
tools/checks/check_no_dynamic_alloc.sh
ruby tools/quality/run_module_tests.rb esy_common/smf/tests \
  esy_common/smf/tests/build/sanitizer test:all --sanitizer
```

The unit suite covers flat and hierarchical order, default-child descent,
propagation, external transition sources, invalid topology, failure cleanup and
reentrant mutation rejection. CMake checks build and execute the example above
as strict C11 and C++17 consumers through the aggregate and narrow targets.
They also check disabled/unset source exclusion, optional private compiler
settings and consumer feature-file precedence over conflicting cache values.
`ESY_RTOS=None` and `ESY_RTOS=FreeRTOS` are inputs in these common-only checks;
they deliberately assemble no scheduler or hardware provider.

The repository quality gate discovers this module's Ceedling suite and runs its
CMake consumer and sanitizer checks. Real callback execution budgets remain a
consumer validation responsibility.

## Known Limits

- History states, orthogonal regions, internal/local transition modes and
  deferred event storage are outside this module's contract.
- Callback errors retain ownership for explicit stop; SMF cannot undo arbitrary
  external side effects or repair a callback that violates its failure contract.
- Public concrete machine storage permits static allocation. Consumers must
  use the same concrete context layout in every translation unit and rebuild
  all consumers after changing it.
- Module state machine and public context integration belongs to the consumer.
