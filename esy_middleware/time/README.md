<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# UTC time and SNTP

## Purpose

Provide an allocation-free UTC clock and a caller-driven SNTP exchange for
logging, collection schedules and other consumers of calendar time. The core
uses an injected monotonic microsecond clock and supports both baremetal and
RTOS callers without creating a task. UTC calendar conversion lives in
`esy_common/utils/time/esy_datetime.h`; hardware/calendar RTC providers live in
`esy_hal/esy_hal_rtc.h`.

## Public Surface

| Header | Target | Operations |
| --- | --- | --- |
| `esy_middleware/time/esy_time.h` | `esy_time` | Initialize/deinitialize, set/read UTC, invalidate, query validity |
| `esy_middleware/time/esy_sntp.h` | `esy_sntp` | Start/process/cancel a single UDP exchange, query its outcome |
| `esy_middleware/time/esy_time_rtc.h` | `esy_time_rtc` | Load UTC from an RTC or store current UTC in it |

UTC values use normalized `esy_timestamp_t` from `esy_common/defs/base/esy_time_defs.h`,
independent of libc `time_t`. The core covers Unix seconds zero through
9999-12-31 23:59:59 with nanosecond fractions; elapsed time has microsecond
resolution. Leap seconds and local timezone/DST conversion are outside the API.

## Integration

The consumer defines feature values in its feature import before adding the
platform. `ENABLE_ESY_TIME` adds `esy_time`; `ENABLE_ESY_TIME_SNTP` additionally
adds `esy_sntp`. Link only the targets needed by the caller. Neither target
requires HAL, an OS target or a networking stack. SNTP borrows an initialized
UDP transport provider that pins one server address and port, normally 123.

`ENABLE_ESY_TIME_RTC` adds `esy_time_rtc` and requires `ENABLE_ESY_RTC`.
The bridge borrows an initialized RTC facade during each call. Load seeds UTC
from the RTC's calendar; store writes current UTC and discards fractional
seconds. Both operations are synchronous. Reading/writing an RTC can change
its alarm behavior according to the selected provider. The bridge does not
initialize either handle or supply persistence, clocks, backup power or board
policy.

For SNTP, initialize `esy_sntp_t client = {0}` with the clock, its monotonic
provider and bounded transport callbacks. Call `esy_sntp_start()` with a finite
microsecond timeout and a fresh, unpredictable nonzero 64-bit request token.
Call `esy_sntp_process()` from the owning main loop/task until status leaves
`ESY_SNTP_WAITING`. A successful exchange sets the UTC service once. The owner
may then explicitly call `esy_time_rtc_store()` to update an RTC. Inspect errors
and apply server selection, backoff and retry policy in the consumer.

### Usage Example

Supply a monotonic-clock configuration and a trusted UTC anchor corresponding
to the time of the `esy_time_set` sample. This synchronous example initializes
a local clock, sets UTC, reads it and releases the provider before returning.
The provider context stays alive throughout the call; use the output only when
the function returns `ESY_OK`.

```c
#include "esy_middleware/time/esy_time.h"

esy_err_t time_example_read(const esy_time_config_t *config,
                           const esy_timestamp_t *anchor,
                           esy_timestamp_t *current)
{
    esy_time_t clock = {0};
    esy_err_t result = esy_time_init(&clock, config);
    if (result != ESY_OK) return result;
    result = esy_time_set(&clock, anchor);
    if (result == ESY_OK) result = esy_time_now(&clock, current);
    esy_err_t cleanup = esy_time_deinit(&clock);
    return cleanup == ESY_OK ? result : cleanup;
}
```

For repeated reads, keep the handle in the owner's persistent context, establish
the anchor once, then call `esy_time_now` as needed. Stop SNTP/other borrowers
before successful deinit releases their clock dependency.

## Contracts

One task/main-loop owner serializes all operations on each handle and its
borrowed providers, including status queries. Calls are not ISR safe. Never
copy, move, clear or alter a live handle, and keep borrowed providers/contexts
alive until deinit succeeds. Initial handles require `{0}`. Repeated init is
rejected; failed init leaves the handle unchanged. Deinit is synchronous and
idempotent for the zero state, invokes no providers and permits reinit.

The unsigned 64-bit monotonic counter wraps modulo 2^64 microseconds, about
584,542 years. It must not reset, run backwards or change rate during use.
It must progress across sleep when UTC and timeout should progress there.
UTC anchor-to-read intervals must be below 2^63 microseconds, and UTC range
exhaustion is an error. SNTP timeout is 1..UINT32_MAX microseconds and includes
provider runtime. At elapsed >= timeout, timeout wins. Process must observe an
active wait before a full 2^64-us cycle; polling more frequently than the
timeout gives useful responsiveness. SNTP and UTC providers must observe the
same counter epoch and rate. The final UTC anchor sample is checked before
committing, so a timeout preserves both the previous anchor and its validity.
Deadline decisions use the sampled counter value; clock providers should sample
near return and account for any remaining latency in the consumer's budget.
Completed, failed and cancelled waits remain disarmed through any later counter
value until an explicit new start.

Each process call performs at most one receive and bounded packet processing.
All providers run synchronously; their execution time and caller scheduling
latency contribute to response time and synchronization error. The receive
callback returns `ESY_ERR_BUSY` if no datagram is immediately ready, reports
the original length for oversized datagrams and rejects other endpoints.
Other provider errors terminate the attempt. Malformed/unmatched packets
return `ESY_ERR_CRC_FAIL` and keep the existing wait active; matched stratum-zero
responses terminate it with `ESY_ERR_NOT_SUPPORTED`, including kiss-of-death
with an unsynchronized leap indicator (LI=3). Length, version, server mode and
originate token are checked before handling these control responses.
Cancel/deinit cannot retract an already transmitted packet. Tokens must never
be reused, including across failure or reinitialization; immediately repeating
the last token is detected. A subsequent attempt ignores stale replies with
different tokens.

For time synchronization, SNTP requires an exact 48-byte NTPv4 server-mode
response, synchronized leap indicator, stratum 1..15, matching originate token
and nonzero receive/transmit timestamps. This deliberately omits extension/MAC
processing. Server seconds
are decoded into the fixed Unix 0..UINT32_MAX interval (1970-01-01 through
2106-02-07 06:28:15 UTC), including the 2036 NTP era rollover. An all-zero NTP
timestamp is unavailable, including the exact first instant of that rollover.
Fractions are truncated below one nanosecond. Server transmit must follow
receive, and server processing must fit within measured monotonic elapsed time.
The resulting UTC is server transmit plus half the network round trip. When
initial UTC is valid within that era window, status also reports the equivalent
four-timestamp offset; otherwise offset is explicitly unavailable.

The packet layout and offset/delay calculation follow
[RFC 4330](https://www.rfc-editor.org/rfc/rfc4330) and
[RFC 5905](https://www.rfc-editor.org/rfc/rfc5905). The request uses an opaque
correlation token, with local timestamps kept separately, following the
[NTP client data minimization proposal](https://datatracker.ietf.org/doc/html/draft-ietf-ntp-data-minimization-04).
The transport owner should choose a randomized ephemeral source port as
recommended by [RFC 9109](https://www.rfc-editor.org/rfc/rfc9109).

## Resource Model

Each caller owns one fixed-size opaque handle, plus its existing clock and UDP
provider state. No external workspace, hidden singleton, heap allocation,
socket allocation, callback registration, worker, queue or timer is created.
SNTP uses one 48-byte packet on the stack per start/process call. Providers
must not retain its address or recursively access the handle. Their own memory,
blocking and network-stack requirements remain the transport owner's contract.

## Verification

From `tests/`, run `ceedling test:all` and `ceedling gcov:all`. Public-API tests
cover first/repeated/failed lifecycle, UTC range/normalization, provider errors,
counter wrap, timeout edges, maximum timeout, completion/cancel disarming,
explicit restart, final-anchor timeout with previous UTC preservation,
LI=3 kiss-of-death and unmatched control replies, NTP fractions/era/offset/delay,
packet validation and RTC load/store. `test_cmake_consumer.py` checks C/C++ header
and target integration,
including configurations with no HAL/OS targets for the core. The repository's
module-test runner applies `tests/sanitizer.yml` for address/undefined behavior
checks. These deterministic tests do not establish network accuracy or native
RTC retention and oscillator behavior. Consumer HIL procedures measure those
properties for the selected clock and network providers.

## Known Limits

UTC steps immediately and can move backwards; use monotonic time for timeouts.
Validity records an established anchor, not freshness, synchronization quality
or provider health. There is no clock slewing, drift estimator, automatic
resynchronization, DNS, server failover, authentication, NTS, broadcast mode or
full NTP filtering. SNTP's fixed era window deliberately differs from the
larger UTC core range. Endpoint pinning and unpredictable tokens do not
authenticate a server or protect against an on-path attacker. Accuracy depends
on network symmetry, polling and synchronous provider latency. Firmware pause,
OTA, sleep and watchdog coordination belong to the execution/transport owner;
the module has no independent task or ISR to stop.
