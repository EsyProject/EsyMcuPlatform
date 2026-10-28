<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# OTA Middleware

## Purpose

`esy_ota_core` manages one caller-owned sequential image upload. Its typed
manifest, byte ranges and status are independent of ProtoBin, JSON, Flash and OS
services. A storage provider performs preparation, writes, verification,
activation and cleanup. The optional `esy_ota_flash` provider uses public HAL
Flash APIs and the bootloader middleware.

Hosts upload through the [Remote Manager OTA group](../remote_manager/README.md).
The manager's optional OTA adapter delegates normalized operations to this core;
the channel/session owner still owns transport delivery and product admission.
There is no separate OTA wire domain, device-driven discovery/data request flow,
protocol router or transport adapter in this module.

## Public Surface

| API | Behavior |
| --- | --- |
| `esy_ota_init` / `esy_ota_deinit` | Bind caller resources / perform checked cleanup and detach. |
| `esy_ota_begin` | Validate the manifest and prepare a fresh RECEIVING session. |
| `esy_ota_write` | Accept the next byte range or the exact most recent block again. |
| `esy_ota_finish` | Flush and verify the complete image; enter READY without boot publication. |
| `esy_ota_activate` | Explicitly publish a READY image; enter ACTIVATED without resetting. |
| `esy_ota_abort` | Release an unfinished session without promising persistent rollback. |
| `esy_ota_status` / `esy_ota_poll` | Copy cached status / observe the progress deadline. |
| `esy_ota_flash_init` / `esy_ota_flash_deinit` | Bind / detach the optional native provider and its cache. |
| `esy_ota_flash_get_provider` | Copy typed provider operations borrowing an initialized Flash handle. |

Shared manifest, phase and status values are declared in
[`esy_image_defs.h`](../../esy_common/defs/include/esy_common/defs/image/esy_image_defs.h).
They contain no runtime handle or wire framing. The manifest contains version,
image size, SHA-256 and a detached signature. The status reports session ID,
phase, accepted byte offset, total size and an optional terminal result.

## Integration

The consuming project's feature import selects the generic core:

```cmake
set(ENABLE_ESY_OTA ON)
```

Link `esy_ota_core`. Its public and compiled dependencies contain only common
value/guard types and private build options. Selecting a concrete Flash provider
also requires:

```cmake
set(ENABLE_ESY_OTA_FLASH ON)
set(ENABLE_ESY_SYSTEM ON)
set(ENABLE_ESY_FLASH ON)
set(ENABLE_ESY_BOOTLOADER ON)
```

Link `esy_ota_flash` for the native provider. HAL Flash, bootloader and common
utilities must already be available. The child feature without its parent,
missing provider features and missing provider targets are configuration errors.
Neither target creates a task or selects a transport. An alternate storage
provider supplies the same `esy_ota_provider_t` operations without enabling the
native Flash target.

The serialized owner initializes Flash and bootloader dependencies, binds a
zero-initialized Flash handle and cache, obtains its provider descriptor, then
initializes a zero-initialized `esy_ota_t` with that descriptor. Set explicit image
and chunk limits, distinct last-block storage, a fresh-session-ID callback and,
when timeouts are enabled, a continuous clock. Configuration/resource descriptors
are copied; their backing objects remain borrowed.

The host sends BEGIN with the manifest, then sequential WRITE requests containing
the returned session ID, byte offset and image bytes. After all bytes, FINISH
verifies storage. Only after receiving that result does the host explicitly send
ACTIVATE. STATUS resolves uncertain delivery outcomes; repeated FINISH/ACTIVATE
do not repeat successful provider work. The owner chooses when to reset and
confirms the running image after reconnection. Wire layout and encoding belong
to the Remote Manager adapter, not this module.

Before BEGIN, the product owner must complete authorization, candidate policy,
Flash arbitration and required task/ISR/DMA quiescence. Transport ACK handling,
watchdog scheduling, failure recovery, reset and reconnect/version confirmation
remain owner responsibilities. A disconnect aborts the live upload through the
same owner; a replacement connection does not inherit it.

### Usage Example

For a one-block image, the authorized non-ISR upload owner supplies a `{0}` OTA
handle, a prepared provider/configuration and a verified manifest whose image
size fits `max_chunk_bytes`. `image` contains exactly that many bytes. The fresh-ID
allocator, provider, optional guard/clock and distinct last-chunk buffer remain
alive until deinit succeeds. Flash consumers satisfy the admission and quiescence
requirements above before calling this helper.

```c
#include "esy_middleware/ota/esy_ota.h"

esy_err_t install_one_block(esy_ota_t *ota, const esy_ota_config_t *config,
                            const esy_image_manifest_t *manifest,
                            const uint8_t *image) {
    esy_image_status_t status;
    esy_err_t result = esy_ota_init(ota, config);
    if (result != ESY_OK) return result;
    result = esy_ota_begin(ota, manifest, &status);
    if (result == ESY_OK) {
        result = esy_ota_write(ota, status.session_id, 0u, image,
                               manifest->image_size, &status);
    }
    if (result == ESY_OK) {
        result = esy_ota_finish(ota, status.session_id, &status);
    }
    if (result == ESY_OK) {
        result = esy_ota_activate(ota, status.session_id, &status);
    }
    esy_err_t cleanup = esy_ota_deinit(ota);
    return cleanup != ESY_OK ? cleanup : result;
}
```

Larger images use sequential bounded writes with the returned session ID and
next offset before FINISH. An error may follow storage or activation effects;
reconcile authoritative state before retrying. Retain the handle and backing
resources when cleanup fails. Reboot and subsequent startup confirmation remain
explicit owner decisions; this helper performs neither.

## Contracts

### Session identity and writes

The owner supplies a nonzero fresh session ID. Its allocator must avoid reuse
while stale requests can remain, including across connection rebinds; an ID is
correlation data and provides no authentication. A matching BEGIN manifest while
RECEIVING or READY returns the existing session. A different manifest is BUSY.

WRITE accepts only `next_offset`, or exactly the immediately preceding block's
offset, length and bytes. Changed repeats, older blocks and future offsets return
STATE without provider writes. Empty/oversized blocks return INVALID_ARG. The
cached final block can also be replayed in READY/ACTIVATED without reopening the
upload. Duplicates do not extend deadlines. `next_offset` counts accepted bytes;
a provider may still hold some in RAM until FINISH. A partially failed provider
write terminates the session instead of retrying its effects.

FINISH requires all image bytes and enters READY only after successful flush and
verification. READY retains the provider, activity guard and deadline until
ACTIVATE or ABORT. ACTIVATE runs once. If persistent activation succeeds but
releasing the guard fails, the phase remains ACTIVATED with an error result and
the guard is retained for cleanup retry. A subsequent ABORT cannot undo an
ACTIVATED image. The generic core can accept another session after complete
terminal cleanup, subject to its provider's admission policy.

### Ownership and cleanup

One non-ISR task or main loop serializes every core/provider operation. Calls
are synchronous and callbacks must not reenter the handle. One core exclusively
borrows a provider; do not call that descriptor directly or share it with another
live core. Provider abort also handles partially failed BEGIN or activation and
must be idempotent. A callback error may indicate partial persistent effects.

Initialize handles with `{0}`; do not copy, move or clear a live handle. Request
and output objects, handle memory, last-block storage and provider backing memory
must be disjoint. Provider/clock/ID callback contexts, guard and buffers remain
alive until deinit succeeds. Abort or guard-release failure retains ownership
for checked abort/deinit retry; a new BEGIN cannot bypass retained resources.
Provider deinit follows successful core deinit, never the reverse.

### Timeout

The timebase is a continuously advancing `uint32_t` millisecond counter,
including during provider work; it wraps every approximately 49.7 days. Zero
disables timeout and permits a NULL clock. A nonzero timeout is at most
`INT32_MAX` milliseconds. While RECEIVING/READY, the owner observes the deadline
at intervals no greater than that timeout and prevents clock-stopping sleep.

BEGIN starts timing before guard/provider acquisition. BEGIN, accepted WRITE and
FINISH sample completion against the previous deadline before refreshing it.
Blocking erase/program/hash time is included. READY receives one fresh deadline
while waiting for ACTIVATE. STATUS, repeated requests and unrelated traffic do
not renew it; poll expires at the exact elapsed boundary using modulo-safe
subtraction. Terminal/aborted waits remain disabled across complete clock cycles.
ACTIVATE checks its deadline before entering persistent work. A successful
publication remains ACTIVATED even if that operation crosses the deadline; the
core does not report that published state as an unperformed timeout.

### Native Flash behavior

The caller-owned cache has nonzero capacity, at most `UINT32_MAX`, and is a
multiple of HAL program size. It can be smaller than an erase sector. BEGIN
snapshots the inactive slot and checks full Flash containment, program alignment,
final padding and ownership of complete physical erase sectors before accepting
work. The owner keeps layout/geometry fixed for the session; each erase also
checks its current sector. Readback hashing uses HAL reads, not mapped pointers.

FINISH verifies SHA-256 without writing boot metadata. ACTIVATE writes TRY_BOOT
metadata and then calls native image activation. These changes are not atomic.
Every activation attempt latches publication in the provider: later BEGIN returns
STATE even after ABORT, preventing overwrite of an image awaiting reboot or an
uncertain native boot target. The owner must reconcile persistent boot state
before explicitly deinitializing/reinitializing this provider; those lifecycle
calls do not themselves perform reconciliation. ABORT clears transient cache and
session state without erasing or rolling back boot records.

## Resource Model

All runtime objects and buffers are caller-owned; there is no allocator, global
current-session pointer, timer, worker or internal queue. `esy_ota_t` holds the
copied configuration, manifest, status, deadline and repeat bookkeeping. Declare
last-block memory with `ESY_OTA_STATIC_STORAGE` and bind it with
`ESY_OTA_BIND_STATIC_RESOURCES`; its capacity must cover `max_chunk_bytes`.

The native Flash provider uses one opaque `esy_ota_flash_t` and a separate
programming/readback cache. That cache cannot overlap the core's last-block
memory. Both stay borrowed until their respective successful deinit. An optional
`esy_activity_guard_t` is acquired before preparation and held through
RECEIVING/READY; terminal cleanup releases it with checked errors. The owner
provides all transport input/output storage separately.

## Verification

From this directory:

```sh
cd tests
ceedling test:all
ceedling gcov:all
python3 check_coverage.py build/artifacts/gcov/gcovr/coverage.json
python3 test_cmake_consumer.py
```

From the platform root, run the isolated sanitizer configuration and allocation
check:

```sh
ruby tools/quality/run_module_tests.rb esy_middleware/ota/tests esy_middleware/ota/tests/build/sanitizer test:all --sanitizer
bash tools/checks/check_no_dynamic_alloc.sh esy_middleware/ota
bash tools/checks/check_esy_common_contracts.sh
```

Core tests exercise exact repeats, IDs, partial failures, retained cleanup,
reentry, deadlines across wrap, disabled terminal waits and blocking provider
time. Native-provider tests cover geometry, nonuniform sectors, padding,
readback failures, separate verification/publication and publication admission.
CMake consumers compile/link/run C11 and C++17 public APIs, verify pure-core
closure and illegal feature combinations, and run an interrupted-update probe
against the real bootloader with existing HAL fakes. Use consumer firmware checks
to exercise the actual layout, Flash execution safety and product scheduling policy.

## Known Limits

- SHA-256 detects image corruption; neither this core nor the native provider
  authenticates signatures. The product/native bootloader owns trust keys,
  signature admission and anti-rollback policy.
- There is one sequential session with one-block replay memory, no out-of-order
  upload, persistent resume, multi-image transaction or power-loss rollback.
- Native metadata and boot records are separate writes. Publication/confirmation
  failures require owner reconciliation; metadata alone cannot establish native
  acceptance. Reset is always an owner decision.
- Flash operations can stall execution or mask interrupts. Execution budgets,
  concurrent task policy and watchdog servicing depend on the selected hardware
  and consumer scheduling policy.
- Boot confirmation belongs to
  [`esy_bootloader_mark_success`](../bootloader/README.md); the bootloader has no
  dependency on this OTA engine.
