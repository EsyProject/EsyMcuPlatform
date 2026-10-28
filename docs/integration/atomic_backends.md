# Atomic Backends

## Purpose

Describe the build and concurrency contract for 32-bit C11 atomics.
Device references, Wi-Fi requests/snapshots and LVGL lifecycle/submission share
this capability without introducing another atomic API or runtime allocation.

## Public Surface

Public interfaces use C11 atomic storage with matching C++ storage
representations. `esy_atomic_defs.h` checks the configured backend in C
implementations and exposes no runtime API. RP2040 uses SDK IRQ-spinlock
operations: maskable ISR operations may briefly spin while another core
completes an atomic operation.

## Integration

The EsyMcuPlatform entry CMake probes the configured C compiler and flags. It
rechecks the probe on every configuration, ignoring previously cached answers.
Native ports must report `ATOMIC_INT_LOCK_FREE == 2`. If that check fails, only
RP2040 with a supplied SDK `pico_atomic` target selects the IRQ-spinlock backend;
other unsupported toolchains fail during configuration.

`ESY_ATOMIC_IRQ_SPINLOCK_BACKEND` is derived backend metadata, not a feature
option. The common types target propagates it without any hardware dependency.
The private Pico HAL backend explicitly links the SDK runtime and compiles checks
for hardware spinlocks and separation from RTOS/other SDK lock owners. A private
HAL backend ABI shim maps GCC's `__atomic_test_and_set` calls to the SDK's
`__atomic_test_and_set_c`, retaining the SDK IRQ-save/restore and spinlock
semantics. Common and middleware contain no SDK includes, RTOS waits or board
lock configuration.
Direct module consumers retain the native requirement unless they supply all
of this verified assembly. Defining the backend macro alone is unsupported.

The SDK compatibility baseline is Pico SDK 2.1.1. SDK upgrades must retain the
primitive semantics. Target builds check runtime linkage, and the procedures
below cover hardware concurrency.

## Contracts

Native ports use always-lock-free integer operations. RP2040 SDK primitives
save and mask local interrupts before taking the reserved hardware atomic lock;
unlock restores the previous interrupt state. This prevents a maskable ISR on
the same core from preempting a lock owner. Both cores serialize through that
hardware lock. Atomic operations do not call user callbacks or the scheduler.

Contention is a spin wait with interrupts masked, not a lock-free or wait-free
operation. A fixed worst-case ISR time is not promised. Keep all users of the
SDK atomic lock short; do not use it directly, nest it with another lock, or
hold it across blocking work, logging, allocation or callbacks. Large unrelated
SDK atomic objects can also increase contention and must be audited.

NMI/fault handlers must not use this backend. Do not halt/reset a participating
core while it owns the atomic lock. Device graph lifecycle remains single-owner;
its ISR release path operates on an immutable live graph. Wi-Fi APIs remain
task-only. LVGL object access remains exclusive to the GUI execution context;
the backend does not relax resource ownership or shutdown rules.

## Resource Model

Reuse the SDK-reserved atomic lock. The port checks that it is a valid hardware
lock and does not alias SDK IRQ, timer, hardware-claim, random or RTOS locks,
or enter the shared striped/dynamically claimed lock ranges.
Consumers must not claim it for unrelated work. No application task, lock
object, heap allocation or board-specific resource is introduced.

## Verification

Run the backend integration tests, common public-header checks and affected
module suites. Build RP2040 baremetal/minimal OS and USB/UART OTA consumers,
Pico W TCP/Wi-Fi, and Pico 2/STM32 consumers. Verify 32-bit atomic load/store,
CAS, fetch operations and flag operations resolve to the selected runtime.
Changing the target/compiler in a cached build must not retain backend metadata.

On hardware, qualify task/ISR and dual-core contention, nested IRQ behavior,
restoration of an already-masked IRQ state, and reset/halt exclusions. For OTA,
measure atomic/interrupt latency, watchdog behavior and Flash-safe-execute
coordination under transport/DMA load. GUI ports additionally exercise concurrent
submission, stop and physical DMA completion. Record measurements with the
selected board, SDK, compiler and workload.

## Known Limits

RP2040 operations can spin on another core; there is no portable worst-case
contention bound. Timing depends on the participating cores, interrupt load and
SDK atomic-lock users. Product board wiring, RAM budgets, transport recovery and
display integration remain consumer-owned.
