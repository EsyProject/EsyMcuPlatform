<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Pico Wi-Fi Network Runtime HIL Plan

## Purpose and Setup

Verify native network progress and core-region hooks on Pico W and Pico 2 W
with the real polling CYW43/lwIP provider. Record the MCU, SDK/compiler versions,
feature inputs and firmware revision. Enable Wi-Fi through the consumer feature
import and use one SDK-supported, serialized, non-ISR owner for station control,
network endpoints, hooks and shutdown. Keep network storage alive until all
borrowers and native callbacks have retired. The hooks do not make endpoints
safe for concurrent use by multiple tasks.

## Procedure

1. Before starting Wi-Fi, call `esy_hal_wifi_get_network_runtime()` into a
   caller-owned descriptor. Require `OK`, three non-NULL callbacks and a NULL
   context. Use debugger breakpoints or a dedicated SDK trace to verify that
   the query performs no native init, allocation, poll or core-region operation.
   A NULL output must return `INVALID_ARG`. Calling each hook before native
   init must be harmless and must not enter the corresponding SDK operation.
2. Start the station through the existing owner. Copy the descriptor callbacks
   and context into the network endpoint's explicit progress/core-region hooks.
   Do not include a Pico SDK header in the middleware borrower. Exchange TCP
   data with a host and require normal peer acknowledgements and input dispatch.
   Trace each explicit progress call to one `cyw43_arch_poll()` invocation and
   each paired enter/leave to the native begin/end operations. Native callbacks
   may run synchronously; record progress and callback latency under traffic.
3. During a pending station attempt, invoke only the network progress hook for
   a measured interval. Require no new station-connect submission or replacement
   deadline from those hooks. Then resume the normal station process owner and
   verify its original timeout/retry policy still governs the attempt. Repeat
   with a connected station: hooks must not run the station manager or change
   its cached state as a side effect. Native link changes may still be observed
   when the station owner next processes the provider.
4. Stop new TCP work, finish endpoint cleanup and retire all borrowed callbacks
   before Wi-Fi stop/deinit. Require that no endpoint calls native APIs after
   teardown. With no borrowers attached, invoke the saved hooks after deinit;
   they must defensively return without SDK entry. This no-op is not permission
   to retain an active network borrower beyond provider shutdown.
5. Reinitialize the station, reattach freshly initialized endpoints and repeat
   traffic and shutdown. Querying the descriptor again must not initialize or
   restart the station. Verify repeated query leaves accepted credentials,
   connection-attempt timing and native resource ownership unchanged.

## Evidence and Limits

Preserve native call counts, owner/core identity, station-connect submissions,
deadline observations, TCP traces and cleanup order. Host HAL forwarding tests
validate descriptor/error propagation; source-contract checks validate the
guarded SDK wrappers. Neither establishes real provider timing, cross-core
safety or hardware shutdown.
