/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "native_test_support.h"
#include "queue_test_support.h"
#include <assert.h>
#include <setjmp.h>
#include <string.h>

typedef struct {
  native_test_support_ctx_t public;
  struct {int operation; TimerHandle_t timer; PendedFunction_t fence; void *arg;} commands[32];
  unsigned queued;
  jmp_buf worker_return;
  bool worker_running;
} native_test_support_private_ctx_t;
static native_test_support_private_ctx_t s_ctx;
void native_test_support_reset(void) {memset(&s_ctx, 0, sizeof(s_ctx));}
native_test_support_ctx_t *native_test_support_get(void) {return &s_ctx.public;}
static void worker_block(void) {
  if (s_ctx.worker_running) longjmp(s_ctx.worker_return, 1);
}
void native_run_worker(void) {
  StaticTask_t *task = s_ctx.public.last_task;
  if (!task || task->state == eSuspended || task->state == eDeleted) return;
  TaskHandle_t previous = s_ctx.public.current;
  s_ctx.public.current = task;
  s_ctx.worker_running = true;
  queue_test_support_get_ctx()->on_empty = worker_block;
  if (setjmp(s_ctx.worker_return) == 0) task->entry(task->arg);
  s_ctx.worker_running = false;
  s_ctx.public.current = previous;
  queue_test_support_get_ctx()->on_empty = NULL;
}
TaskHandle_t xTaskCreateStatic(void (*entry)(void *), const char *name,
    uint32_t depth, void *arg, UBaseType_t priority, StackType_t *stack, StaticTask_t *storage) {
  (void)name; (void)depth; (void)priority; (void)stack;
  if (s_ctx.public.create_fail) return NULL;
  *storage = (StaticTask_t){entry, arg, eReady};
  s_ctx.public.last_task = storage;
  return storage;
}
void vTaskDelete(TaskHandle_t task) {((StaticTask_t *)task)->state = eDeleted; ++s_ctx.public.deletes;}
void vTaskSuspend(TaskHandle_t task) {
  StaticTask_t *target = task ? task : s_ctx.public.current;
  target->state = eSuspended;
  worker_block();
}
eTaskState eTaskGetState(TaskHandle_t task) {return ((StaticTask_t *)task)->state;}
void vTaskDelay(TickType_t ticks) {
  assert(!s_ctx.public.critical_depth);
  s_ctx.public.ticks = ticks;
  if (s_ctx.public.on_delay) s_ctx.public.on_delay();
}
void vTaskDelayUntil(TickType_t *previous, TickType_t period) {*previous += period; s_ctx.public.period = period;}
TickType_t xTaskGetTickCount(void) {return s_ctx.public.ticks;}
TaskHandle_t xTaskGetCurrentTaskHandle(void) {return s_ctx.public.current;}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) {(void)task; return 42;}
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value, eNotifyAction action) {
  (void)task; (void)value; s_ctx.public.action = action; return pdPASS;
}
BaseType_t xTaskNotifyWait(uint32_t entry, uint32_t exit, uint32_t *value, TickType_t ticks) {
  (void)entry; (void)exit; *value = 42; s_ctx.public.ticks = ticks; return pdPASS;
}
BaseType_t xTaskNotifyStateClear(TaskHandle_t task) {(void)task; return pdPASS;}
BaseType_t xTaskNotifyGive(TaskHandle_t task) {(void)task; return pdPASS;}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks) {(void)clear; s_ctx.public.ticks = ticks; return 1;}
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *woken) {(void)task; *woken = pdTRUE;}
BaseType_t xTaskNotifyFromISR(TaskHandle_t task, uint32_t value, eNotifyAction action, BaseType_t *woken) {
  *woken = pdTRUE; return xTaskNotify(task, value, action);
}
uint32_t native_enter(int isr, void *mux) {
  assert((bool)isr == s_ctx.public.isr);
  ++s_ctx.public.critical_depth;
  if (isr) {++s_ctx.public.isr_entries; s_ctx.public.isr_mux = mux;}
  else {++s_ctx.public.task_entries; s_ctx.public.task_mux = mux;}
  return 7;
}
void native_exit(int isr, void *mux, uint32_t key) {
  (void)mux; (void)key;
  assert((bool)isr == s_ctx.public.isr && s_ctx.public.critical_depth);
  --s_ctx.public.critical_depth;
  if (!s_ctx.public.critical_depth && s_ctx.public.on_unlock) {
    void (*hook)(void) = s_ctx.public.on_unlock;
    s_ctx.public.on_unlock = NULL;
    hook();
  }
}
void native_yield(void) {++s_ctx.public.yields;}
bool esy_hal_system_is_in_isr(void) {return s_ctx.public.isr;}
int xPortInIsrContext(void) {return s_ctx.public.isr ? 1 : 0;}
void os_fixture_context_irq_disable(void) {s_ctx.public.local_irq_mask = 7u;}
void os_fixture_context_irq_enable(void) {s_ctx.public.local_irq_mask = 0u;}
uint32_t os_fixture_context_irq_save(void) {
  uint32_t previous = s_ctx.public.local_irq_mask;
  os_fixture_context_irq_disable();
  return previous;
}
void os_fixture_context_irq_restore(uint32_t key) {s_ctx.public.local_irq_mask = key;}
TimerHandle_t xTimerCreateStatic(const char *name, TickType_t period, BaseType_t reload,
    void *id, void (*callback)(TimerHandle_t), StaticTimer_t *storage) {
  (void)name;
  if (s_ctx.public.create_fail) return NULL;
  *storage = (StaticTimer_t){id, callback, false, false, reload != 0};
  s_ctx.public.last_timer = storage;
  s_ctx.public.period = period;
  return storage;
}
void *pvTimerGetTimerID(TimerHandle_t timer) {return timer->id;}
static BaseType_t command(TimerHandle_t timer, TickType_t ticks, int operation) {
  assert(ticks == 0 && !timer->deleted);
  if (s_ctx.public.command_fail || s_ctx.queued == 32) return pdFALSE;
  unsigned i = s_ctx.queued++;
  s_ctx.commands[i].operation = operation;
  s_ctx.commands[i].timer = timer;
  ++s_ctx.public.command_count;
  return pdPASS;
}
BaseType_t xTimerStart(TimerHandle_t timer, TickType_t ticks) {return command(timer, ticks, 1);}
BaseType_t xTimerStop(TimerHandle_t timer, TickType_t ticks) {return command(timer, ticks, 2);}
BaseType_t xTimerDelete(TimerHandle_t timer, TickType_t ticks) {return command(timer, ticks, 3);}
BaseType_t xTimerPendFunctionCall(PendedFunction_t function, void *arg, uint32_t value, TickType_t ticks) {
  assert(ticks == 0 && value == 0);
  if (s_ctx.public.fence_fail || s_ctx.queued == 32) return pdFALSE;
  unsigned i = s_ctx.queued++;
  s_ctx.commands[i].operation = 4;
  s_ctx.commands[i].fence = function;
  s_ctx.commands[i].arg = arg;
  ++s_ctx.public.fence_count;
  return pdPASS;
}
void native_drain_commands(void) {
  for (unsigned i = 0; i < s_ctx.queued; ++i) {
    TimerHandle_t timer = s_ctx.commands[i].timer;
    switch (s_ctx.commands[i].operation) {
    case 1: timer->active = true; break;
    case 2: timer->active = false; break;
    case 3: timer->active = false; timer->deleted = true; break;
    case 4: s_ctx.commands[i].fence(s_ctx.commands[i].arg, 0); break;
    }
  }
  s_ctx.queued = 0;
}
void native_fire_timer(void) {
  TimerHandle_t timer = s_ctx.public.last_timer;
  if (timer && timer->active && !timer->deleted) {
    if (!timer->reload) timer->active = false;
    assert(!s_ctx.public.critical_depth);
    timer->callback(timer);
  }
}
SemaphoreHandle_t xSemaphoreCreateCountingStatic(UBaseType_t max, UBaseType_t initial, StaticSemaphore_t *storage) {
  if (s_ctx.public.create_fail) return NULL;
  *storage = (StaticSemaphore_t){initial, max}; return storage;
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) {return xSemaphoreCreateCountingStatic(1, 1, storage);}
void vSemaphoreDelete(SemaphoreHandle_t sem) {(void)sem; ++s_ctx.public.deletes;}
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks) {
  s_ctx.public.ticks = ticks;
  if (!sem->count) return pdFALSE;
  --sem->count; return pdPASS;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
  if (sem->count == sem->max) return pdFALSE;
  ++sem->count; return pdPASS;
}
BaseType_t xSemaphoreTakeFromISR(SemaphoreHandle_t sem, BaseType_t *woken) {*woken = pdTRUE; return xSemaphoreTake(sem, 0);}
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *woken) {*woken = pdTRUE; return xSemaphoreGive(sem);}
StreamBufferHandle_t xStreamBufferCreateStatic(size_t capacity, size_t trigger, uint8_t *data, StaticStreamBuffer_t *storage) {
  (void)trigger;
  if (s_ctx.public.create_fail) return NULL;
  *storage = (StaticStreamBuffer_t){data, capacity - 1, 0}; return storage;
}
void vStreamBufferDelete(StreamBufferHandle_t buffer) {(void)buffer; ++s_ctx.public.deletes;}
BaseType_t xStreamBufferReset(StreamBufferHandle_t buffer) {
  if (s_ctx.public.reset_fail) return pdFALSE;
  buffer->used = 0; return pdPASS;
}
size_t xStreamBufferSend(StreamBufferHandle_t buffer, const void *data, size_t size, TickType_t ticks) {
  s_ctx.public.ticks = ticks;
  if (size > buffer->capacity - buffer->used) size = buffer->capacity - buffer->used;
  memcpy(buffer->data + buffer->used, data, size); buffer->used += size; return size;
}
size_t xStreamBufferReceive(StreamBufferHandle_t buffer, void *data, size_t size, TickType_t ticks) {
  s_ctx.public.ticks = ticks;
  if (size > buffer->used) size = buffer->used;
  memcpy(data, buffer->data, size); buffer->used -= size;
  memmove(buffer->data, buffer->data + size, buffer->used); return size;
}
size_t xStreamBufferSendFromISR(StreamBufferHandle_t buffer, const void *data, size_t size, BaseType_t *woken) {*woken = pdTRUE; return xStreamBufferSend(buffer, data, size, 0);}
size_t xStreamBufferReceiveFromISR(StreamBufferHandle_t buffer, void *data, size_t size, BaseType_t *woken) {*woken = pdTRUE; return xStreamBufferReceive(buffer, data, size, 0);}
BaseType_t xStreamBufferSetTriggerLevel(StreamBufferHandle_t buffer, size_t trigger) {return trigger <= buffer->capacity ? pdPASS : pdFALSE;}
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t buffer) {return buffer->used;}
size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t buffer) {return buffer->capacity - buffer->used;}
MessageBufferHandle_t xMessageBufferCreateStatic(size_t capacity, uint8_t *data, StaticMessageBuffer_t *storage) {return xStreamBufferCreateStatic(capacity, 1, data, storage);}
size_t xMessageBufferSend(MessageBufferHandle_t buffer, const void *data, size_t size, TickType_t ticks) {
  s_ctx.public.ticks = ticks;
  if (size + sizeof(size) > buffer->capacity - buffer->used) return 0;
  xStreamBufferSend(buffer, &size, sizeof(size), ticks);
  return xStreamBufferSend(buffer, data, size, ticks);
}
size_t xStreamBufferNextMessageLengthBytes(MessageBufferHandle_t buffer) {
  size_t size = 0;
  if (buffer->used) memcpy(&size, buffer->data, sizeof(size));
  return size;
}
size_t xMessageBufferReceive(MessageBufferHandle_t buffer, void *data, size_t size, TickType_t ticks) {
  s_ctx.public.ticks = ticks;
  size_t next = xStreamBufferNextMessageLengthBytes(buffer), ignored;
  if (!next || next > size) return 0;
  xStreamBufferReceive(buffer, &ignored, sizeof(ignored), ticks);
  return xStreamBufferReceive(buffer, data, next, ticks);
}
size_t xMessageBufferSendFromISR(MessageBufferHandle_t buffer, const void *data, size_t size, BaseType_t *woken) {*woken = pdTRUE; return xMessageBufferSend(buffer, data, size, 0);}
size_t xMessageBufferReceiveFromISR(MessageBufferHandle_t buffer, void *data, size_t size, BaseType_t *woken) {*woken = pdTRUE; return xMessageBufferReceive(buffer, data, size, 0);}
