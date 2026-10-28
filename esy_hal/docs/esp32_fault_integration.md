<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors
SPDX-License-Identifier: Apache-2.0
-->

# ESP32 Fatal Capture Integration

## Supported Provider

The private panic adapter supports the classic Xtensa ESP32 with ESP-IDF
**5.3.4**. Other targets and SDK versions fail compilation while fault capture
is enabled. Porting the adapter requires reviewing the native panic call site,
exception-frame layout and panic-memory placement before changing that guard.
It does not replace ESP-IDF's bootloader or exception vectors.

Enable `ENABLE_ESY_FAULT` in the consumer's existing feature input. Native SDK
discovery must provide `idf::esp_system`, `idf::esp_hw_support` and
`idf::xtensa`. Set `CONFIG_ESP_PANIC_HANDLER_IRAM=y` and leave
`CONFIG_COMPILER_LTO` disabled. Configuration rejects missing providers or
incompatible settings; the SDK version is additionally checked by the compiler.
The backend registers GNU linker options with ESP-IDF's final application link
properties to wrap `esp_panic_handler` and retain its wrapper. This also works
when a native consumer retains CMake 3.16 policies that do not propagate private
static-library link options. Do not install another wrapper for the same symbol.

## Native Panic Contract

ESP-IDF's architecture panic entry establishes its RTC watchdog and stalls peer
cores before calling the common panic handler. The adapter captures the first
snapshot at that boundary and then calls the original handler. Native hardware
exceptions, watchdog panics and aborts therefore use the same integration;
software fatal events deliberately enter it through `esp_system_abort()`.
See the provider's [panic entry](https://github.com/espressif/esp-idf/blob/v5.3.4/components/esp_system/port/panic_handler.c)
and [panic implementation](https://github.com/espressif/esp-idf/blob/v5.3.4/components/esp_system/panic.c).

When using the OS fatal bridge, the native app startup owner calls
`esy_os_init()` and then `esy_os_set_fatal_handler(esy_hal_fault_raise)` before
creating application-owned tasks. ESP-IDF has already started its scheduler at
`app_main()`, so the ESP32 binding intentionally accepts that state. The handler
is published once with atomic release/acquire operations for peer fatal readers.
Both HAL capture configuration and the OS fatal callback have one serialized
startup owner; the HAL copies the configuration before its release publication,
and peer panic readers use acquire loads. There is no runtime rebind or unbind,
kernel lock or scheduler call in OS dispatch.

The adapter uses the native panic stack. It does not claim Cortex-style private
emergency stacks or guarantee recovery from an already unusable native stack.
The snapshot resides in ordinary internal DRAM to avoid a large local object.
The entire capture call chain must reside in IRAM, with its context and ordinary
state in internal DRAM; it may write only consumer-reserved retained RAM.
No OS call, log, Flash operation, allocation, lock, FP operation or unbounded
wait is permitted during capture. Stack capacity and interrupt/peer behavior
must be verified on the actual SDK configuration.

The adapter validates the complete native frame range in internal DRAM before
reading it. It records Xtensa A0..A15, PC, PS, EXCCAUSE and EXCVADDR in the
architecture-specific public register slots. It copies no external-PSRAM stack
window. Invalid frames remain explicitly unsupported. ARM-specific registers
stay zero. A valid native `panic_info_t.core` identifies the faulting CPU; an
unavailable or invalid native core falls back to the CPU executing the adapter.
The [SDK frame definition](https://github.com/espressif/esp-idf/blob/v5.3.4/components/xtensa/include/xtensa_context.h)
and [memory helpers](https://github.com/espressif/esp-idf/blob/v5.3.4/components/esp_hw_support/include/esp_memory_utils.h)
define the private provider boundary.

The original panic handler retains ownership of UART diagnostics, an optional
native IDF core dump, reset/halt policy and debugger behavior. A debugger-aware
panic can return to the native entry; the adapter preserves that behavior.
The first-capture admission remains latched until reset. There is no runtime
diagnostic or resume API.

## Retained RAM And OTA

The consumer must reserve a fixed retained range shared by its bootloader and
every application image, including rollback images. SDK `RTC_NOINIT_ATTR` can
exclude an object from one image's initialization, but does not establish a
fixed address shared by independently linked images. Use consumer-owned linker
reservations and assert their addresses/sizes across all participating images.

For capture from either classic ESP32 core, choose memory accessible from both
cores. If using RTC noinit storage, select RTC slow memory; do not rely on RTC
fast memory that is accessible only to the PRO CPU on this chip. In particular,
review `CONFIG_ESP32_RTCDATA_IN_FAST_MEM` when placing SDK RTC sections.
Ordinary capture context remains in internal DRAM and holds the retained-range
pointer. See the SDK's [memory types](https://docs.espressif.com/projects/esp-idf/en/v5.3.4/esp32/api-guides/memory-types.html).

No retention claim covers power loss, brownout, an RTC-domain reset or a
bootloader/application pair without matching retained-memory reservations. The
native panic watchdog is a fallback reset mechanism and can invalidate assumptions
about the RTC domain.
Record behavior separately for each deployed reset source, OTA activation and
rollback path. A previous application without the reservation is incompatible
with an across-image retention claim even when its image signature is valid.

## Optional Flash Persistence

Capture writes RAM only. After reboot, a normal serialized owner may persist a
validated record using the public Flash HAL, with the usual native Flash and
watchdog budgets. Allocate a dedicated consumer-owned partition, disjoint from
`otadata`, factory/OTA images, filesystems and every other storage user. Resolve
its bounds within the board/backend boundary and inject the product's offset
and capacity into the persistence configuration. SDK partition headers must
not become coredump-middleware dependencies.

A custom data partition is distinct from ESP-IDF's `data,coredump` partition.
The latter belongs to the native IDF format when native core dumps are enabled;
the two writers must never share a partition. Preserve the original native
format if using IDF's host decoding tools. Encrypted/secure storage requires a
separate validated integration; CRC alone does not protect sensitive dump data.
See the [SDK partition-table contract](https://docs.espressif.com/projects/esp-idf/en/v5.3.4/esp32/api-guides/partition-tables.html)
and [native core-dump guide](https://docs.espressif.com/projects/esp-idf/en/v5.3.4/esp32/api-guides/core_dump.html).

## Verification

Compile/link validation used the official ESP-IDF v5.3.4 commit
`1b459d9c4950395dec12ce19256c73f9a41e306f` and its recommended Xtensa GCC
13.2.0 (`esp-13.2.0_20240530`) in a minimal native consumer. Fault capture and
the RAM coredump core were enabled, with Logger and Flash adapters disabled.
The native application and bootloader built successfully. Final disassembly
confirmed the native panic entry calls the wrapper, the wrapper calls the
original handler, and capture code/helpers reside in IRAM with adapter state
and software-abort text in internal DRAM. The fixture's 4 KiB RTC noinit buffer
was a link-placement check, not a cross-image retention validation.

An additional narrow native target compiled the production OS core wrapper
against `idf::freertos` and linked real OS init, fatal registration and assertion
calls. Its final ELF resolved the native scheduler's stack-overflow call to the
Esy strong hook, with hook/assert/dispatch in IRAM. Handler pointer loads and
stores compiled to inline `memw` plus `l32i`/`s32i`; HAL configuration publication
also used inline acquire/release operations. The inspected fatal closure had no
atomic helper, compiler/libc helper or Flash-literal dependency. This validates
the OS core integration, not the complete OS module assembly or hardware behavior.

For native integration verification, build the ESP-IDF consumer and inspect
the final ELF/map to confirm that the panic caller resolves to
`__wrap_esp_panic_handler`, the original handler is retained,
and the adapter/capture callees and constants are accessible without Flash.
Reject upgrades or LTO settings that bypass the wrapper.

Inject an illegal instruction, invalid load/store, native abort, OS assertion,
stack overflow and interrupt-watchdog panic on each core. Verify the Xtensa
register mapping, invalid/PSRAM stack handling, one-capture admission and
unchanged native panic output/reset or debugger behavior. Exercise simultaneous
fatal cores and an interrupted capture; record the RTC watchdog fallback.
Then verify retained bytes through reset, bootloader handoff, A/B activation and
rollback before testing normal-context Flash persistence. The compile/link
evidence above does not establish hardware exception or retention behavior.
