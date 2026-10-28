<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Diagnostics

## Purpose

`esy_diagnostics` publishes named health assessments and explicitly requested,
synchronous self-tests. It supplies one typed result model for local callers
and remote-management frontends without selecting a wire format, transport,
storage backend or hardware implementation. No diagnostics are installed or
executed automatically.

## Public Surface

`esy_common/defs/diagnostics/esy_diagnostics_defs.h` defines the shared health, capability,
result and snapshot types. `esy_middleware/diagnostics/esy_diagnostics.h` provides
the caller-owned handle, immutable source registrations and these operations:

| Operation | Behavior |
| --- | --- |
| `init` / `deinit` | Borrow or detach one source inventory without invoking sources. |
| `enumerate` | Copy one source's name and supported operations; never query health or start a test. |
| `read` | Obtain one current assessment from the exact named source. |
| `run` | Explicitly execute one supported self-test to completion. |

An assessment contains `health`, a source-defined unsigned `code`, and optional
printable ASCII detail of at most 96 bytes. Health is UNKNOWN, OK, WARNING or
ERROR. Operation success and health are separate: a completed test detecting a
fault returns `ESY_OK` with ERROR health. UNKNOWN is allowed for READ and is
invalid as the result of a completed RUN.

## Integration

Select `ENABLE_ESY_DIAGNOSTICS` in the consumer's sole feature entry and link
`esy_diagnostics`. The target adds only common types and private build options;
it requires no HAL, OS, filesystem, command shell or transport. A remote frontend
can borrow the same instance, authorize a request and call its public APIs.
The diagnostics module itself does not authenticate or authorize callers.

Initialize the handle with `{0}`. Supply `config.static_resources` containing
an immutable `esy_diagnostics_entry_t` array and its count. Every entry has a
canonical name and a required READ callback; RUN is optional. The module copies
the array binding, while registrations, names and source contexts stay borrowed
until every frontend detaches and deinit completes. Keep their addresses stable;
do not copy, move, reset or free a live handle or its source state.

Names contain 1..48 ASCII letters, digits, `_`, `.`, `-` or `/`, with exact,
case-sensitive matching. Registered names are NUL-terminated; lookup APIs use
explicit spans and do not require a terminator. A lookup can refer directly to
a registered name. Duplicate names are rejected, while distinct prefixes remain
separate sources. An empty array is supported without placeholder callbacks.

### Usage Example

For a synchronous local query, pass a consumer-prepared configuration containing
the real source registrations and an exact registered name. Their names and
contexts remain valid throughout this call; source callbacks retain nothing.

```c
#include "esy_middleware/diagnostics/esy_diagnostics.h"

esy_err_t read_health(const esy_diagnostics_config_t *config,
                      const char *name, size_t name_length,
                      esy_diagnostics_snapshot_t *snapshot) {
    esy_diagnostics_t diagnostics = {0};
    esy_err_t result = esy_diagnostics_init(&diagnostics, config);
    if (result != ESY_OK) {
        return result;
    }
    result = esy_diagnostics_read(&diagnostics, name, name_length, snapshot);
    esy_err_t cleanup = esy_diagnostics_deinit(&diagnostics);
    return result != ESY_OK ? result : cleanup;
}
```

Use one non-ISR owner and disjoint output storage. Consume the snapshot only on
`ESY_OK`, then inspect its assessment: successful READ can still report WARNING
or ERROR health. This example neither starts a self-test nor installs a frontend.

## Contracts

- One non-ISR task or main-loop owner serializes all APIs, source updates and
  lifecycle changes. The per-instance busy guard rejects callback reentry to
  init, deinit, enumeration, READ or RUN; it supplies no inter-task lock.
- Source callbacks run synchronously on the caller and must complete within its
  declared execution budget. READ never starts a self-test or changes controlled
  outputs. RUN completes exactly one requested self-test, without an automatic
  follow-up READ, retry, scheduler or asynchronous job.
- A callback returns `ESY_OK` for a trustworthy assessment, including WARNING or
  ERROR findings. Negative provider errors propagate. Positive returns, invalid
  health values, malformed details and UNKNOWN from RUN return STATE. The
  caller's snapshot remains unchanged on all errors, even when a failed RUN has
  already affected provider state. Reconcile through READ or the source owner
  before deciding whether another action is safe.
- Detail length is 0..96, its terminator is at that length and every declared
  byte is printable ASCII `0x20..0x7e`. Trailing storage is ignored. Detail is
  explanatory text; callers use the named source, health and code for structured
  decisions. No timestamp, persistent history or universal reason-code registry
  is implied.
- Enumeration returns the stable count/index/name/capabilities and an entirely
  zero assessment. It invokes no source callback. Every registered source has
  READ capability; RUN is advertised only when its callback exists. An empty
  inventory returns a zero snapshot for index zero; other indices are invalid.
  Unknown names and unsupported self-tests return NOT_SUPPORTED.
- Required pointers, bounded spans, representable inventory sizes and known
  output aliases are validated before callbacks or writes. Output cannot cover
  the handle, registration array, any registered name or the lookup span.
  Accessible objects and output disjointness from opaque source-owned state
  remain caller preconditions; the module cannot discover arbitrary context
  object sizes. Configuration errors preserve the handle for correction/retry.
- The consumer owns self-test authorization, admission, quiescence, hardware
  output safety and OTA/Flash arbitration. Cross-task sources hand off through
  their owner's coherent snapshots or narrow request/completion interface;
  callbacks must not execute another task's private business flow directly.
  No module-owned hardware test, startup health gate or boot confirmation occurs.

## Resource Model

The module borrows one registration array and the names/callback contexts it
references. It allocates no backing storage, task, timer, queue, IRQ, DMA, lock,
Flash partition or heap object. Snapshots and temporary provider results are
bounded values on the caller's stack. All callbacks and their full execution
cost belong to that caller, including any provider-specific I/O or waiting.

Init validates duplicate names in O(count squared) bounded name comparisons.
Enumeration and lookup validate known output aliases across the inventory;
lookup additionally searches exact names in O(count). Each individual name is
at most 48 bytes. The public count is at most UINT32_MAX and must fit a native
array size; consumers budget their actual catalogue and source cost. There is
no automatic polling interval or time calculation.

## Verification

From this module's `tests` directory:

```sh
ceedling test:all
ceedling gcov:all
```

From the platform root:

```sh
ruby tools/quality/run_module_tests.rb esy_middleware/diagnostics/tests \
  esy_middleware/diagnostics/tests/build/sanitizer test:all --sanitizer
python3 -B esy_middleware/diagnostics/tests/test_cmake_consumer.py
tools/checks/check_no_dynamic_alloc.sh esy_middleware/diagnostics
```

Unit tests cover metadata-only discovery, callback/context selection, successful
unhealthy findings, READ versus RUN unknown health, explicit bounded lookup,
canonical names and maximum detail lengths, lifecycle/reentry, empty catalogues,
provider errors, malformed assessments and detectable alias/range violations.
Sanitizers exercise the same boundaries without a HAL/OS fake. Consumer checks
compile C11/C++17 public contracts and validate optional frontend assembly.

The consumer owns timing budgets and safety admission for actual sensor, memory,
bus, storage or output self-tests. Target procedures measure their provider
behavior. Host tests establish dispatch and result contracts; they do not
establish physical hardware health or OTA safety.

## Known Limits

All operations are synchronous and need a serialized owner. This module supplies
no asynchronous test lifecycle, cancellation, deadline, progress stream,
automatic hardware probing, fault capture, statistics collection, persistence
or recovery action. It does not certify a provider as nonblocking, ISR-safe or
safe to run during OTA. Sources requiring another execution context must use a
consumer-owned handoff compatible with their actual lifecycle and timing.
