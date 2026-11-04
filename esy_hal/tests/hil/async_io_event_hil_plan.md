<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Automatic DMA and Scheduled I/O Scenarios

1. Native finite completion delivers one ISR callback without any HAL process
   call. Half/circular callbacks retain ownership; late IRQs do not duplicate
   finite terminal delivery.
2. Delay one SPI direction or STM32 UART TC. Resources remain borrowed until
   the documented terminal condition. Pico TX DMA retires source storage while
   its physical UART busy query can remain true.
3. Inject a bounded-stop failure. Retain buffers/claim for explicit task abort;
   never report early success or spin indefinitely in an IRQ.
4. Verify logical DMA release versus Pico cached SDK reservation lifecycle.
   Failed task release/deinit keeps the handle valid until retry succeeds.
5. ESP32 public DMA and UART stream callbacks reject with no retained storage.
   Schedulers use public synchronous HAL and deliver results in the chosen owner.
6. Verify HAL/backend headers, sources and targets have no platform OS dependency.
   Explicit middleware OS adapters own static task, queue and stack resources.
