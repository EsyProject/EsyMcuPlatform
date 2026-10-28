<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Bluetooth LE

## Purpose

Provide portable Bluetooth LE GAP, GATT server and security interfaces with an
injected software-stack provider. The core validates inputs and lifecycle order;
the provider implements radio procedures, attributes, authentication and durable
bond storage. This module does not implement a Bluetooth Host or Controller.

The core has no third-party stack, HAL or OS dependency. It can be linked into an
OS application or a baremetal application when the selected provider supports
that execution model. There is no built-in task or polling loop.

## Public Surface

Include `esy_middleware/ble/esy_ble.h` and link `esy_ble` or `Esy::ble`.
The optional adapter uses `esy_middleware/ble/esy_ble_btstack.h` and
`esy_ble_btstack` / `Esy::ble_btstack`; its public header hides native types.

| Surface | Behavior |
| --- | --- |
| `esy_ble_init`, `esy_ble_deinit` | Bind an exclusive provider; stop and release it. |
| `esy_ble_adv_start/stop`, `esy_ble_scan_start/stop` | Legacy advertising and continuous scanning. |
| `esy_ble_connect`, `esy_ble_disconnect` | Initiate and terminate LE connections. |
| `esy_ble_gatt_register/unregister` | Register caller-owned services and characteristics with ATT read/write callbacks. |
| `esy_ble_notify` | Notify one subscribed connection using its local characteristic value handle. |
| `esy_ble_security`, `esy_ble_auth_reply` | Request security and answer passkey, numeric comparison or pairing requests. |
| `esy_ble_bonds_foreach`, `esy_ble_unpair` | Enumerate peer identities and remove one or all persisted bonds. |
| `esy_ble_ops_t` | Inject the software stack; optional operations can be unsupported. |

Events cover scan results, connection/disconnection, security and pairing results,
passkey display/entry/confirmation, pairing confirmation and authentication
cancellation. Event objects and scan payloads expire when the callback returns.

## Integration

Set `ENABLE_ESY_BLE` in the consumer's `esy_feature_import.cmake` and link the module.
No other feature is enabled automatically. Disabling the module assembles neither
its sources nor any protocol-stack dependency.

```cmake
# In the consumer's feature input:
set(ENABLE_ESY_BLE ON)

# After platform assembly:
target_link_libraries(application PRIVATE Esy::ble)
```

The consumer supplies a provider descriptor containing a constant operation table
and an exclusive context. Its adapter translates the portable definitions and
events into the selected stack's public software API. No stack-specific types
belong in `esy_ble.h`. Hardware transport and RTOS integration remain within the
applicable HAL/backend and OS boundaries; the software adapter must not directly
include hardware SDK or raw RTOS headers.

Initialize the core handle to `{0}` before its first use. The descriptor is
copied; its operation table, exclusive provider context and event callback state
remain borrowed until successful deinit. Copying a descriptor does not create a
new context. Never bind multiple core handles to the same resource-owning context.
A shared native stack needs distinct binding tokens so a rejected owner can be
cleaned up without affecting the active owner.

Integration sequence:

1. Configure the provider's controller/HCI transport, scheduler, fixed capacities,
   pairing IO capabilities and persistent storage. The provider documents which
   resources it owns and which initialized resources it borrows.
2. Call `esy_ble_init` with the descriptor and an event sink. The provider restores
   durable identities/keys before returning success. Even failed init requires
   `esy_ble_deinit`; retain all borrowed resources until cleanup succeeds.
3. Register GATT services, then start GAP procedures from the serialized owner.
   Read/write properties require callbacks and security levels. Notification-only
   characteristics also specify `notify_security`; the provider checks it for
   CCC writes and sends.
4. Copy incoming events to consumer-owned synchronization/storage as needed.
   Answer authentication requests from the owner, with no automatic acceptance
   policy imposed by this module.
5. Stop application activity and call `esy_ble_deinit`. A failed stop retains the
   binding and all borrowed state for cleanup retry only; control calls then
   return `ESY_ERR_STATE`. Success quiesces callbacks, releases
   services and resets their assigned handles. The core handle can be reused.

The provider must document its integration/version contract and provide actual
controller, scheduler and persistence configuration. Selecting this core alone
does not turn Bluetooth on.

### Usage Example

The caller supplies a detached `esy_ble_t` initialized with `{0}`, a configured
provider/runtime, an event sink and validated advertising parameters. Call these
functions from the serialized owner. Keep the handle, provider and event context
alive until `ble_example_stop` succeeds; retry cleanup failures before reuse.

```c
#include "esy_middleware/ble/esy_ble.h"

esy_err_t ble_example_start(esy_ble_t *ble,
                          const esy_ble_provider_t *provider,
                          esy_ble_event_fn events, void *event_context,
                          const esy_ble_adv_params_t *advertising)
{
    esy_err_t result = esy_ble_init(ble, provider, events, event_context);
    if (result == ESY_OK) result = esy_ble_adv_start(ble, advertising);
    if (result == ESY_OK) return ESY_OK;
    esy_err_t cleanup = esy_ble_deinit(ble);
    return cleanup == ESY_OK ? result : cleanup;
}

esy_err_t ble_example_stop(esy_ble_t *ble)
{
    return esy_ble_deinit(ble);
}
```

Success means advertising was accepted; the provider still owns stack/radio
progress. Process copied events in the owner and apply the product's pairing
policy there. Deinit also stops advertising and quiesces registered callbacks.

### Optional BTstack software adapter

Set `ENABLE_ESY_BLE_BTSTACK` alongside `ENABLE_ESY_BLE` and provide an existing
`esy_ble_btstack_provider` target with BTstack's software library, native headers
and compatible `btstack_config.h`. This dependency is private to the adapter;
the public core and adapter headers remain independent of native types.

The shared `btstack_config.h` must explicitly define
`NVN_NUM_GATT_SERVER_CCC` as `0` when compiling every native provider source,
including `att_server.c`, and the adapter. This disables BTstack's automatic
CCC TLV writes and restoration, keeping subscriptions connection-local and
preventing ordinary ATT subscription writes from invoking native persistent
storage. Defining the macro only for the adapter target does not configure the
native library. Missing or nonzero settings are rejected during adapter
compilation. `ENABLE_GATT_OVER_EATT` is unsupported and must remain undefined;
the adapter uses the fixed ATT bearer's MTU.

The compatibility baseline is BTstack 1.6.2 at commit
`501e6d2b86e6c92bfb9c390bcf55709938e25ac1`, as included in Pico SDK 2.1.1.
No upstream source is copied, downloaded or relicensed here. BTstack retains its
own licensing terms, including any applicable SDK supplemental license; this
module's Apache-2.0 license does not relicense that dependency.

The consumer initializes native pools, run loop, HCI transport, L2CAP, SM and
LE device DB, then brings HCI to `HCI_STATE_WORKING`. Construct an initially
zeroed `esy_ble_btstack_t` and a config, then call `esy_ble_btstack_provider` to
obtain the descriptor for `esy_ble_init`. The adapter borrows this host and
exclusively owns ATT/GAP/SM control while attached. It never powers HCI or
initializes a physical transport. Do not attach alongside another ATT server.

Provide an `esy_ble_btstack_static_resources_t` aggregate containing the attribute
database buffer, service pointer slots, link slots, per-link subscription slots
and a `read_buffer` with `read_buffer_size` in `1..UINT16_MAX`. All buffers are
required, remain exclusively borrowed until successful core deinit, and need not
be cleared. Set capacities for the actual consumer; the link capacity must cover
the native maximum simultaneous connections. The adapter token must start with
`{0}`. Attribute schema capacity failures leave public definitions unchanged.

The read buffer holds one dynamic value slice while the serialized native ATT
length probe prepares a response. Each probe requires space for
`min(total_length - offset, fixed_ATT_MTU - 1)` bytes; insufficient capacity
returns `ATT_ERROR_INSUFFICIENT_RESOURCES`. Provision at least the largest
negotiated fixed ATT MTU minus one to support every value slice. The adapter
queries the total length, reads that slice into the buffer, then returns either
the total length or an ATT error to the native probe. The native copy phase only
copies the validated snapshot and does not invoke application code. Application
reads must have no destructive side effects: probes may prefetch slices that
are later discarded. A successful slice must return exactly
`min(capacity, total_length - offset)` bytes consistent with its length query;
inconsistent lengths return `ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH`.

All adapter API calls, native events, ATT callbacks, persistence hooks and
lifecycle operations must run in the same serialized, non-ISR BTstack context.
The consumer arranges that context through its native port; linking a stack
library alone does not establish this guarantee. Calls from unrelated application
tasks must be handed to that owner. Native persistence hooks may perform bounded
storage I/O and contribute to host latency. Include these hooks in Flash/OTA
coordination and worst-case timing measurements. Event sinks and application ATT
callbacks remain short and nonblocking.

Persistence hooks are mandatory: `restore` populates the initialized native
device DB before init succeeds; `flush` commits/checks native changes and reports
durability errors. An eagerly persistent TLV backend may use `flush` to report
latched write failures. The adapter chooses no physical storage or Flash layout.
Configuration fixes IO capabilities, bondability and the minimum security policy;
unsupported security upgrades must fail explicitly.

The host owner continues pumping BTstack during shutdown. The adapter requests
GAP stop, connection cancellation and disconnects; `host_quiesce` returns OK only
when those controller procedures are complete and no new connection can appear.
BUSY asks the owner to continue native progress and retry deinit. All buffers and
callback state remain borrowed during that period. Successful shutdown detaches
callbacks and ATT and clears service handles; the borrowed host stays initialized.
BTstack retains some library-owned static ATT dispatch registrations. The adapter
replaces their database and application callbacks with permanent empty/NULL
values before releasing caller storage; reinit does not duplicate registrations.

## Contracts

All core control/lifecycle calls belong to one serialized, non-ISR owner. Event
and ATT callbacks use provider-native non-ISR contexts, possibly concurrent with
the owner or each other. Callbacks must be short, nonblocking and must not invoke
core control/lifecycle APIs. Consumers provide bounded handoff and synchronization
appropriate to the provider. No global lock or hidden queue is installed here.

`init`/`deinit` are mandatory provider operations. Other NULL operations return
`ESY_ERR_NOT_SUPPORTED`. Invalid public arguments return `ESY_ERR_INVALID_ARG`;
control calls on detached or failed-init handles return `ESY_ERR_STATE`. Provider
errors pass through unchanged. A successful asynchronous control request means
accepted, not completed or acknowledged by the peer. Repeated start/stop statuses
follow the selected provider. `deinit` on an already detached handle succeeds.

Advertising inputs are encoded `[length, type, value...]` structures, at most
31 bytes for each of advertising data and scan response. A zero terminator may
only precede zero padding. The provider consumes/copies these buffers before
returning. Intervals use the Bluetooth units documented in the public types.
Scanning is continuous until explicitly stopped. Connection supervision timing
must exceed twice `(latency + 1) * interval_max`; equality is invalid.

This core measures no elapsed time and maintains no deadlines or retry counters.
Radio intervals are 16-bit protocol parameters, not local timestamps. Providers
own scan/connect/controller timeout clocks, wraparound and suspend semantics and
must document their native scheduling requirements and stopping guarantees.

Connection IDs expire at disconnect and can be reused; discard queued requests
for previous sessions. Authentication requests must also be discarded after
cancellation. Addresses and UUIDs are encoded least significant octet first;
UUID length is exactly 2 or 16 bytes.
Pairing completion/failure also expires pending authentication. The owner must
invalidate outstanding UI work before processing another request on the same
connection; this interface provides no authentication generation token.

GATT definitions and callback state remain at stable addresses until successful
unregister or deinit. Initialize service/characteristic handles to zero. Successful
registration assigns local handles atomically; failure leaves the definitions
unchanged and releases temporary provider resources. The provider validates
service identity on unregister and quiesces its ATT callbacks before success.
Unregister failures retain the live definitions and handles for retry. Read
callbacks return an ATT status and the number of bytes in the requested slice;
write callbacks return an ATT status, with no response possible for commands.
A read with NULL buffer, zero capacity and zero offset queries total value length
without copying data; this supports providers that probe length before reading.
Notifications do not imply delivery confirmation or automatic fragmentation.
The BTstack adapter rejects notification payloads larger than the connection's
fixed ATT MTU minus three with `ESY_ERR_INVALID_ARG`, before calling the native
send operation. A zero-length notification remains valid.

Bond persistence belongs to the provider and its configured storage. Restore
keys before enabling operations, propagate storage failures and erase durable
records when unpair succeeds. This module does not expose key material or choose
Flash addresses, partitions, encryption policy or OTA behavior. A provider must
coordinate its Flash operations with the consumer's resource owner.

## Resource Model

The core owns no static runtime state or dynamic memory. Each caller owns its
`esy_ble_t`; providers own their context/capacity configuration. GATT arrays and
callback state are borrowed. Transient advertising and notification buffers are
consumed/copied before return. Native stack pools, tasks, timers, key databases
and controller transport are outside the core's memory budget.

Providers may create native resources only through documented lifecycle/control
operations. They must declare internal allocation, maximum capacities, blocking
and cleanup behavior. This core's lack of allocation does not imply that an
injected stack is heap-free or hard real-time. Successful deinit must finish all
asynchronous access before borrowed memory is released; cleanup failure cannot
be bypassed by clearing the handle.

## Verification

From `tests/`, run `ceedling test:all`, `ceedling gcov:all`, and
`ceedling --mixin=sanitizer.yml test:all`. Public API tests exercise a deterministic
module-local provider, including invalid arguments, lifecycle failure/retry,
operation forwarding, callbacks, GATT definitions and security/bond operations.
They verify core contracts and do not certify a real BLE stack or wireless link.

Run `python3 -B tests/test_cmake_consumer.py` from this directory for actual C11
and C++17 compile/link/execute checks, feature on/off closure, private option
isolation and integration without HAL, OS or protocol-stack targets.

Controller-backed integration procedures exercise advertising,
active/passive scanning, central/peripheral links, ATT read/write/CCC/notify,
security rejection, authentication cancellation, bonding across restart and
durable unpair, resource exhaustion, stack shutdown and Flash/OTA coordination
in the consumer configuration. No hardware or controller simulation
result is implied by the core tests.

With an explicit upstream checkout, run the native-header and adapter runtime
checks without downloading dependencies:

```sh
python3 -B tests/test_cmake_consumer.py --btstack-path /path/to/btstack
python3 -B tests/test_btstack_runtime.py --btstack-path /path/to/btstack
python3 -B tests/test_btstack_runtime.py --btstack-path /path/to/btstack --sanitize
```

The runtime harness compiles the production adapter against real upstream public
headers and substitutes deterministic host calls. Its additional native ATT mode
links upstream `att_db.c` to check real request decoding, error responses, read
snapshots and bounded notification lengths. Consumer compilation also rejects
missing/nonzero native CCC persistence configuration and unsupported EATT.
The consumer checks above measure controller timing and actual on-air behavior.
Omitting the explicit path skips the provider checks.

## Known Limits

The BTstack adapter requires a configured native Host/Controller and durable
storage integration; it does not add a complete board-specific Bluetooth runtime.
There is no implicit stack download or hardware selection.

The portable GATT server supports services, characteristics and generated CCC.
It does not cover GATT client discovery/read/write/subscription, arbitrary
descriptors, included services, indications or prepared/reliable writes. GAP
advertising is legacy only; extended/periodic advertising, multiple identities,
explicit privacy policy, PHY tuning, L2CAP channels, Classic Bluetooth, Mesh and
LE Audio are outside this interface. Unsupported native capabilities must return
`ESY_ERR_NOT_SUPPORTED`, rather than silently weakening security or behavior.

The BTstack adapter registers/unregisters services only while GAP is idle and no
links or pending connection exist. Unregister is LIFO and reclaims the removed
handle range while keeping existing handles stable. Its native write callback
cannot identify command versus request on a
characteristic exposing both write properties, so this combination is rejected;
each property is supported separately. Configured link/subscription/database
capacities and native buffer/MTU availability limit operations. Subscriptions
are cleared on disconnect and are not restored from durable bonds.

The native build must enable BLE central, peripheral and Secure Connections and
provide `MAX_NR_HCI_CONNECTIONS` for fixed link budgeting. Disconnect accepts the
native API's remote-user-terminated reason (`0x13`) only. The configured minimum
security policy is immutable while attached; higher unsupported requests fail.
Dynamic read lengths must be below BTstack's `0xfe00` error-encoding boundary.
Read snapshots require the caller-owned staging capacity and consistent,
nondestructive callbacks described above. EATT bearers are unsupported.

The adapter does not supply GAP/GATT profiles or implement Service Changed/cache
invalidation. Consumers must register the services required by their roles and
profiles and keep bonded-peer schema/handle caching consistent across restarts;
changing a persistent schema requires an explicit cache migration or removal of
the affected bonds before peers reconnect. No compatibility with a changed
schema is inferred from successful registration.
