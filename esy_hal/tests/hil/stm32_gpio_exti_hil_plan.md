<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# STM32F4 GPIO EXTI HIL Plan

## Purpose

Verify per-line ownership and callback retirement while several GPIOs share an
EXTI NVIC group. Public HAL tests and real SDK compilation do not prove physical
IRQ delivery, pending-event preservation or electrical wake behavior.

## Integration

Use an STM32F4 consumer with GPIO enabled and the real EXTI vector provider.
Reserve at least two supported pads in lines 5..9 and two in lines 10..15. Where
available, reserve a pad on another port with the same line number. Supply a
signal generator or jumper fixture and capture callback counts and NVIC/EXTI
registers with a debugger. Record MCU/package, board, firmware revision, SDK,
clock settings, pad mapping, NVIC priority grouping and board-provided priorities.

Serialize lifecycle/configuration on task owners. Callbacks may count events or
post a bounded notification; they must not configure, attach, enable/disable or
deinitialize GPIOs. Keep callback arguments alive until deinit or wake-source
detach returns. Leave all NVIC configuration for owned EXTI groups to the backend.
Do not install competing raw EXTI callbacks or change interrupt priority grouping.

## Procedure

1. Initialize two public GPIO handles on distinct lines of EXTI9_5; attach equal
   priorities and verify that each pad invokes only its own encoded-pin callback.
   Repeat with EXTI15_10 and a dedicated EXTI0..4 line.
2. Keep one IRQ attached and deinitialize a second ordinary GPIO in the same group
   that never attached an IRQ. Verify continued callback delivery and unchanged
   peer IMR, pending bit, routing and group priority. Repeat with two attached
   lines: deinitialize one and verify the peer continues. The group NVIC IRQ must
   stay enabled until its final registration is released.
3. Attach PAx, then initialize/deinitialize PBx as an ordinary GPIO for the same
   EXTI line, using pads actually available on the selected device. The PAx
   registration, EXTICR field, pending state and callbacks must remain intact.
   Attempt to attach PBx while PAx owns that line; expect STATE with no register
   or callback change. After PAx deinit, attach PBx successfully and verify routing.
4. Attempt a duplicate attach to the same pin and a peer attach in the same shared
   group with a different priority. Both return STATE without altering existing
   hardware or callback ownership. An out-of-range priority returns INVALID_ARG.
   Repeat after temporarily disabling the existing line: its priority still owns
   the group. Once every registration is detached, attach a new priority.
5. With two peer lines registered, call the backend enable/disable primitive from
   a diagnostic task owner on one encoded pin. Disable only its IMR bit and clear
   only its pending bit; the other line's callbacks continue. Disabled callbacks
   must not run if a peer causes shared dispatch. Repeated disable/enable is safe.
   Pulse the disabled line, enable it, and verify old events are discarded while
   new edges are delivered. Enable/disable on another port of that line must not
   change its owner. These backend primitives are diagnostic integration calls,
   not middleware dependencies.
6. Hold a peer event pending while the owner detaches another line. Verify that
   clearing the detached line or its NVIC lifecycle does not discard the peer
   event. Repeat while disabling/deinitializing a normal GPIO in the same group.
   Use a task-controlled PRIMASK window or target debugger to arrange the event;
   do not call lifecycle from ISR or install a second production IRQ provider.
7. Register a fresh wake-source GPIO, then disable_wakeup and enable it again.
   Verify detach releases its EXTI ownership and no old callback/argument is used.
   Repeated public disable_wakeup returns STATE; repeated backend detach is
   harmless. A wake attach to an already registered line returns STATE. If the
   selected low-power mode supports that EXTI source, verify actual wake and
   normal peer IRQ delivery across the cycle. Do not infer deep-sleep support
   from an ordinary edge interrupt alone.
8. Generate edges immediately before/during task-side disable, wake detach and
   deinit. After successful detach/deinit returns, retire the callback argument
   and confirm no late access. Reinitialize and attach a different callback; no
   old callback may be replayed. Observe that callbacks execute outside task-side
   register-update masking and that ISR work stays bounded to its group lines.
9. In an isolated diagnostic callback, attempt status-returning backend lifecycle
   calls; expect STATE with existing registrations intact. Void lifecycle calls
   must have no effect. Ordinary production callbacks must obey the task-only
   lifecycle rule; the public HAL facade must not be called from that diagnostic
   callback because its contract excludes lifecycle reentry.

## Acceptance and Limits

Every event dispatch belongs to the enabled owner of that exact EXTI line.
Independent GPIO cleanup does not disable or discard a peer interrupt. Successful
registration rejects line/priority conflicts before changing hardware. Temporary
masking retains callback ownership; final detach retires it before return.

Hardware EXTI pending bits coalesce edges; no exact lossless edge count is
promised. Static resource ownership and shared NVIC priority do not add hardware
capture precision or permit uncoordinated native register/IRQ access.
