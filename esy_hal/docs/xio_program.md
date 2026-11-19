<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# RP PIO Program Execution

## Purpose

The program executor runs caller-supplied RP PIO v0 instructions through public
HAL. It provides precise multi-SM startup and 32-bit FIFO DMA without exposing
SDK objects or peripheral register addresses. The caller owns signal semantics,
pin assignment, cycle budgets, processing and scheduling. Existing XIO SPI/UART
interfaces remain appropriate for their documented transport contracts.

## Public Surface

Include `esy_hal/xio/esy_hal_xio_program.h` and link `esy_hal`. Program/SM types,
configuration, DMA notifications and status use standard C and platform-owned
definitions only. Program words intentionally use the target-specific RP PIO v0
encoding: this interface does not claim portable bytecode compatibility.

## Integration

Set `ENABLE_ESY_XIO` and `ENABLE_ESY_XIO_PROGRAM` in the consumer's feature entry.
Pico, Pico W, Pico 2 and Pico 2 W use the native provider; other ports reject the
feature during configuration. No OS or generic `ENABLE_ESY_DMA` facade is needed.

This example runs a one-instruction no-op program on a caller-reserved SM. The
immutable instruction array outlives the program. The owner calls the function
from task/main-loop context and retains all storage if cleanup fails.

```c
#include "esy_hal/xio/esy_hal_xio_program.h"

static const uint16_t k_idle[] = {0xa042u}; /* RP PIO v0 MOV Y,Y. */

esy_err_t example(esy_xio_program_t *program, esy_xio_program_sm_t *sm,
                  uint8_t instance, uint8_t index) {
  const esy_xio_program_config_t code = {
      .instructions = k_idle, .instruction_count = 1u,
      .entry = 0u, .wrap_bottom = 0u, .wrap_top = 0u};
  const esy_xio_program_sm_config_t config = {
      .instance = instance, .sm_index = index};
  esy_err_t result = esy_hal_xio_program_init(program, &code);
  if (result != ESY_OK) return result;
  result = esy_hal_xio_program_sm_init(sm, program, &config);
  if (result != ESY_OK) {
    esy_err_t cleanup = esy_hal_xio_program_deinit(program);
    return cleanup != ESY_OK ? cleanup : result;
  }
  esy_xio_program_sm_t *group[] = {sm};
  result = esy_hal_xio_program_start_group(group, 1u);
  esy_err_t cleanup = esy_hal_xio_program_sm_deinit(sm);
  if (cleanup != ESY_OK) return cleanup;
  cleanup = esy_hal_xio_program_deinit(program);
  return cleanup != ESY_OK ? cleanup : result;
}
```

The first call requires both caller-owned objects to be initialized with `{0}`.
Preconfigure safe output latches before enabling programs that drive pins.

## Contracts

One task/main-loop owner serializes lifecycle and I/O for all cooperating SMs.
No public operation is ISR-safe. DMA callbacks execute in ISR context and only
publish bounded notifications; callback contexts and handles remain alive until
successful task-context abort retires the route. No OS calls, logging or heap
allocation occur inside the executor.

Program initialization validates 1..32 instructions, entry/wrap bounds and
program-relative JMP targets. Instruction memory is allocated when an SM claims
the program. Init failures release temporary claims; repeated live init returns
STATE. Program deinit returns BUSY while any SM references it. Deinit is otherwise
idempotent and clears successfully released objects.

SM init remains disabled. Reset requires disabled hardware and no claimed DMA;
it clears FIFOs, returns PC to entry and zeroes X/Y/ISR/OSR without changing GPIO
latches. Both init and reset leave the ISR shift count at zero and the OSR shift
count at 32 (empty). With autopull enabled, the first OUT therefore fetches TX
data instead of shifting a zero prefix; an empty FIFO stalls that OUT. Reset
does not clear shared IRQ flags or sticky stall flags. Group start validates every SM
before one native synchronized divider-restart/enable operation; it preserves
preloaded FIFOs and PCs. Equal divider settings synchronize instruction cycles.
The owner proves cross-program phase, FIFO starvation behavior and electrical
idle levels. Clear IRQ/stall flags explicitly when their owner starts a run.

Divider ratios are quantized down to 1/256. Fractional dividers can vary individual
instruction durations; absolute cycle budgets must account for that variation.
Mapped inputs initialize with the native two-system-clock synchronizer enabled;
stale input-bypass settings from earlier transports are cleared. Input latency is
measured in `clk_sys` cycles. Keep the system clock fixed while timing matters.

DMA accepts nonempty aligned 32-bit memory arrays, at most `0x0fffffff` words,
channels 0..11 and IRQ 0/1. TX and RX can use different channels independently.
The first native IRQ assembly fixes its core and priority according to the shared
DMA backend contract. Completion reports transferred memory, not PIO waveform
completion. TX may finish while words remain queued; RX may finish before trailing
PIO instructions. Observe program status or application-defined hardware markers.

A DMA route stays claimed after its callback. Abort it before another start on
that direction, reset or deinit. Abort succeeds only after native hardware and
callbacks retire; failure retains borrowed storage and ownership for retry. Error
notifications likewise retain the buffer until abort. SM stop only disables the
SM and preserves the DMA claim. Establish safe pins, then abort DMA and deinit.
Native abort may block during task-context retirement; no additional wall-clock
timeout or software scheduler is introduced.

## Resource Model

Programs and SMs are caller-owned, aligned fixed storage. Each SM has one optional
TX and RX DMA route. Program instructions remain borrowed until program deinit.
The provider claims native SM/instruction resources alongside other XIO/SDK users.
The configuration owner reserves GPIOs and internal PIO IRQ flags and prevents
other programs from changing them. Only PIO instances 0/1 and GPIO bank base zero
are supported. No resource number or signal count is fixed by device business.

## Verification

HAL tests exercise the real forwarder using the reusable deterministic provider
fake, including grouped start rejection, DMA ownership through completion,
cleanup failure/retry and independent stall clearing. The same fake replaces
public HAL in consumer tests; it does not execute instructions or emulate edges.
Public headers compile as C11/C++17. Feature tests check source closure, parent
requirements and non-Pico rejection. The source contract check in
`tests/test_xio_program_contract.py` guards the final restart after register
injection for both init and reset; it does not execute PIO instructions.
Native SDK consumer builds validate the Pico
provider and linker closure. Hardware acceptance follows the
[PIO program HIL plan](../tests/hil/xio_program_hil_plan.md).

## Known Limits

PIO v1-only instructions, GPIO bank base 16, automatic SM allocation, circular
DMA, native pointers and software interpretations of bytecode are not provided.
The owner must not modify borrowed instructions or DMA buffers during active
access. Source/native build checks do not establish pad timing, analogue settling
or real FIFO service margins; those require hardware measurements.
