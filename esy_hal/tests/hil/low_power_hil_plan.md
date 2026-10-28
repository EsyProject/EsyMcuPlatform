# Low-Power HIL Plan

## Purpose

This plan verifies the power transaction, deadline timer, clocksource,
wake-reason, constraint, multicore, and FreeRTOS tickless contracts on real
Pico and STM32F4 hardware. Host tests cannot prove oscillator continuity, wake
routing, clock restoration, cross-core parking, or current draw.

## Instrumentation

- A controllable GPIO wake input connected to a signal generator.
- A board output toggled immediately before atomic entry and after early wake.
- Serial output emitted only after interrupts and clocks are restored.
- A logic analyzer for entry duration and wake latency.
- A current probe with enough bandwidth to distinguish active, CPU idle, and
  retention states.
- A reference timer accurate to at least 100 ppm for the long-run drift test.

Record board revision, firmware commit, toolchain, RTOS tick rate, clock tree,
wake source, requested state, selected state, wake reason, elapsed microseconds,
stepped ticks, measured duration, and average current for every run.

## Pico Procedure

1. Build with low power and FreeRTOS tickless enabled. Initialize the standard
   wakeup-timer adapter with an explicitly reserved hardware alarm index and
   use it as both deadline timer and clocksource for `CPU_IDLE` only.
2. Run deadlines of 0.5, 1, 10, 100, and 1000 ticks. Verify that the selected
   state is never deeper than `CPU_IDLE`, the timer wakes the core, and the
   serial task continues without timeout compression.
3. Assert the GPIO wake input immediately before WFI and while sleeping.
   Verify no missed wake and no repeated entry loop; this exercises the
   DSB/WFI/ISB and pending-event boundary.
4. Repeat 100,000 random deadlines and external wake offsets. The accumulated
   RTOS time error must remain within one kernel tick plus reference-clock
   tolerance, with no monotonic regression.
5. Race a power-lock acquisition with atomic entry. Either acquisition fails
   before activity starts or entry aborts during final lock revalidation; all
   prepared resources must be released exactly once.
6. When both cores are scheduler-managed, run with and without a core
   coordinator. Initialization must reject the missing coordinator. With one
   installed, verify the peer is parked before commit and remains parked until
   device graph recovery and wake-source unmask are both complete.
7. Measure the armed hardware-alarm timestamp. It must equal the absolute hard
   deadline minus advertised exit latency and configured late-resume guard.
   Repeat entry near the start, middle, and end of a kernel tick. The absolute
   latest-resume deadline must subtract the elapsed current-tick fraction and
   must never move later than the next FreeRTOS unblock tick.
8. Run with the watchdog adapter, delay transaction preparation across several
   monitor ticks, and verify atomic revalidation either shortens the alarm or
   aborts entry before the hardware deadline can be violated.

## STM32F4 Procedure

1. First run without an RTC/LPTIM timekeeper registration. Timed retention must
   fail closed; no STOP entry is permitted. Confirm that TIM6 is neither
   initialized nor read by the system-time implementation.
2. Add a board-owned RTC/LPTIM deadline timer and clocksource. Verify their
   independent per-state masks reject unsupported combinations. Clock
   restoration remains wholly inside the STM32F4 low-power HAL backend lifecycle.
3. Run STOP deadlines of 10 ms, 100 ms, 1 s, and 30 s. After wake, verify PLL,
   HCLK, APB clocks, UART baud, and RTOS deadlines before normal device resume.
4. Inject deadline-arm failure, STOP-entry failure, clocksource-read failure,
   wake-reason-read failure, disarm failure, and clock-restore failure. Confirm
   rollback order and surfaced errors. Clock-restore failure must reset while
   interrupts remain masked and before SysTick restarts.
5. Hold HSE ready and PLL ready low in separate fault-injection runs. Each
   early restore attempt must leave its polling loop within the advertised
   exit-latency bound and enter the fatal reset path without a SysTick or other
   interrupt. Repeat with DWT initially enabled and disabled, and verify the
   HAL backend preserves the original trace and cycle-counter enable state.
6. Cross the RTC subsecond and calendar rollover boundaries. Elapsed time must
   remain monotonic and within the source accuracy; no 65.536 ms discontinuity
   is allowed.
7. Run at least eight hours of randomized tickless retention. RTOS drift must
   remain within one tick plus measured oscillator tolerance, and no device may
   resume before clocks and the kernel tick are restored.
8. Run with the hardware watchdog active. Its registered constraint must cap
   every armed deadline to the measured safe remaining watchdog interval; no
   idle cycle may rely on feeding the watchdog from the power path.

## Pass Criteria

Also run these recovery-boundary injections on each enabled
port and record callback order with GPIO trace markers:

- Make a constraint change from infinite to finite after prepare. Without a
  prepared deadline timer, entry must abort before WFI; all parked peers remain
  held until late recovery completes.
- Fail graph prepare and its rollback. The OS must request reset with global
  interrupts masked, without resuming producers or unmasking wake sources.
- Fail late graph recovery, constraint resume and producer resume separately.
  Confirm dependent wake unmasking and peer release do not occur before reset.
- Add a known bounded delay to early recovery, including an entry canceled after
  tick suspension. Stepped RTOS time must include this interval. Measure the
  stop/read and read/restart gaps separately and include their worst-case error
  in the product timing budget.
- Unregister the middle of three wake sources during configuration and reuse
  its slot. Confirm the surviving sources retain their mask bits and physical
  wake attribution. Retire any request mask that referred to the removed source.

- Automatic idle never enters standby or another reset-class state.
- Every finite deadline has a demonstrably active wake source.
- Every deadline and constraint uses one absolute monotonic clock domain; the
  programmed alarm reserves exit latency and the late-resume guard.
- Reported elapsed time is monotonic and matches the external reference within
  the documented oscillator tolerance.
- Pending SysTick produces exactly one kernel tick, never zero or two.
- A nonzero current-tick fraction advances the latest-resume deadline by that
  fraction; tickless entry is skipped if the phase cannot be sampled safely.
- Prepare executes with interrupts enabled. Atomic entry and `recover_atomic`
  keep global interrupts masked while the backend and graph restore their
  IRQ-safe minimum state. Prepared wake sources remain locally masked while
  global interrupts and the OS tick are restored. Late graph callbacks,
  participant recovery, wake-source unmask, and peer release then run with
  global interrupts enabled; no source IRQ may run against a suspended graph.
- When deferred output or backlight restoration is assembled, instrument those
  callbacks (for example ADF4372/ILI9488 providers when enabled). Verify neither
  runs in the global interrupt-masked window, and verify the configured
  wake-source ISR remains blocked until each late callback finishes.
- All failure injections leave resources balanced or explicitly fault the
  manager. No test relies on a nominal-success placeholder.
- Standby occurs only after an explicit shutdown request and boots through the
  reset path; automatic idle never selects it.

## Known Limits

This repository does not select a concrete STM32F4 board RTC/LPTIM resource or
flashing/debug probe. Those are consumer-board facts. A HIL result is valid only
when the product records the concrete board configuration and raw measurement
artifacts alongside the firmware commit.
