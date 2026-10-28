/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esy_fixture_task.h
 * @brief Task fixture helpers based on fake_esy_os_task and fake_esy_os_queue.
 */

#ifndef ESY_TEST_FIXTURE_TASK_H_
#define ESY_TEST_FIXTURE_TASK_H_

#include <stdbool.h>
#include <stdint.h>

#include "esy_middleware/os/esy_os_task.h"
#include "esy_common/defs/base/esy_common_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset common OS fake state for tasks and queues.
 */
void esy_fixture_reset_os_task_and_queue(void);

/**
 * @brief Ensure a task handle exists.
 *
 * If the input handle is NULL, the helper creates a task. Existing handles are
 * accepted without creating another task.
 * Calls and reset are serialized by the test owner. Failed creation consumes
 * no backing slot and may be retried. Successfully created tasks retain their
 * backing slots until reset; stop all task users before resetting the fixture.
 *
 * @param[in]     task_name    Task name.
 * @param[in]     entry        Task entry.
 * @param[in]     arg          Task argument.
 * @param[in]     stack_size   Stack size in bytes.
 * @param[in]     priority     Task priority.
 * @param[in,out] inout_task   Input/output task handle.
 *
 * @retval ESY_OK Success.
 * @retval ESY_ERR_INVALID_ARG Invalid argument.
 * @retval <0 Task creation failure.
 */
esy_err_t esy_fixture_ensure_task(const char *task_name,
                                  esy_os_task_func_t entry,
                                  void *arg,
                                  uint32_t stack_size,
                                  uint32_t priority,
                                  esy_os_task_t **inout_task);

/**
 * @brief Ensure a worker task with default test parameters.
 *
 * Default parameters:
 * - entry: (esy_os_task_func_t)0x1
 * - arg: NULL
 * - stack_size: 256
 * - priority: 1
 */
esy_err_t esy_fixture_ensure_worker_task(const char *task_name,
                                         esy_os_task_t **inout_task);

/**
 * @brief Query notify_give_count for a task.
 *
 * @param[in]  task       Task handle.
 * @param[out] out_count  Output count.
 *
 * @retval true  Task was found and the count was returned.
 * @retval false Task was not found or an argument was invalid.
 */
bool esy_fixture_get_task_notify_give_count(esy_os_task_t *task,
                                            uint32_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* ESY_TEST_FIXTURE_TASK_H_ */
