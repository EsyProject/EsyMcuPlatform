<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Memory Model

EsyMcuPlatform uses static or caller-owned storage for its own contexts, with
caller-provided task stacks, OS control blocks, queues and data buffers. This
makes their capacity visible to the application and avoids requiring a
general-purpose heap for those resources. Hardware SDKs and optional software
providers have their own memory models, described below.

## What static resources mean

Three different properties matter when choosing a memory policy:

| Property | Meaning |
| --- | --- |
| Static, caller-owned objects | The application supplies each object's storage and retains it for the documented lifetime. |
| No system-heap allocation | A path does not use libc, RTOS or SDK heap allocators. It may still allocate objects inside a separately reserved pool. |
| No runtime allocation | A path performs no allocation or release, including allocation from a fixed pool. |

A fixed memory budget does not imply that individual objects are statically
allocated. A pool can reserve all its RAM at link time and still allocate,
resize and release objects while the application runs. Exhaustion, fragmentation
and execution time must be assessed for the actual allocator and workload.

## Library-owned resources

The common, HAL, HAL backend and middleware implementations do not directly use
general-purpose heap allocation for their own contexts and buffers. The OS
wrapper also uses caller-owned storage: its FreeRTOS adapter passes the supplied
task stack/control storage to `xTaskCreateStatic()` and queue storage to
`xQueueCreateStatic()`. See the [OS resource contract](../../esy_middleware/os/README.md),
[task adapter](../../esy_middleware/os/src/freertos/esy_os_task.c) and
[queue adapter](../../esy_middleware/os/src/freertos/esy_os_queue.c).

Storage is borrowed until the module's documented cleanup completes. Capacity,
alignment, initialization state, callback lifetime and failed-cleanup behavior
remain part of each module's public contract. A static handle does not make
reclaiming its live backing storage safe.

Pico and STM32F4 builds can use these static interfaces without a libc or
FreeRTOS heap. The native FreeRTOS configuration must enable static allocation;
it may disable dynamic object creation when every selected dependency supports
that configuration. This is a property of the selected build, rather than a
claim that every optional SDK or software provider is allocation-free.

## LVGL: fixed RAM pool, dynamic GUI objects

The [LVGL adapter](../../esy_middleware/lvgl/README.md) requires the built-in
allocator, `LV_MEM_ADR=0` and no LVGL native OS port. With the provider's normal
static pool configuration, LVGL reserves `LV_MEM_SIZE` bytes in an internal
static array and manages that storage with TLSF. Do not configure a custom
`LV_MEM_POOL_ALLOC` hook that obtains this pool from a system heap when a
no-system-heap policy is required. The consumer chooses the pool capacity for
its UI; no product-specific capacity is required by this library.

The pool is used dynamically:

- `lv_obj_create()` reaches `lv_obj_class_create_obj()`, which calls
  `lv_malloc_zeroed()` for each object's instance storage.
- Formatted label updates such as `lv_label_set_text_fmt()` release previously
  owned text and allocate replacement text through `lv_text_set_text_vfmt()`.
- Other LVGL internals can allocate during rendering and interaction, so
  creating all widgets at startup alone does not establish a runtime without
  allocation.

These paths are visible in the LVGL v9.6.0
[object implementation](https://github.com/lvgl/lvgl/blob/v9.6.0/src/core/lv_obj_class.c),
[label implementation](https://github.com/lvgl/lvgl/blob/v9.6.0/src/widgets/label/lv_label.c)
and [built-in allocator](https://github.com/lvgl/lvgl/blob/v9.6.0/src/stdlib/builtin/lv_mem_core_builtin.c).

Persistent application text buffers with `lv_label_set_text_static()` avoid
allocating the label's text buffer on each update, provided the buffers remain
valid while LVGL references them. This does not remove widget or rendering
allocations. The adapter's own task, queue and drawing buffers remain
caller-owned; LVGL objects are provider-owned allocations inside its pool.

## Pico networking: lwIP pools and optional SDK allocation

The [TCP adapter](../../esy_middleware/protocol/README.md) uses lwIP connection
objects, packet buffers and copied send segments. Calls such as
`tcp_write(..., TCP_WRITE_FLAG_COPY)` can allocate through `pbuf_alloc()` and
`mem_malloc()`; connection and segment objects also use `memp_malloc()`.

With `MEM_LIBC_MALLOC=0`, `MEM_USE_POOLS=0` and `MEMP_MEM_MALLOC=0`, the normal
lwIP configuration uses an internal `ram_heap` array for variable-sized blocks
and separate static pools for fixed-sized objects. `MEM_SIZE` and `MEMP_NUM_*`
are consumer configuration inputs. This avoids libc malloc but still allocates
and releases provider objects during connection setup, receive and transmit.
Changing allocator hooks or enabling `MEM_LIBC_MALLOC` changes that conclusion.
See upstream lwIP's [memory implementation](https://github.com/lwip-tcpip/lwip/blob/STABLE-2_1_3_RELEASE/src/core/mem.c),
[pool implementation](https://github.com/lwip-tcpip/lwip/blob/STABLE-2_1_3_RELEASE/src/core/memp.c)
and [TCP output implementation](https://github.com/lwip-tcpip/lwip/blob/STABLE-2_1_3_RELEASE/src/core/tcp_out.c).
Check the actual provider revision used by the consumer.

The Pico SDK async provider is also significant. In **Pico SDK 2.1.1**:

- [`async_context_freertos_init()`](https://github.com/raspberrypi/pico-sdk/blob/2.1.1/src/rp2_common/pico_async_context/async_context_freertos.c)
  uses dynamic FreeRTOS semaphore, timer and task creation APIs. Selecting this
  provider requires the corresponding kernel allocation support.
- The [threadsafe background provider](https://github.com/raspberrypi/pico-sdk/blob/2.1.1/src/rp2_common/pico_async_context/async_context_threadsafe_background.c)
  can create an alarm pool when no custom pool is supplied and the default pool
  is disabled, or when its core does not match the default pool's core. The
  [alarm-pool creation implementation](https://github.com/raspberrypi/pico-sdk/blob/2.1.1/src/common/pico_time/time.c)
  calls `malloc()` and `calloc()`.

The library's default
[Pico network provider assembly](../../cmake/esy_third_party_import.cmake) uses
`pico_cyw43_arch_lwip_poll`, so those alternate async-context paths are not implied
by enabling that default provider. Custom providers must be checked separately.

Optional CYW43 Bluetooth support is another configuration boundary. The SDK's
[`cyw43_malloc` / `cyw43_free` hooks](https://github.com/raspberrypi/pico-sdk/blob/2.1.1/src/rp2_common/pico_cyw43_driver/include/cyw43_configport.h)
default to `malloc` / `free`, and
[`cybt_sharedbus_init()`](https://github.com/raspberrypi/pico-sdk/blob/2.1.1/src/rp2_common/pico_cyw43_driver/cybt_shared_bus/cybt_shared_bus.c)
uses them for shared-bus storage. The CYW43 driver enables that initialization
path under `CYW43_ENABLE_BLUETOOTH`; enabling Bluetooth requires checking those
hooks and the Bluetooth stack's resource model. A Pico Wi-Fi build without that
path does not establish the behavior of a Bluetooth-enabled build.

## C library output providers

The optional [printf logger backend](../../esy_middleware/logger/src/esy_logger_backend_printf_core.c)
uses `fwrite()` and `fflush()` on `stdout`. The selected C library and stdio
configuration determine whether stream initialization or buffering obtains heap
memory. Static logger queues and buffers do not establish that the output
provider is heap-free. Check the linked libc/SDK stdio implementation and its
buffer configuration when this backend is selected.

## ESP32 SDK allocations

The ESP32 backend uses ESP-IDF's native driver APIs. Those drivers own resources
whose storage is allocated by the SDK, even though EsyMcuPlatform's handles and
buffers remain static. The following examples are based on **ESP-IDF v5.3.4 for
classic ESP32**; different releases and targets require a fresh check.

| Platform path | Native operation and allocation boundary |
| --- | --- |
| [UART initialization](../../esy_hal_backend/src/esp32/esy_hb_uart.c) | `uart_driver_install()` allocates driver state, RX/TX ring buffers and synchronization resources. This backend disables the optional UART event queue, which does not remove those other allocations. See the [SDK UART implementation](https://github.com/espressif/esp-idf/blob/v5.3.4/components/esp_driver_uart/src/uart.c). |
| [I2C initialization](../../esy_hal_backend/src/esp32/esy_hb_i2c.c) | `i2c_new_master_bus()` and `i2c_master_bus_add_device()` allocate native bus/device resources. This backend creates one addressless device during initialization and reuses it across target addresses. See the [SDK I2C implementation](https://github.com/espressif/esp-idf/blob/v5.3.4/components/esp_driver_i2c/i2c_master.c). |
| [HAL timer start/cancel](../../esy_hal_backend/src/esp32/esy_hb_timer.c) | Starting a timer calls `esp_timer_create()`, which allocates an SDK timer object. Cancellation or one-shot retirement deletes it. Creation is a timer control operation and can occur after application startup. See the [SDK timer implementation](https://github.com/espressif/esp-idf/blob/v5.3.4/components/esp_timer/src/esp_timer.c). |
| [SPI initialization and transfer](../../esy_hal_backend/src/esp32/esy_hb_spi.c) | Native bus/device initialization allocates SDK resources. During `spi_device_transmit()`, `setup_priv_desc()` can also allocate temporary DMA buffers when the supplied TX/RX memory fails the SDK's DMA capability or alignment requirements. See the [SDK SPI implementation](https://github.com/espressif/esp-idf/blob/v5.3.4/components/esp_driver_spi/src/gpspi/spi_master.c). |

Most listed driver allocations occur during initialization or explicit control
operations. SPI's temporary-buffer path is a transfer-time exception; therefore
the ESP32 backend cannot promise that every data transfer avoids the SDK heap.
Suitable DMA-capable, aligned buffers can avoid that conditional path. The
selected target, native driver configuration and transaction sizes determine
whether the native driver uses a temporary buffer.

The SDK commonly uses `heap_caps_malloc()`, `heap_caps_calloc()` and related
capability-aware allocators. These are heap allocations even when no source
calls libc `malloc()` directly. ESP-IDF also connects libc allocation to its
heap infrastructure. See the official
[ESP-IDF heap allocation documentation](https://docs.espressif.com/projects/esp-idf/en/v5.3.4/esp32/api-reference/system/mem_alloc.html).

The wrapper's static resources do not replace SDK-owned native objects. Owners
must handle creation failure, retain resources while cleanup is incomplete and
follow the corresponding public HAL lifecycle contract. Measure SDK memory cost,
fragmentation and latency across repeated native create/delete cycles with the
selected ESP32 configuration and workload.

## What the FreeRTOS allocation switch controls

`configSUPPORT_DYNAMIC_ALLOCATION=0` disables FreeRTOS APIs that dynamically
create kernel objects, such as `xTaskCreate()` and `xQueueCreate()`. It does not
disable libc allocation, SDK allocators or allocation inside LVGL/lwIP pools.

ESP32 uses the FreeRTOS configuration supplied by ESP-IDF, rather than a Pico or
STM32F4 consumer's `FreeRTOSConfig.h`. The
[ESP-IDF v5.3.4 configuration](https://github.com/espressif/esp-idf/blob/v5.3.4/components/freertos/config/include/freertos/FreeRTOSConfig.h)
enables both static and dynamic allocation. Changing that SDK configuration
requires checking all enabled SDK dependencies; it is not a supported way to
turn the SDK into a no-heap implementation.

Even a native `*Static()` call does not identify the origin of its storage.
For example, ESP-IDF's `xQueueCreateWithCaps()` first allocates backing memory
with `heap_caps_malloc()` and then calls `xQueueCreateStatic()`. See the
[ESP-IDF FreeRTOS additions](https://github.com/espressif/esp-idf/blob/v5.3.4/components/freertos/esp_additions/idf_additions.c).
In contrast, EsyMcuPlatform's OS wrappers borrow the caller's supplied backing
storage directly.

## Choosing and verifying a memory policy

For a **no-system-heap** build, select static-capable Pico/STM32F4 paths, provide
all required OS and module storage, and audit each enabled provider's allocator
configuration. LVGL and lwIP can fit this policy with their static pool/heap
configurations, while still allocating inside the reserved memory. The listed
ESP32 native driver paths require SDK heap resources and do not fit this policy.

For a **strict no-runtime-allocation** build, also exclude or separately qualify
every provider path that allocates from fixed pools. The current LVGL GUI and
lwIP TCP paths do not provide that guarantee. If allocation is permitted only
during startup, assess subsequent control operations, reconnects, UI updates
and rendering separately; startup-only application object creation is
insufficient evidence.

The [direct-allocation source check](../../tools/checks/check_no_dynamic_alloc.py)
rejects direct C/POSIX allocator references in scanned production sources; its
scope excludes the OS implementation, tests and external providers. It neither
follows SDK/library calls nor proves a complete firmware image is allocation-free.
Use the actual feature configuration, provider sources, linked image and target
allocation tracing to verify the intended policy. Source checks and host tests
do not replace native memory and timing measurements.
