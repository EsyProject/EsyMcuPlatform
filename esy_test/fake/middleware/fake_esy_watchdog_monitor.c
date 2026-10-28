/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_esy_watchdog_monitor.h"

#include <stddef.h>
#include <string.h>

#define FAKE_ESY_WATCHDOG_REPORT_MAGIC 0x45535957u

static fake_esy_watchdog_monitor_ctx_t s_ctx;

static void fake_esy_watchdog_monitor_record_report(
    const esy_watchdog_report_t *report) {
  if (report != NULL) {
    s_ctx.last_report = *report;
  }
}

void fake_esy_watchdog_monitor_reset(void) {
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.init_result = ESY_OK;
  s_ctx.deadline_result = ESY_ERR_NOT_SUPPORTED;
  s_ctx.start_result = ESY_OK;
  s_ctx.register_result = ESY_OK;
  s_ctx.enable_result = ESY_OK;
  s_ctx.get_status_result = ESY_OK;
  s_ctx.get_task_count_result = ESY_OK;
  s_ctx.task_alive = true;
  s_ctx.next_report.generation = 1u;
  s_ctx.next_report.magic = FAKE_ESY_WATCHDOG_REPORT_MAGIC;
}

fake_esy_watchdog_monitor_ctx_t *fake_esy_watchdog_monitor_get_ctx(void) {
  return &s_ctx;
}

void esy_watchdog_monitor_default_cfg(esy_watchdog_monitor_config_t *cfg) {
  if (cfg != NULL) {
    memset(cfg, 0, sizeof(*cfg));
  }
}

esy_err_t esy_watchdog_monitor_init(
    const esy_watchdog_monitor_config_t *cfg) {
  s_ctx.init_count++;
  if (cfg != NULL) {
    s_ctx.last_init_config = *cfg;
    s_ctx.has_last_init_config = true;
  }
  return s_ctx.init_result;
}

esy_err_t esy_watchdog_monitor_start(void) {
  s_ctx.start_count++;
  return s_ctx.start_result;
}

esy_err_t esy_watchdog_monitor_stop(void) {
  s_ctx.stop_count++;
  return s_ctx.stop_result;
}

esy_err_t esy_watchdog_monitor_deinit(void) {
  s_ctx.deinit_count++;
  return s_ctx.deinit_result;
}

esy_err_t esy_watchdog_monitor_register(
    const char *task_name, esy_watchdog_report_t *out_report) {
  s_ctx.register_count++;
  s_ctx.last_task_name = task_name;
  if ((task_name == NULL) || (out_report == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (s_ctx.register_result != ESY_OK) {
    return s_ctx.register_result;
  }

  *out_report = s_ctx.next_report;
  s_ctx.last_report = *out_report;
  s_ctx.next_report.slot_index++;
  s_ctx.registered_task_count++;
  return ESY_OK;
}

void esy_watchdog_monitor_unregister(esy_watchdog_report_t *report) {
  s_ctx.unregister_count++;
  fake_esy_watchdog_monitor_record_report(report);
  if (report != NULL) {
    memset(report, 0, sizeof(*report));
    if (s_ctx.registered_task_count > 0u) {
      s_ctx.registered_task_count--;
    }
  }
}

esy_err_t esy_watchdog_monitor_enable_task_check(
    const esy_watchdog_report_t *report) {
  s_ctx.enable_count++;
  fake_esy_watchdog_monitor_record_report(report);
  if (report == NULL) {
    return ESY_ERR_INVALID_ARG;
  }
  return s_ctx.enable_result;
}

void esy_watchdog_monitor_disable_task_check(
    const esy_watchdog_report_t *report) {
  s_ctx.disable_count++;
  fake_esy_watchdog_monitor_record_report(report);
}

esy_err_t esy_watchdog_monitor_get_task_status(
    const esy_watchdog_report_t *report, bool *out_alive) {
  s_ctx.get_status_count++;
  fake_esy_watchdog_monitor_record_report(report);
  if ((report == NULL) || (out_alive == NULL)) {
    return ESY_ERR_INVALID_ARG;
  }
  if (s_ctx.get_status_result == ESY_OK) {
    *out_alive = s_ctx.task_alive;
  }
  return s_ctx.get_status_result;
}

void esy_watchdog_monitor_report(const esy_watchdog_report_t *report) {
  s_ctx.report_count++;
  fake_esy_watchdog_monitor_record_report(report);
}

int32_t esy_watchdog_monitor_get_task_count(void) {
  s_ctx.get_task_count_count++;
  if (s_ctx.get_task_count_result != ESY_OK) {
    return (int32_t)s_ctx.get_task_count_result;
  }
  return (int32_t)s_ctx.registered_task_count;
}

void esy_watchdog_monitor_disable_hw_feed(void) {
  s_ctx.disable_hw_feed_count++;
}

esy_err_t esy_watchdog_monitor_set_alert_cb(esy_watchdog_alert_cb_t cb,
                                       void *user_ctx) {
  s_ctx.set_alert_count++;
  if (s_ctx.set_alert_result != ESY_OK) return s_ctx.set_alert_result;
  s_ctx.alert_cb = cb;
  s_ctx.alert_user_ctx = user_ctx;
  return ESY_OK;
}

esy_err_t esy_watchdog_monitor_get_hard_deadline_atomic(
    const esy_monotonic_clock_t *clock, uint64_t *out_deadline_us) {
  if (clock == NULL || out_deadline_us == NULL) return ESY_ERR_INVALID_ARG;
  s_ctx.deadline_query_count++;
  if (s_ctx.deadline_result != ESY_OK) return s_ctx.deadline_result;
  *out_deadline_us = s_ctx.deadline_us;
  return ESY_OK;
}
