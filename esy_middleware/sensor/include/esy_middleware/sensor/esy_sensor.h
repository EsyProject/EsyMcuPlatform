/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESY_SENSOR_H
#define ESY_SENSOR_H

#include <stdbool.h>
#include <stdint.h>
#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file esy_sensor.h
 * @brief Synchronous, caller-driven sensor framework without OS dependencies.
 *
 * One main-loop/task owner serializes every operation, including borrowed bus
 * access and lifecycle. No operation is ISR-safe. Fetch performs one driver
 * acquisition and at most one synchronous callback; bus timeouts, conversion
 * delays and callback execution contribute to latency. There is no scheduler,
 * timestamp comparison, retry loop, allocation or background work. The caller
 * chooses sampling cadence and hands off IRQ notifications before fetching.
 */

/** @brief Scalar measurement channels; vectors use three independent axes. */
typedef enum {
  ESY_SENSOR_CHAN_TEMPERATURE = 0, /**< Degrees Celsius. */
  ESY_SENSOR_CHAN_HUMIDITY, /**< Relative humidity in percent. */
  ESY_SENSOR_CHAN_PRESSURE, /**< Pascals. */
  ESY_SENSOR_CHAN_ACCEL_X, /**< Acceleration along X, m/s^2. */
  ESY_SENSOR_CHAN_ACCEL_Y, /**< Acceleration along Y, m/s^2. */
  ESY_SENSOR_CHAN_ACCEL_Z, /**< Acceleration along Z, m/s^2. */
  ESY_SENSOR_CHAN_GYRO_X, /**< Angular velocity around X, rad/s. */
  ESY_SENSOR_CHAN_GYRO_Y, /**< Angular velocity around Y, rad/s. */
  ESY_SENSOR_CHAN_GYRO_Z, /**< Angular velocity around Z, rad/s. */
  ESY_SENSOR_CHAN_VOLTAGE, /**< Bus voltage in volts. */
  ESY_SENSOR_CHAN_SHUNT_VOLTAGE, /**< Signed shunt voltage in volts. */
  ESY_SENSOR_CHAN_CURRENT, /**< Signed current in amperes. */
  ESY_SENSOR_CHAN_POWER, /**< Power in watts. */
  ESY_SENSOR_CHAN_COUNT /**< Number of channels; never a valid channel. */
} esy_sensor_channel_t;

/** @brief Channel mask bit; pass a valid esy_sensor_channel_t only. */
#define ESY_SENSOR_CHANNEL_BIT(channel) (UINT32_C(1) << (channel))
/** @brief Capacity reserved for built-in driver state; do not inspect it. */
#define ESY_SENSOR_DRIVER_BYTES 128u
/** @brief Fixed-point multiplier: one physical unit equals this many micro units. */
#define ESY_SENSOR_MICRO INT64_C(1000000)

/** @brief Physical units associated with scalar channels. */
typedef enum {
  ESY_SENSOR_UNIT_CELSIUS = 0, /**< Degrees Celsius. */
  ESY_SENSOR_UNIT_PERCENT, /**< Percent, with 100 representing saturation. */
  ESY_SENSOR_UNIT_PASCAL, /**< Pascal. */
  ESY_SENSOR_UNIT_METRE_PER_SECOND_SQUARED, /**< Acceleration. */
  ESY_SENSOR_UNIT_RADIAN_PER_SECOND, /**< Angular velocity. */
  ESY_SENSOR_UNIT_VOLT, /**< Volt. */
  ESY_SENSOR_UNIT_AMPERE, /**< Ampere. */
  ESY_SENSOR_UNIT_WATT /**< Watt. */
} esy_sensor_unit_t;

/** @brief Configurable driver attributes; unsupported combinations fail explicitly. */
typedef enum {
  ESY_SENSOR_ATTR_SAMPLING_FREQUENCY = 0, /**< Microhertz; device rate, not a scheduler. */
  ESY_SENSOR_ATTR_FULL_SCALE /**< Positive range magnitude in channel micro units. */
} esy_sensor_attribute_t;

/** @brief Acquisition result; only bits present in channels have valid values. */
typedef struct {
  uint32_t channels; /**< Successfully acquired channel mask. */
  int64_t micro[ESY_SENSOR_CHAN_COUNT]; /**< Physical values multiplied by 1,000,000. */
} esy_sensor_sample_t;

/** @brief Software events evaluated only after a successful explicit fetch. */
typedef enum {
  ESY_SENSOR_TRIGGER_DATA_READY = 0, /**< Every successful acquisition. */
  ESY_SENSOR_TRIGGER_THRESHOLD_HIGH, /**< First sample >= threshold, then crossings. */
  ESY_SENSOR_TRIGGER_THRESHOLD_LOW /**< First sample <= threshold, then crossings. */
} esy_sensor_trigger_type_t;

/** @brief Single copied subscription; threshold events rearm after leaving the condition. */
typedef struct {
  esy_sensor_trigger_type_t type; /**< Event condition. */
  esy_sensor_channel_t channel; /**< Required supported scalar channel. */
  int64_t threshold_micro; /**< Threshold in the channel's micro units. */
} esy_sensor_trigger_t;

/**
 * @brief Callback in the fetch owner's context, with borrowed call-lifetime data.
 * Do not reenter this handle, block indefinitely, or retain trigger/sample pointers.
 * All handle operations from the callback return ESY_ERR_BUSY.
 */
typedef void (*esy_sensor_event_fn)(const esy_sensor_trigger_t *trigger,
                                    const esy_sensor_sample_t *sample, void *arg);

/**
 * @brief Provider contract for concrete drivers and focused custom providers.
 *
 * The immutable table and ctx are borrowed until successful deinit. fetch writes
 * a complete sample with exactly channels on success; failures discard its output.
 * Optional attribute hooks return NOT_SUPPORTED when absent. They use scalar
 * channel micro units, except frequency in microhertz. deinit may be NULL for
 * providers without cleanup; on failure retain resources for a retry. Hooks run
 * synchronously on the serialized owner, never ISR context, without reentry.
 */
typedef struct {
  uint32_t channels; /**< Nonempty supported mask containing only defined channels. */
  esy_err_t (*fetch)(void *ctx, esy_sensor_sample_t *sample); /**< Required acquisition. */
  esy_err_t (*attr_set)(void *ctx, esy_sensor_channel_t channel,
                      esy_sensor_attribute_t attr, int64_t micro); /**< Optional configuration. */
  esy_err_t (*attr_get)(void *ctx, esy_sensor_channel_t channel,
                      esy_sensor_attribute_t attr, int64_t *micro); /**< Optional query. */
  esy_err_t (*deinit)(void *ctx); /**< Optional resource cleanup. */
} esy_sensor_driver_t;

/**
 * @brief Caller-owned handle; initialize with {0} before first use.
 *
 * Fields are implementation-owned. Do not copy, edit, clear or move a live handle.
 * Failed init leaves a zero handle reusable; repeated init preserves a live one.
 * Successful deinit zeroes the handle and cancels its subscription. No references
 * or operations survive success. Keep provider, callback arg and buses alive
 * until then; failed deinit retains them for retry. Built-in drivers borrow buses
 * without initializing, deinitializing or registering their IRQs.
 */
typedef struct {
  const esy_sensor_driver_t *driver; /**< Private dispatch. */
  void *ctx; /**< Private borrowed provider state. */
  esy_sensor_sample_t sample; /**< Private committed cache. */
  esy_sensor_trigger_t trigger; /**< Private copied subscription. */
  esy_sensor_event_fn on_event; /**< Private synchronous callback. */
  void *event_arg; /**< Private borrowed callback context. */
  bool valid; /**< Private cache validity. */
  bool busy; /**< Private reentry guard, not a concurrency lock. */
  bool triggered; /**< Private threshold edge state. */
  ESY_ALIGNED_STORAGE(storage, ESY_SENSOR_DRIVER_BYTES); /**< Built-in driver storage. */
} esy_sensor_t;

/**
 * @brief Attach a provider with non-NULL context; performs no hardware operations.
 * @return INVALID_ARG for invalid pointers/table/mask, STATE for a live handle,
 * BUSY on callback reentry, otherwise OK. No references retained on failure.
 */
esy_err_t esy_sensor_init(esy_sensor_t *sensor, const esy_sensor_driver_t *driver,
                          void *ctx);
/** @brief Release a provider; zero handles succeed, failures retain live state. */
esy_err_t esy_sensor_deinit(esy_sensor_t *sensor);
/**
 * @brief Acquire all supported channels, replace the cache, and evaluate one trigger.
 * @return Driver error, IO for an invalid provider sample mask, STATE if detached,
 * BUSY on reentry, INVALID_ARG for NULL, otherwise OK. Any failure from acquisition
 * invalidates cached readings; channel_get never presents stale data as fresh.
 */
esy_err_t esy_sensor_sample_fetch(esy_sensor_t *sensor);
/**
 * @brief Query the supported channel mask without I/O; writes only on success.
 * @return INVALID_ARG for NULL, STATE if detached, BUSY on reentry, otherwise OK.
 */
esy_err_t esy_sensor_get_channels(const esy_sensor_t *sensor, uint32_t *channels);
/**
 * @brief Read one cached micro-unit scalar without I/O; output changes only on OK.
 * @return INVALID_ARG for pointers/channel, NOT_SUPPORTED for an absent channel,
 * STATE before a successful fetch/after configuration, BUSY on reentry, or OK.
 */
esy_err_t esy_sensor_channel_get(const esy_sensor_t *sensor,
                                 esy_sensor_channel_t channel, int64_t *micro);
/** @brief Query a channel's unit without a device; invalid arguments leave output unchanged. */
esy_err_t esy_sensor_channel_unit(esy_sensor_channel_t channel, esy_sensor_unit_t *unit);
/**
 * @brief Set a positive attribute on a supported channel; invalidates the cache.
 * @return INVALID_ARG for pointers/enums/nonpositive values, STATE if detached,
 * BUSY on reentry, NOT_SUPPORTED for absent channels/hooks/combinations, or driver result.
 * Once a hook is called, the cache is invalid even on error because bus writes
 * may have partially succeeded. Driver-specific accepted ranges are documented.
 */
esy_err_t esy_sensor_attr_set(esy_sensor_t *sensor, esy_sensor_channel_t channel,
                              esy_sensor_attribute_t attr, int64_t micro);
/** @brief Query an attribute; same validation as set, output changes only on OK. */
esy_err_t esy_sensor_attr_get(esy_sensor_t *sensor, esy_sensor_channel_t channel,
                              esy_sensor_attribute_t attr, int64_t *micro);
/**
 * @brief Replace one subscription, or cancel with trigger=NULL and callback=NULL.
 * @param sensor Live handle.
 * @param trigger Copied condition; its channel must be supported, even for DATA_READY.
 * @param callback Required with a trigger; invoked synchronously during fetch.
 * @param arg Borrowed callback context, may be NULL.
 * @return INVALID_ARG for mismatched NULLs/enums, NOT_SUPPORTED for absent channel,
 * STATE if detached, BUSY on reentry, otherwise OK. Cancellation is permanent
 * until explicitly registered again and creates no timer or hardware IRQ.
 */
esy_err_t esy_sensor_trigger_set(esy_sensor_t *sensor,
                                 const esy_sensor_trigger_t *trigger,
                                 esy_sensor_event_fn callback, void *arg);

#ifdef __cplusplus
}
#endif
#endif /* ESY_SENSOR_H */
