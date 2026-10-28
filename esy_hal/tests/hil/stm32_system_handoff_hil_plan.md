<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# STM32F4 Application Handoff HIL Plan

## Purpose and Preconditions

Verify that direct application handoff abandons the caller's stack only inside
the native trampoline, independently of compiler frame-pointer choices. This is
a startup transfer, not a CPU reset, exception return or RTOS context switch.
Use privileged Thread mode with MSP selected. Disable interrupts and stop DMA,
callbacks, other asynchronous users and unnecessary peripherals before calling.
The application must tolerate retained CPU/peripheral state and restore its own
interrupt policy during startup; the handoff leaves PRIMASK set.

Use a caller-validated image with a correctly aligned readable vector table, an
eight-byte-aligned initial MSP inside writable application RAM, and a Thumb
Reset_Handler address. Keep bootloader and application stack regions distinct.
Record board, MCU, memory map, vector contents, compiler, options and revision.

## Native Assembly Check

With `ESY_TEST_ARM_TOOLCHAIN_ROOT` pointing to the Arm GNU toolchain root and
`ESY_TEST_STM32_SUPPORT_ROOT` pointing to native CMSIS and STM32 HAL providers,
run the HAL-owned native regression from the repository root:

```sh
python3 -B esy_hal/tests/test_stm32_handoff_native.py
```

It compiles the actual backend to assembly and object files at all three settings
below, using the provider's HAL configuration template and real headers, then
runs the assembly checker. Missing environment variables produce an explicit
unittest skip; configured but unavailable providers fail. No SDK API is stubbed.

Compile the real `esy_hal_backend/src/stm32f4/esy_hb_boot.c` against the native
STM32F4 CMSIS/HAL headers at `-O0 -fomit-frame-pointer`, `-Og` and `-O2`. Select
the consumer's MCU and floating-point ABI. Produce both object files and GCC
`-S` assembly; a host stub is not equivalent evidence. From the repository root:

```sh
python3 -B esy_hal/tests/check_stm32_handoff_assembly.py \
    /tmp/stm32-handoff/O0-omit-fp.s \
    /tmp/stm32-handoff/Og.s /tmp/stm32-handoff/O2.s
```

The check requires the C wrapper to call the native entry without changing MSP.
The trampoline must contain only MSP assignment, barriers, the reset-entry
branch and a stack-free return trap. Keep compiler output and object disassembly
as evidence; passing this check does not establish hardware startup success.

## Hardware Procedure

1. Build both images at each optimization setting above. Break immediately before
   handoff and at the application's first Reset_Handler instruction. Confirm the
   initial MSP and reset entry were loaded while the bootloader stack remained
   selected, VTOR points to the application, PRIMASK is one and external NVIC
   enables/pending bits are clear. Watch both stack regions: after the trampoline
   writes MSP, no bootloader C local, load/store, prologue or epilogue may access
   either stack before application entry.
2. Let application startup initialize data/BSS and its vector/interrupt policy.
   Confirm its stack stays inside its own region and the first explicitly enabled
   interrupt reaches the application handler. Do not expect a default vendor
   Reset_Handler to restore PRIMASK automatically; the image owns that step.
3. In a separate diagnostic image, make Reset_Handler return immediately. Confirm
   execution stays in the trampoline's no-stack trap and never resumes the
   bootloader C wrapper or reads its abandoned stack.
4. In separate diagnostic runs, invoke the handoff from an ISR, an unprivileged
   Thread context and a PSP-selected Thread context. It must return without
   changing interrupt mask, tick/NVIC state, VTOR or either stack. Restore the
   diagnostic caller through its own CPU-context recovery mechanism.

## Evidence and Limits

Keep native assembly/object disassembly, register and stack-watch traces,
application entry/interrupt evidence and the diagnostic return/context results.
No backend host unit test substitutes for these checks. This plan does not
validate image authenticity, reset-vector ranges, board peripheral shutdown,
FPU/lazy exception state, caches, or a general-purpose running-system reboot.
