/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/input/esy_input.h"

#include <string.h>

static const int8_t k_gray_edges[16] = {
    0, 1, -1, 0,
    -1, 0, 0, 1,
    1, 0, 0, -1,
    0, -1, 1, 0,
};

static bool time_valid(bool sampled, uint32_t previous, uint32_t now) {
  return !sampled || (uint32_t)(now - previous) <= (uint32_t)INT32_MAX;
}

static void button_emit(esy_input_button_t *button, esy_input_event_type_t type,
                        int32_t value, uint32_t now_ms) {
  const esy_input_event_t event = {
      .type = type,
      .source_id = button->config.source_id,
      .code = button->config.code,
      .value = value,
      .timestamp_ms = now_ms,
  };
  button->config.on_event(&event, button->config.event_arg);
}

esy_err_t esy_input_button_init(esy_input_button_t *button,
                              const esy_input_button_config_t *config) {
  if (button == NULL || config == NULL || config->on_event == NULL ||
      config->debounce_ms > (uint32_t)INT32_MAX ||
      config->long_press_ms > (uint32_t)INT32_MAX ||
      config->double_click_ms > (uint32_t)INT32_MAX) {
    return ESY_ERR_INVALID_ARG;
  }
  if (button->owner != NULL) return ESY_ERR_STATE;

  *button = (esy_input_button_t){.owner = button, .config = *config};
  return ESY_OK;
}

esy_err_t esy_input_button_deinit(esy_input_button_t *button) {
  if (button == NULL) return ESY_ERR_INVALID_ARG;
  if (button->owner != NULL && button->owner != button) return ESY_ERR_STATE;
  if (button->busy) return ESY_ERR_BUSY;
  memset(button, 0, sizeof(*button));
  return ESY_OK;
}

static void button_expire_timers(esy_input_button_t *button, uint32_t now_ms) {
  if (button->click_pending &&
      (uint32_t)(now_ms - button->released_since_ms) >=
          button->config.double_click_ms) {
    button->click_pending = false;
    button_emit(button, ESY_INPUT_EVENT_CLICK, 1, now_ms);
  }
  if (button->long_pending &&
      (uint32_t)(now_ms - button->pressed_since_ms) >=
          button->config.long_press_ms) {
    button->long_pending = false;
    button->long_emitted = true;
    button_emit(button, ESY_INPUT_EVENT_LONG_PRESS, 1, now_ms);
  }
}

static void button_commit_level(esy_input_button_t *button, uint32_t now_ms) {
  button->debouncing = false;
  button->stable_pressed = button->candidate_pressed;
  if (button->stable_pressed) {
    button->pressed_since_ms = now_ms;
    button->long_pending = button->config.long_press_ms != 0u;
    button->long_emitted = false;
    button_emit(button, ESY_INPUT_EVENT_KEY, 1, now_ms);
    return;
  }

  button->long_pending = false;
  button_emit(button, ESY_INPUT_EVENT_KEY, 0, now_ms);
  if (button->long_emitted) return;
  if (button->config.double_click_ms == 0u) {
    button_emit(button, ESY_INPUT_EVENT_CLICK, 1, now_ms);
  } else if (button->click_pending) {
    button->click_pending = false;
    button_emit(button, ESY_INPUT_EVENT_DOUBLE_CLICK, 2, now_ms);
  } else {
    button->released_since_ms = now_ms;
    button->click_pending = true;
  }
}

esy_err_t esy_input_button_sample(esy_input_button_t *button, bool pressed,
                                uint32_t now_ms) {
  if (button == NULL) return ESY_ERR_INVALID_ARG;
  if (button->owner != button) return ESY_ERR_STATE;
  if (button->busy) return ESY_ERR_BUSY;
  if (!time_valid(button->sampled, button->last_sample_ms, now_ms)) {
    return ESY_ERR_STATE;
  }

  button->busy = true;
  button->sampled = true;
  button->last_sample_ms = now_ms;
  button_expire_timers(button, now_ms);
  if (button->candidate_pressed != pressed) {
    button->candidate_pressed = pressed;
    button->candidate_since_ms = now_ms;
    button->debouncing = pressed != button->stable_pressed;
  }
  if (button->debouncing &&
      (uint32_t)(now_ms - button->candidate_since_ms) >=
          button->config.debounce_ms) {
    button_commit_level(button, now_ms);
  }
  button->busy = false;
  return ESY_OK;
}

esy_err_t esy_input_encoder_init(esy_input_encoder_t *encoder,
                               const esy_input_encoder_config_t *config) {
  if (encoder == NULL || config == NULL || config->on_event == NULL ||
      (config->edges_per_step != 1u && config->edges_per_step != 2u &&
       config->edges_per_step != 4u)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (encoder->owner != NULL) return ESY_ERR_STATE;

  *encoder = (esy_input_encoder_t){.owner = encoder, .config = *config};
  return ESY_OK;
}

esy_err_t esy_input_encoder_deinit(esy_input_encoder_t *encoder) {
  if (encoder == NULL) return ESY_ERR_INVALID_ARG;
  if (encoder->owner != NULL && encoder->owner != encoder) return ESY_ERR_STATE;
  if (encoder->busy) return ESY_ERR_BUSY;
  memset(encoder, 0, sizeof(*encoder));
  return ESY_OK;
}

esy_err_t esy_input_encoder_sample(esy_input_encoder_t *encoder, uint8_t ab,
                                 uint32_t now_ms) {
  if (encoder == NULL || ab > 3u) return ESY_ERR_INVALID_ARG;
  if (encoder->owner != encoder) return ESY_ERR_STATE;
  if (encoder->busy) return ESY_ERR_BUSY;
  if (!time_valid(encoder->sampled, encoder->last_sample_ms, now_ms)) {
    return ESY_ERR_STATE;
  }

  encoder->last_sample_ms = now_ms;
  if (!encoder->sampled) {
    encoder->sampled = true;
    encoder->previous_ab = ab;
    return ESY_OK;
  }
  const uint8_t previous = encoder->previous_ab;
  encoder->previous_ab = ab;
  if ((previous ^ ab) == 3u) {
    encoder->accumulated_edges = 0;
    return ESY_ERR_IO;
  }

  encoder->accumulated_edges += k_gray_edges[(previous << 2u) | ab];
  const int8_t threshold = (int8_t)encoder->config.edges_per_step;
  if (encoder->accumulated_edges >= threshold ||
      encoder->accumulated_edges <= -threshold) {
    int32_t direction = encoder->accumulated_edges > 0 ? 1 : -1;
    if (encoder->config.inverted) direction = -direction;
    encoder->accumulated_edges = 0;
    const esy_input_event_t event = {
        .type = ESY_INPUT_EVENT_RELATIVE,
        .source_id = encoder->config.source_id,
        .code = encoder->config.code,
        .value = direction,
        .timestamp_ms = now_ms,
    };
    encoder->busy = true;
    encoder->config.on_event(&event, encoder->config.event_arg);
    encoder->busy = false;
  }
  return ESY_OK;
}
