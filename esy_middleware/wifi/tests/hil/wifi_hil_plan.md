# Wi-Fi Hardware Qualification

This plan describes target test procedures for RF, SDK, IRQ/DMA, power and Flash
behavior beyond host-test scope. Run with the consumer board/SDK/toolchain
combination in use, including Pico W and Pico 2 W when comparing both. HAL backend
unit tests with mocked vendor calls are outside this plan's scope.

Record board, SDK/provider version, native configuration, firmware revision,
compiler, feature inputs, poll period, task/core ownership and observed timing.
Do not include real credentials in public logs or test artifacts.

1. Connect to open and supported WPA/WPA2 networks. Exercise invalid credentials,
   unavailable AP, no DHCP response, delayed DHCP, lease/address change, loss of
   link and restored AP. Check state, address and error snapshots against real
   network observations. Confirm timeout and product retry pacing.
2. Supply caller-owned temporary credentials to manager init, reclaim those
   input strings, then connect and reconnect. Check the manager's owned copy.
   Verify exact maximum lengths and rejected oversized configuration.
3. Inject SDK start/init failure using the platform's available fault mechanism.
   Check hardware/network/SDK allocations after failure, retry, stop and deinit.
   Confirm no retained callbacks access released resources after deinit returns.
4. Register a Wi-Fi device; test idle runtime suspend followed by a new request,
   connection-preserving intent over system suspend/resume, prepare rollback,
   and a request during suspension. Verify backend shutdown/restart on the
   SDK-supported core. Measure callback latency and current draw. If the backend
   cannot satisfy a power-state contract, do not advertise that state.
5. Stop network clients before Wi-Fi stop or graph teardown. Verify usage counts,
   successful registry deinit, typed detach, manager deinit and a fresh lifecycle.
   Exercise repeated init and attempted second owner; original connection and
   device association must survive rejection.
6. Run request/query producers with the processing owner and PM owner on the
   consumer's permitted task/core placement. Confirm polling liveness and no
   state corruption. Check IRQ/DMA inactivity after suspend/deinit, including
   late completion and wakeup. Snapshot APIs are task-only.
7. Exercise configured Flash/OTA operations with the product's network policy.
   Verify execution-from-Flash restrictions, bounded task quiescence when needed,
   watchdog service and network/OTA recovery. If OTA or a mechanism is absent,
   record the configuration-based reason it is not applicable.
