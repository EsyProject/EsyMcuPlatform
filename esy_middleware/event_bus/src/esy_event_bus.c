/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esy_middleware/event_bus/esy_event_bus.h"

#include <string.h>
#include "esy_middleware/os/esy_os_queue.h"
#include "esy_middleware/os/esy_os_task.h"

/* Runtime objects overlay caller-owned bytes. Size/alignment alone do not
 * establish a type-based aliasing contract, including with optimized/LTO builds.
 * A new compiler port must provide an equivalent guarantee. */
#if defined(__has_attribute)
#if !__has_attribute(__may_alias__)
#error "Event bus private storage requires compiler support for may_alias"
#endif
#elif !defined(__GNUC__)
#error "Event bus private storage requires compiler support for may_alias"
#endif
#define ESY_EVENT_BUS_MAY_ALIAS __attribute__((__may_alias__))

typedef enum {
  SUBSCRIPTION_FREE = 0,
  SUBSCRIPTION_CREATING,
  SUBSCRIPTION_OPEN,
  SUBSCRIPTION_CLOSING,
  SUBSCRIPTION_RETIRING
} subscription_state_t;

typedef struct ESY_EVENT_BUS_MAY_ALIAS {
  esy_event_bus_subscription_t *subscriptions;
  size_t capacity;
  size_t publishers;
  size_t subscriptions_used;
  bool closing;
} esy_event_bus_ctx_t;

typedef struct ESY_EVENT_BUS_MAY_ALIAS {
  esy_event_bus_t *bus;
  esy_os_queue_t *queue;
  size_t users;
  uint32_t topic_mask;
  subscription_state_t state;
  esy_os_queue_static_storage_t queue_storage;
} subscription_ctx_t;

_Static_assert(sizeof(esy_event_bus_ctx_t) <= ESY_EVENT_BUS_STORAGE_BYTES,
               "Event bus storage budget is too small");
_Static_assert(sizeof(subscription_ctx_t) <= ESY_EVENT_BUS_SUBSCRIPTION_STORAGE_BYTES,
               "Event bus subscription storage budget is too small");
_Static_assert(_Alignof(esy_event_bus_t) >= _Alignof(esy_event_bus_ctx_t),
               "Event bus storage alignment is too small");
_Static_assert(_Alignof(esy_event_bus_subscription_t) >= _Alignof(subscription_ctx_t),
               "Event bus subscription alignment is too small");

static esy_event_bus_ctx_t *bus_context(esy_event_bus_t *bus) {
  return (esy_event_bus_ctx_t *)(void *)bus->storage.bytes;
}

static subscription_ctx_t *subscription_context(esy_event_bus_subscription_t *slot) {
  return (subscription_ctx_t *)(void *)slot->storage.bytes;
}

/* MPSC task/ISR publishers share registry admission and queue-lifetime counts
 * with one task lifecycle owner. These multi-field transactions must be atomic:
 * an OS queue alone cannot prevent its deletion during a pending copy. Use the
 * port's shared task/ISR lock, including SMP serialization. No queue operation,
 * payload copy, allocation, wait or callback runs inside these short sections. */
static uint32_t lock_registry(bool from_isr) {
  if (from_isr) return esy_os_task_enter_critical_from_isr();
  esy_os_task_enter_critical();
  return 0u;
}

static void unlock_registry(bool from_isr, uint32_t key) {
  if (from_isr) esy_os_task_exit_critical_from_isr(key);
  else esy_os_task_exit_critical();
}

esy_err_t esy_event_bus_init(esy_event_bus_t *bus,
                             const esy_event_bus_config_t *config) {
  if (bus == NULL || config == NULL || config->static_resources == NULL ||
      (uintptr_t)bus % _Alignof(esy_event_bus_t) != 0u) {
    return ESY_ERR_INVALID_ARG;
  }
  if (bus->owner != NULL) return ESY_ERR_STATE;
  const esy_event_bus_static_resources_t resources = *config->static_resources;
  if (resources.subscriptions == NULL || resources.subscription_capacity == 0u ||
      resources.subscription_capacity > SIZE_MAX / sizeof(*resources.subscriptions) ||
      (uintptr_t)resources.subscriptions % _Alignof(esy_event_bus_subscription_t) != 0u) {
    return ESY_ERR_INVALID_ARG;
  }

  memset(bus->storage.bytes, 0, sizeof(bus->storage.bytes));
  esy_event_bus_ctx_t *ctx = bus_context(bus);
  ctx->subscriptions = resources.subscriptions;
  ctx->capacity = resources.subscription_capacity;
  for (size_t i = 0u; i < ctx->capacity; ++i) {
    memset(&ctx->subscriptions[i], 0, sizeof(ctx->subscriptions[i]));
    subscription_context(&ctx->subscriptions[i])->bus = bus;
  }
  bus->owner = bus;
  return ESY_OK;
}

esy_err_t esy_event_bus_deinit(esy_event_bus_t *bus) {
  if (bus == NULL) return ESY_ERR_INVALID_ARG;
  esy_os_task_enter_critical();
  if (bus->owner == NULL) {
    esy_os_task_exit_critical();
    return ESY_OK;
  }
  if (bus->owner != bus) {
    esy_os_task_exit_critical();
    return ESY_ERR_STATE;
  }
  esy_event_bus_ctx_t *ctx = bus_context(bus);
  ctx->closing = true;
  if (ctx->publishers != 0u || ctx->subscriptions_used != 0u) {
    esy_os_task_exit_critical();
    return ESY_ERR_BUSY;
  }
  bus->owner = NULL;
  esy_os_task_exit_critical();
  return ESY_OK;
}

esy_err_t esy_event_bus_subscribe(esy_event_bus_t *bus,
    const esy_event_bus_subscription_config_t *config,
    esy_event_bus_subscription_t **out_subscription) {
  if (out_subscription != NULL) *out_subscription = NULL;
  if (bus == NULL || config == NULL || out_subscription == NULL ||
      config->static_resources == NULL || config->topic_mask == 0u ||
      config->queue_depth == 0u ||
      sizeof(esy_event_bus_event_t) > SIZE_MAX / config->queue_depth) {
    return ESY_ERR_INVALID_ARG;
  }
  const esy_event_bus_subscription_static_resources_t resources = *config->static_resources;
  if (resources.queue_buffer == NULL ||
      resources.queue_buffer_size < ESY_EVENT_BUS_QUEUE_BYTES(config->queue_depth) ||
      (uintptr_t)resources.queue_buffer % ESY_OS_QUEUE_STATIC_STORAGE_ALIGN != 0u) {
    return ESY_ERR_INVALID_ARG;
  }
  esy_os_task_enter_critical();
  if (bus->owner != bus || bus_context(bus)->closing) {
    esy_os_task_exit_critical();
    return ESY_ERR_STATE;
  }
  esy_event_bus_ctx_t *ctx = bus_context(bus);
  esy_os_task_exit_critical();

  /* Only the serialized lifecycle owner writes slot state or capacity.
   * Publishers only read state under the lock, so searching need not mask IRQs. */
  esy_event_bus_subscription_t *slot = NULL;
  for (size_t i = 0u; i < ctx->capacity; ++i) {
    if (subscription_context(&ctx->subscriptions[i])->state == SUBSCRIPTION_FREE) {
      slot = &ctx->subscriptions[i];
      break;
    }
  }
  if (slot == NULL) return ESY_ERR_NO_MEM;
  subscription_ctx_t *sub = subscription_context(slot);
  esy_os_task_enter_critical();
  sub->state = SUBSCRIPTION_CREATING;
  ++ctx->subscriptions_used;
  esy_os_task_exit_critical();

  esy_os_queue_t *queue = NULL;
  esy_err_t status = esy_os_queue_create_static(sizeof(esy_event_bus_event_t),
      config->queue_depth, resources.queue_buffer, &sub->queue_storage, &queue);
  esy_os_task_enter_critical();
  if (status == ESY_OK) {
    sub->queue = queue;
    sub->topic_mask = config->topic_mask;
    sub->state = SUBSCRIPTION_OPEN;
  } else {
    sub->state = SUBSCRIPTION_FREE;
    --ctx->subscriptions_used;
  }
  esy_os_task_exit_critical();
  if (status == ESY_OK) *out_subscription = slot;
  return status;
}

esy_err_t esy_event_bus_unsubscribe(esy_event_bus_subscription_t *subscription) {
  if (subscription == NULL) return ESY_ERR_INVALID_ARG;
  subscription_ctx_t *sub = subscription_context(subscription);
  esy_os_task_enter_critical();
  esy_event_bus_t *bus = sub->bus;
  if (bus == NULL || bus->owner != bus) {
    esy_os_task_exit_critical();
    return ESY_ERR_STATE;
  }
  if (sub->state == SUBSCRIPTION_FREE) {
    esy_os_task_exit_critical();
    return ESY_OK;
  }
  if (sub->state != SUBSCRIPTION_OPEN && sub->state != SUBSCRIPTION_CLOSING) {
    esy_os_task_exit_critical();
    return ESY_ERR_BUSY;
  }
  sub->state = SUBSCRIPTION_CLOSING;
  if (sub->users != 0u) {
    esy_os_task_exit_critical();
    return ESY_ERR_BUSY;
  }
  sub->state = SUBSCRIPTION_RETIRING;
  esy_os_queue_t *queue = sub->queue;
  esy_os_task_exit_critical();

  esy_os_queue_delete(queue);

  esy_os_task_enter_critical();
  sub->queue = NULL;
  sub->topic_mask = 0u;
  sub->state = SUBSCRIPTION_FREE;
  --bus_context(bus)->subscriptions_used;
  esy_os_task_exit_critical();
  return ESY_OK;
}

static esy_err_t publish(esy_event_bus_t *bus, const esy_event_bus_event_t *event,
    esy_event_bus_publish_result_t *result, bool from_isr, bool *need_yield) {
  if (need_yield != NULL) *need_yield = false;
  if (result != NULL) *result = (esy_event_bus_publish_result_t){0};
  if (bus == NULL || event == NULL || result == NULL ||
      event->topic >= ESY_EVENT_BUS_TOPIC_COUNT || event->size > ESY_EVENT_BUS_PAYLOAD_BYTES) {
    return ESY_ERR_INVALID_ARG;
  }
  uint32_t key = lock_registry(from_isr);
  if (bus->owner != bus || bus_context(bus)->closing) {
    unlock_registry(from_isr, key);
    return ESY_ERR_STATE;
  }
  esy_event_bus_ctx_t *ctx = bus_context(bus);
  if (ctx->publishers == SIZE_MAX) {
    unlock_registry(from_isr, key);
    return ESY_ERR_BUSY;
  }
  ++ctx->publishers;
  unlock_registry(from_isr, key);

  esy_event_bus_event_t copy = {0};
  copy.topic = event->topic;
  copy.size = event->size;
  memcpy(copy.data, event->data, event->size);
  const uint32_t topic_mask = ESY_EVENT_BUS_TOPIC_BIT(copy.topic);
  for (size_t i = 0u; i < ctx->capacity; ++i) {
    subscription_ctx_t *sub = subscription_context(&ctx->subscriptions[i]);
    esy_os_queue_t *queue = NULL;
    key = lock_registry(from_isr);
    if (sub->state == SUBSCRIPTION_OPEN && (sub->topic_mask & topic_mask) != 0u) {
      ++result->matched;
      if (sub->users == SIZE_MAX) {
        ++result->failed;
        if (result->first_error == ESY_OK) result->first_error = ESY_ERR_BUSY;
      } else {
        ++sub->users;
        queue = sub->queue;
      }
    }
    unlock_registry(from_isr, key);
    if (queue == NULL) continue;

    esy_err_t status;
    if (from_isr) {
      bool wake = false;
      status = esy_os_queue_send_from_isr(queue, &copy, &wake);
      if (need_yield != NULL) *need_yield = *need_yield || wake;
    } else {
      status = esy_os_queue_send(queue, &copy, 0u);
    }
    if (status == ESY_OK) ++result->delivered;
    else {
      ++result->failed;
      if (result->first_error == ESY_OK) result->first_error = status;
    }
    key = lock_registry(from_isr);
    --sub->users;
    unlock_registry(from_isr, key);
  }
  key = lock_registry(from_isr);
  --ctx->publishers;
  unlock_registry(from_isr, key);
  return result->failed == 0u ? ESY_OK : ESY_ERR_BUSY;
}

esy_err_t esy_event_bus_publish(esy_event_bus_t *bus,
    const esy_event_bus_event_t *event, esy_event_bus_publish_result_t *result) {
  return publish(bus, event, result, false, NULL);
}

esy_err_t esy_event_bus_publish_from_isr(esy_event_bus_t *bus,
    const esy_event_bus_event_t *event, esy_event_bus_publish_result_t *result,
    bool *need_yield) {
  return publish(bus, event, result, true, need_yield);
}

esy_err_t esy_event_bus_receive(esy_event_bus_subscription_t *subscription,
    esy_event_bus_event_t *event, uint32_t timeout_ms) {
  if (subscription == NULL || event == NULL || timeout_ms == UINT32_MAX) {
    return ESY_ERR_INVALID_ARG;
  }
  subscription_ctx_t *sub = subscription_context(subscription);
  esy_os_task_enter_critical();
  if (sub->bus == NULL || sub->bus->owner != sub->bus || sub->state != SUBSCRIPTION_OPEN) {
    esy_os_task_exit_critical();
    return ESY_ERR_STATE;
  }
  if (sub->users == SIZE_MAX) {
    esy_os_task_exit_critical();
    return ESY_ERR_BUSY;
  }
  ++sub->users;
  esy_os_queue_t *queue = sub->queue;
  esy_os_task_exit_critical();

  esy_event_bus_event_t received;
  esy_err_t status = esy_os_queue_receive(queue, &received, timeout_ms);
  if (status == ESY_OK) *event = received;

  esy_os_task_enter_critical();
  --sub->users;
  esy_os_task_exit_critical();
  return status;
}
