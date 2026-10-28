/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/logger/esy_logger.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <stdatomic.h>

#define ESY_LOGGER_MAX_LINE 1024u

typedef struct {
  atomic_int filter_level;
  atomic_flag transaction;
  esy_logger_backend_binding_t *bindings;
  size_t binding_count;
} esy_logger_context_t;

static esy_logger_context_t s_ctx = {.transaction = ATOMIC_FLAG_INIT};

esy_err_t esy_logger_init(const esy_logger_config_t *cfg) {
  if (cfg == NULL || cfg->static_resources == NULL ||
      cfg->static_resources->bindings == NULL ||
      cfg->static_resources->binding_count == 0u ||
      (unsigned)cfg->filter_level > ESY_LOG_LEVEL_VERBOSE) {
    return ESY_ERR_INVALID_ARG;
  }

  esy_logger_backend_binding_t *bindings = cfg->static_resources->bindings;
  size_t count = cfg->static_resources->binding_count;
  uintptr_t begin = (uintptr_t)bindings;
  if (count > SIZE_MAX / sizeof(*bindings) ||
      count > (UINTPTR_MAX - begin) / sizeof(*bindings)) {
    return ESY_ERR_INVALID_ARG;
  }

  if (atomic_flag_test_and_set_explicit(&s_ctx.transaction, memory_order_acquire)) {
    return ESY_ERR_BUSY;
  }

  esy_err_t result = ESY_ERR_INVALID_ARG;
  uintptr_t end = begin + count * sizeof(*bindings);
  if (s_ctx.bindings != NULL) {
    uintptr_t old_begin = (uintptr_t)s_ctx.bindings;
    uintptr_t old_end = old_begin + s_ctx.binding_count * sizeof(*bindings);
    if (begin < old_end && old_begin < end) goto done;
  }

  bool active = false;
  for (size_t i = 0u; i < count; ++i) {
    const esy_logger_backend_t *backend = bindings[i].backend;
    if ((unsigned)bindings[i].filter_level > ESY_LOG_LEVEL_VERBOSE) goto done;
    if (backend == NULL) continue;
    if (backend->acquire_buffer == NULL || backend->submit_buffer == NULL) {
      goto done;
    }
    for (size_t j = 0u; j < i; ++j) {
      if (bindings[j].backend == backend) goto done;
    }
    active = true;
  }
  if (!active) goto done;

  s_ctx.bindings = bindings;
  s_ctx.binding_count = count;
  atomic_store_explicit(&s_ctx.filter_level, (int)cfg->filter_level,
                        memory_order_relaxed);
  result = ESY_OK;
done:
  atomic_flag_clear_explicit(&s_ctx.transaction, memory_order_release);
  return result;
}

esy_err_t esy_logger_detach(const esy_logger_backend_t *backend) {
  if (backend == NULL) return ESY_ERR_INVALID_ARG;
  if (atomic_flag_test_and_set_explicit(&s_ctx.transaction, memory_order_acquire)) {
    return ESY_ERR_BUSY;
  }
  bool active = false;
  for (size_t i = 0u; i < s_ctx.binding_count; ++i) {
    if (s_ctx.bindings[i].backend == backend) {
      s_ctx.bindings[i].backend = NULL;
    }
    if (s_ctx.bindings[i].backend != NULL) active = true;
  }
  if (!active) {
    s_ctx.bindings = NULL;
    s_ctx.binding_count = 0u;
  }
  atomic_flag_clear_explicit(&s_ctx.transaction, memory_order_release);
  return ESY_OK;
}

void esy_logger_set_filter_level(esy_log_level_t level) {
  if ((unsigned)level > ESY_LOG_LEVEL_VERBOSE) {
    return;
  }

  atomic_store_explicit(&s_ctx.filter_level, (int)level, memory_order_relaxed);
}

static void submit_line(const esy_logger_backend_t *backend, const char *line,
                        size_t len) {
  size_t sent = 0u;
  while (sent < len) {
    uint8_t *buf = NULL;
    size_t buf_len = 0u;
    esy_err_t ret = backend->acquire_buffer((void *)backend, &buf, &buf_len);
    if (ret != ESY_OK || buf == NULL || buf_len == 0u) break;

    size_t remain = len - sent;
    size_t chunk = remain < buf_len ? remain : buf_len;
    memcpy(buf, (const uint8_t *)line + sent, chunk);
    ret = backend->submit_buffer((void *)backend, buf, chunk);
    if (ret != ESY_OK) break;
    sent += chunk;
  }
}

void esy_log(const char* tag, esy_log_level_t level, const char *fmt, ...) {
  if ((unsigned)level > ESY_LOG_LEVEL_VERBOSE) return;
  if ((int)level > atomic_load_explicit(&s_ctx.filter_level, memory_order_relaxed)) return;
  /* A nonblocking transaction guard serializes whole lines and backend binding.
   * It does not mask interrupts; contention/reentrant logging drops a line. */
  if (atomic_flag_test_and_set_explicit(&s_ctx.transaction, memory_order_acquire)) return;
  bool eligible = false;
  for (size_t i = 0u; !eligible && i < s_ctx.binding_count; ++i) {
    eligible = s_ctx.bindings[i].backend != NULL &&
               level <= s_ctx.bindings[i].filter_level;
  }
  if (!eligible) goto done;

  const char *level_str = "";

  switch (level) {
    case ESY_LOG_LEVEL_ERROR:   level_str = "E"; break;
    case ESY_LOG_LEVEL_WARN:    level_str = "W"; break;
    case ESY_LOG_LEVEL_INFO:    level_str = "I"; break;
    case ESY_LOG_LEVEL_DEBUG:   level_str = "D"; break;
    case ESY_LOG_LEVEL_VERBOSE: level_str = "V"; break;
    default:                    level_str = "U"; break;
  }

  /* 1) Format the whole line into a bounded stack buffer (no malloc). */
  char line[ESY_LOGGER_MAX_LINE];
  size_t cap = sizeof(line);
  size_t len = 0;

  int hn = snprintf(line, cap, "[%s][%s] ", tag ? tag : "", level_str);
  if (hn < 0) goto done;

  len = (size_t)hn;
  if (len >= cap) {
    /* header already overflowed; clamp */
    len = cap - 1u;
    line[len] = '\0';
  }

  if (len < cap - 1u) {
    va_list args;
    va_start(args, fmt);
    int bn = vsnprintf(line + len, cap - len, fmt ? fmt : "", args);
    va_end(args);

    if (bn < 0) {
      /* keep header only */
      line[len] = '\0';
    } else {
      size_t want = (size_t)bn;
      size_t avail = cap - len;

      /* vsnprintf returns would-have-written (excluding '\0') */
      if (want >= avail) {
        /* truncated */
        len = cap - 1u;
        line[len] = '\0';
      } else {
        len += want;
      }
    }
  }

  /* Append a single '\n' if there is room; otherwise overwrite last char with '\n'. */
  if (cap >= 2u) {
    if (len < cap - 1u) {
      line[len++] = '\n';
      line[len] = '\0';
    } else {
      /* buffer full: ensure it ends with '\n' */
      line[cap - 2u] = '\n';
      line[cap - 1u] = '\0';
      len = cap - 1u;
    }
  }

  /* 2) Reuse the formatted line, with independent backend-sized chunks. */
  for (size_t i = 0u; i < s_ctx.binding_count; ++i) {
    const esy_logger_backend_binding_t *binding = &s_ctx.bindings[i];
    if (binding->backend != NULL && level <= binding->filter_level) {
      submit_line(binding->backend, line, len);
    }
  }
done:
  atomic_flag_clear_explicit(&s_ctx.transaction, memory_order_release);
}
