/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/sensor/esy_sensor.h"

#include <string.h>

#define SENSOR_CHANNELS_MASK ((UINT32_C(1) << ESY_SENSOR_CHAN_COUNT) - UINT32_C(1))

static const esy_sensor_unit_t k_units[ESY_SENSOR_CHAN_COUNT] = {
    ESY_SENSOR_UNIT_CELSIUS, ESY_SENSOR_UNIT_PERCENT, ESY_SENSOR_UNIT_PASCAL,
    ESY_SENSOR_UNIT_METRE_PER_SECOND_SQUARED,
    ESY_SENSOR_UNIT_METRE_PER_SECOND_SQUARED,
    ESY_SENSOR_UNIT_METRE_PER_SECOND_SQUARED,
    ESY_SENSOR_UNIT_RADIAN_PER_SECOND, ESY_SENSOR_UNIT_RADIAN_PER_SECOND,
    ESY_SENSOR_UNIT_RADIAN_PER_SECOND, ESY_SENSOR_UNIT_VOLT,
    ESY_SENSOR_UNIT_VOLT, ESY_SENSOR_UNIT_AMPERE, ESY_SENSOR_UNIT_WATT};

static esy_err_t validate_sensor(const esy_sensor_t *sensor) {
  if (sensor == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (sensor->busy) {
    return ESY_ERR_BUSY;
  }
  return sensor->driver == NULL ? ESY_ERR_STATE : ESY_OK;
}

static esy_err_t validate_channel(const esy_sensor_t *sensor,
                                  esy_sensor_channel_t channel) {
  esy_err_t err = validate_sensor(sensor);
  if (err != ESY_OK) {
    return err;
  }
  if ((unsigned)channel >= ESY_SENSOR_CHAN_COUNT) {
    return ESY_ERR_INVALID_ARG;
  }
  return (sensor->driver->channels & ESY_SENSOR_CHANNEL_BIT(channel)) == 0u
             ? ESY_ERR_NOT_SUPPORTED : ESY_OK;
}

esy_err_t esy_sensor_init(esy_sensor_t *sensor, const esy_sensor_driver_t *driver,
                          void *ctx) {
  if (sensor == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (sensor->busy) {
    return ESY_ERR_BUSY;
  }
  if (sensor->driver != NULL) {
    return ESY_ERR_STATE;
  }
  if (driver == NULL || driver->fetch == NULL || ctx == NULL ||
      driver->channels == 0u || (driver->channels & ~SENSOR_CHANNELS_MASK) != 0u) {
    return ESY_ERR_INVALID_ARG;
  }
  sensor->driver = driver;
  sensor->ctx = ctx;
  sensor->valid = false;
  sensor->on_event = NULL;
  sensor->event_arg = NULL;
  sensor->triggered = false;
  return ESY_OK;
}

esy_err_t esy_sensor_deinit(esy_sensor_t *sensor) {
  esy_err_t err;
  if (sensor == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  if (sensor->busy) {
    return ESY_ERR_BUSY;
  }
  if (sensor->driver != NULL && sensor->driver->deinit != NULL) {
    sensor->busy = true;
    err = sensor->driver->deinit(sensor->ctx);
    sensor->busy = false;
    if (err != ESY_OK) {
      return err;
    }
  }
  memset(sensor, 0, sizeof(*sensor));
  return ESY_OK;
}

esy_err_t esy_sensor_sample_fetch(esy_sensor_t *sensor) {
  esy_sensor_sample_t sample = {0};
  esy_err_t err = validate_sensor(sensor);
  bool active;
  bool notify;
  if (err != ESY_OK) {
    return err;
  }
  sensor->busy = true;
  sensor->valid = false;
  err = sensor->driver->fetch(sensor->ctx, &sample);
  if (err == ESY_OK && sample.channels != sensor->driver->channels) {
    err = ESY_ERR_IO;
  }
  if (err == ESY_OK) {
    sensor->sample = sample;
    sensor->valid = true;
    if (sensor->on_event != NULL) {
      active = sensor->trigger.type == ESY_SENSOR_TRIGGER_DATA_READY ||
          (sensor->trigger.type == ESY_SENSOR_TRIGGER_THRESHOLD_HIGH &&
           sample.micro[sensor->trigger.channel] >= sensor->trigger.threshold_micro) ||
          (sensor->trigger.type == ESY_SENSOR_TRIGGER_THRESHOLD_LOW &&
           sample.micro[sensor->trigger.channel] <= sensor->trigger.threshold_micro);
      notify = active &&
          (sensor->trigger.type == ESY_SENSOR_TRIGGER_DATA_READY || !sensor->triggered);
      sensor->triggered = active;
      if (notify) {
        sensor->on_event(&sensor->trigger, &sensor->sample, sensor->event_arg);
      }
    }
  }
  sensor->busy = false;
  return err;
}

esy_err_t esy_sensor_get_channels(const esy_sensor_t *sensor, uint32_t *channels) {
  esy_err_t err;
  if (channels == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  err = validate_sensor(sensor);
  if (err == ESY_OK) {
    *channels = sensor->driver->channels;
  }
  return err;
}

esy_err_t esy_sensor_channel_get(const esy_sensor_t *sensor,
                                 esy_sensor_channel_t channel, int64_t *micro) {
  esy_err_t err;
  if (micro == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  err = validate_channel(sensor, channel);
  if (err != ESY_OK) {
    return err;
  }
  if (!sensor->valid) {
    return ESY_ERR_STATE;
  }
  *micro = sensor->sample.micro[channel];
  return ESY_OK;
}

esy_err_t esy_sensor_channel_unit(esy_sensor_channel_t channel, esy_sensor_unit_t *unit) {
  if ((unsigned)channel >= ESY_SENSOR_CHAN_COUNT || unit == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  *unit = k_units[channel];
  return ESY_OK;
}

esy_err_t esy_sensor_attr_set(esy_sensor_t *sensor, esy_sensor_channel_t channel,
                              esy_sensor_attribute_t attr, int64_t micro) {
  esy_err_t err = validate_channel(sensor, channel);
  if (err != ESY_OK) {
    return err;
  }
  if ((unsigned)attr > ESY_SENSOR_ATTR_FULL_SCALE || micro <= 0) {
    return ESY_ERR_INVALID_ARG;
  }
  if (sensor->driver->attr_set == NULL) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  sensor->busy = true;
  sensor->valid = false;
  err = sensor->driver->attr_set(sensor->ctx, channel, attr, micro);
  sensor->busy = false;
  return err;
}

esy_err_t esy_sensor_attr_get(esy_sensor_t *sensor, esy_sensor_channel_t channel,
                              esy_sensor_attribute_t attr, int64_t *micro) {
  int64_t value = 0;
  esy_err_t err;
  if (micro == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  err = validate_channel(sensor, channel);
  if (err != ESY_OK) {
    return err;
  }
  if ((unsigned)attr > ESY_SENSOR_ATTR_FULL_SCALE) {
    return ESY_ERR_INVALID_ARG;
  }
  if (sensor->driver->attr_get == NULL) {
    return ESY_ERR_NOT_SUPPORTED;
  }
  sensor->busy = true;
  err = sensor->driver->attr_get(sensor->ctx, channel, attr, &value);
  sensor->busy = false;
  if (err == ESY_OK) {
    *micro = value;
  }
  return err;
}

esy_err_t esy_sensor_trigger_set(esy_sensor_t *sensor,
                                 const esy_sensor_trigger_t *trigger,
                                 esy_sensor_event_fn callback, void *arg) {
  esy_err_t err = validate_sensor(sensor);
  if (err != ESY_OK) {
    return err;
  }
  if ((trigger == NULL) != (callback == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (trigger != NULL) {
    if ((unsigned)trigger->type > ESY_SENSOR_TRIGGER_THRESHOLD_LOW) {
      return ESY_ERR_INVALID_ARG;
    }
    err = validate_channel(sensor, trigger->channel);
    if (err != ESY_OK) {
      return err;
    }
    sensor->trigger = *trigger;
  }
  sensor->on_event = callback;
  sensor->event_arg = callback == NULL ? NULL : arg;
  sensor->triggered = false;
  return ESY_OK;
}
