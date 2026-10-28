<!--
SPDX-FileCopyrightText: 2026 EsyProject contributors

SPDX-License-Identifier: Apache-2.0
-->

# Sensors

## Purpose

`esy_sensor` provides a common channel, physical-unit, configuration and event
interface for measurement devices. A synchronous acquisition replaces a cached
sample, which upper layers can read without further bus activity. The consumer
owns scheduling, buses and event routing. The core supports both baremetal main
loops and existing owner tasks without an OS or HAL dependency.

## Public Surface

- [`esy_sensor.h`](include/esy_middleware/sensor/esy_sensor.h): caller-owned
  handles, provider interface, supported-channel discovery, fetch/get, units,
  attributes and one copied trigger subscription per instance.
- [`esy_sensor_sht3x.h`](include/esy_middleware/sensor/esy_sensor_sht3x.h): SHT3x
  single-shot temperature and relative humidity with CRC validation.
- [`esy_sensor_mpu6050.h`](include/esy_middleware/sensor/esy_sensor_mpu6050.h):
  MPU6050 temperature, acceleration and angular velocity with device rate control.
- [`esy_sensor_ina219.h`](include/esy_middleware/sensor/esy_sensor_ina219.h):
  INA219 calibrated bus/shunt voltage, current and power.

Each scalar is an `int64_t` physical value multiplied by 1,000,000. For example,
`-1250000` represents -1.25 degrees Celsius and `3300000` represents 3.3 volts.
Channel units are Celsius, relative humidity percent, pascals, metres per second
squared, radians per second, volts, amperes and watts. Vector axes have separate
channels and share one fetched device sample. Pressure is available to custom
providers; no pressure device driver is included.

## Integration

Enable `ENABLE_ESY_SENSOR` in the consumer's `esy_feature_import.cmake`, then link
`esy_sensor` for a custom provider or `esy_sensor_i2c` for the included devices.
The latter requires `ENABLE_ESY_I2C` and `ENABLE_ESY_SYSTEM`; it propagates core
and public HAL dependencies. Neither target creates an OS dependency. Disabling
`ENABLE_ESY_SENSOR` omits the entire module; disabling I2C omits all built-in drivers
and their HAL dependency while leaving the core available. The library only
consumes feature values and does not define another option source.

The lifecycle owner first initializes a public HAL I2C master handle and ensures
its bus supports combined repeated-start transfers. It then initializes a zeroed
sensor handle with one device's explicit address and finite transaction timeout.
Do not share a physical device with another driver instance. Serialize all
devices sharing a bus with its lifecycle and power-management owner.

For example, attach an SHT3x after its power-up interval with a bus that the
consumer already initialized:

```c
#include "esy_middleware/sensor/esy_sensor_sht3x.h"

esy_sensor_t device = {0};
const esy_sensor_sht3x_config_t config = {
    .i2c = initialized_bus,
    .address = 0x44,
    .timeout_ms = 25,
};
esy_err_t status = esy_sensor_sht3x_init(&device, &config);
/* Check status, then use &device with the common APIs below. */
```

The common acquisition and event flow, after concrete driver initialization, is:

```c
#include "esy_middleware/sensor/esy_sensor.h"

static void on_sample(const esy_sensor_trigger_t *trigger,
                      const esy_sensor_sample_t *sample, void *arg) {
  int64_t *latest_micro_celsius = arg;
  *latest_micro_celsius = sample->micro[trigger->channel];
}

/* sensor is an initialized, exclusively owned esy_sensor_t. */
int64_t temperature = 0;
const esy_sensor_trigger_t trigger = {
    .type = ESY_SENSOR_TRIGGER_DATA_READY,
    .channel = ESY_SENSOR_CHAN_TEMPERATURE,
};
esy_err_t err = esy_sensor_trigger_set(sensor, &trigger, on_sample, &temperature);
/* Check err. Each owner-selected acquisition then uses: */
err = esy_sensor_sample_fetch(sensor);
if (err == ESY_OK) {
  err = esy_sensor_channel_get(sensor, ESY_SENSOR_CHAN_TEMPERATURE, &temperature);
}
/* Check err before consuming temperature. Stop owner activity before cleanup. */
err = esy_sensor_deinit(sensor);
```

Keep callback context alive until cancellation or successful deinit. A callback
can copy data to an existing bounded consumer queue, with overflow policy owned
by that consumer. Hand off GPIO data-ready interrupts through the application's
existing IRQ owner before invoking fetch in its main loop or task.

## Contracts

### Lifecycle and error behavior

Initialize each `esy_sensor_t` with `{0}` before its first use and keep its address
stable. Do not copy, move or clear a live handle. Init rejects a live handle with
`ESY_ERR_STATE`. Driver init validates configuration before bus access and commits
software state only after hardware setup succeeds. Failure permits retry, but
prior successful device writes may have changed physical configuration; there is
no register rollback. The bus remains borrowed and is never initialized or
deinitialized by a sensor driver.

Successful deinit cancels callbacks and restores the zero state; repeated deinit
succeeds. Built-in deinit only detaches software state, leaving hardware conversion
and power settings intact. A custom provider cleanup failure retains the live
handle and borrowed resources for retry. Stop acquisitions and finish callbacks
before deinit, then release the bus and callback context.

Fetch commits all declared channels only when the entire provider acquisition
succeeds. Any acquisition error invalidates the cache and emits no event. Cached
get requires a successful fetch, has no I/O side effects, and changes its output
only on success. An attribute hook attempt also invalidates the cache, including
failed writes which may have partially changed the device. Invalid pointers and
enum/range values return `ESY_ERR_INVALID_ARG`, unsupported channels or attributes
return `ESY_ERR_NOT_SUPPORTED`, and unavailable samples return `ESY_ERR_STATE`.
Negative HAL errors propagate; nonnegative short transfers become `ESY_ERR_IO`.

### Scheduling, timing and concurrency

One main-loop/task owner serializes all operations, provider state and borrowed
resources. APIs and callbacks are never ISR-safe. The reentry guard returns
`ESY_ERR_BUSY` during an operation or its callback; it is not a lock. Callbacks
must return promptly and cannot call back into the same handle.

There is no periodic worker, timestamp deadline or autonomous trigger. Fetch does
one driver acquisition followed by at most one callback. SHT3x acquisition uses
two I2C transactions and a 16 ms conversion delay. MPU6050 fetch uses one combined
transaction; initialization includes a 30 ms startup wait and a 10 ms first-sample
wait. INA219 fetch uses four
sequential combined transactions and can return `ESY_ERR_BUSY` while conversion
is incomplete. Each transfer uses the configured finite timeout in milliseconds,
from 1 through `INT32_MAX`; the bound applies separately to each transfer, not
the complete fetch/init call. Actual latency includes provider and callback time.

The module does not compare timestamps or retain one-shot waits. HAL owns timeout
clock width, wrap handling and stop/sleep semantics; system delay requires the
initialized platform's advancing timebase. There is no sensor observation-gap
constraint, but the caller must choose a cadence consistent with device conversion
rates and tolerated missed events. A pause performs no work or catch-up. Sampling
frequency attributes configure the device's output rate in microhertz, independently
of the frequency of owner calls. Readback gives the accepted quantized device rate.

`DATA_READY` means successful software acquisition. High/low threshold events
emit on the first qualifying sample and when the condition becomes true again
after an observed nonqualifying sample. Equality qualifies. Failed acquisitions
do not rearm a threshold. Replacing a subscription resets its edge state;
cancellation remains inactive until explicitly registered again. Events are
evaluated only on sampled values and do not configure hardware interrupt lines.

### Device behavior

SHT3x uses high-repeatability single-shot acquisition without clock stretching,
validates both CRC bytes and maps humidity into 0..100 percent. Device output-rate
and full-scale attributes are unsupported; the caller chooses its single-shot rate.
Allow the datasheet power-up interval before the first acquisition.

MPU6050 verifies `WHO_AM_I`, enables the X gyroscope PLL after startup, configures
the 1 kHz filtered output path and starts at 100 Hz with fixed +/-2 g and
+/-250 degrees/s ranges. Frequency applies to all axes and temperature; full-scale
queries report the fixed ranges and unsupported changes fail explicitly. The
frequency setter accepts rates representable by the hardware divider; its header
defines integer microhertz rounding. Temperature measures the die, not ambient air.

INA219 uses continuous shunt/bus conversion with the documented 32 V, +/-320 mV,
12-bit ADC register configuration; the electrical bus limit remains 26 V.
Calibration derives from caller-provided shunt
microohms and nominal current-LSB microamperes. Conversion uses the accepted
even-valued calibration to account for register quantization. Overflow is an I/O
error; conversion-not-ready is busy. Power is read last because reading it clears
conversion-ready. Four reads can straddle a conversion; they are not an atomic
snapshot. Output-rate/full-scale attributes are unsupported.

## Resource Model

All instance state is in caller-owned handles, including aligned storage for
built-in drivers. Custom provider state and immutable dispatch tables are borrowed
until successful deinit. There are no module singletons, heap allocations, worker
tasks, queues, timer objects, IRQ registrations, DMA or watchdog subscriptions.
Stack-local bus buffers are retained only for the synchronous HAL call.

The module creates no tasks requiring an OTA stop policy. The consumer's execution
owner decides whether sampling can continue during flash operations based on bus,
callback, ISR and watchdog constraints. Where quiescence is required, stop owner
calls and finish the current synchronous operation before flash activity; cancel
subscriptions or deinitialize before releasing resources. Resume from an explicit
owner decision after OTA success, failure or cancellation.

## Verification

From `tests/` run:

```sh
ceedling test:all
ceedling gcov:all
ruby ../../../tools/quality/run_module_tests.rb . build/sanitizer test:all --sanitizer
python3 -B test_cmake_consumer.py
```

Tests cover physical conversions, CRC, calibration quantization, signed values,
device rate limits, malformed/short transfers, invalid parameters, repeated
lifecycle, cleanup retry, cached-data invalidation, threshold crossings,
cancellation and callback reentry. The reusable HAL fake queues exact transfer
expectations rather than reproducing device algorithms. CMake consumers cover
feature-off, core-only baremetal, optional I2C dependencies and C/C++ public headers.
The existing embedded consumer build validates the target toolchain.

Primary device references are the
[Sensirion SHT3x-DIS datasheet](https://admin.sensirion.com/media/documents/213E6A3B/63A5A569/Datasheet_SHT3x_DIS.pdf),
[TDK MPU-6000/6050 register map](https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Register-Map.pdf)
and [TI INA219 datasheet](https://www.ti.com/lit/ds/symlink/ina219.pdf).

## Known Limits

No SPI/ADC device provider, hardware threshold interrupt configuration, FIFO,
asynchronous transfer, streaming decoder, sensor fusion, automatic discovery or
automatic power management is included. The provider API permits additional
device drivers without changing callers. Host tests establish register and
software contracts. On each board, measure physical accuracy, startup, power
behavior, bus timing and sampling latency with the selected devices.
