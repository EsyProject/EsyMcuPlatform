/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TEST_STREAM_BUFFER_H
#define TEST_STREAM_BUFFER_H
#include "FreeRTOS.h"
typedef struct {uint8_t *data; size_t capacity, used;} StaticStreamBuffer_t;
typedef StaticStreamBuffer_t *StreamBufferHandle_t;
StreamBufferHandle_t xStreamBufferCreateStatic(size_t capacity, size_t trigger, uint8_t *data, StaticStreamBuffer_t *storage);
void vStreamBufferDelete(StreamBufferHandle_t buffer);
BaseType_t xStreamBufferReset(StreamBufferHandle_t buffer);
size_t xStreamBufferSend(StreamBufferHandle_t buffer, const void *data, size_t size, TickType_t ticks);
size_t xStreamBufferReceive(StreamBufferHandle_t buffer, void *data, size_t size, TickType_t ticks);
size_t xStreamBufferSendFromISR(StreamBufferHandle_t buffer, const void *data, size_t size, BaseType_t *woken);
size_t xStreamBufferReceiveFromISR(StreamBufferHandle_t buffer, void *data, size_t size, BaseType_t *woken);
BaseType_t xStreamBufferSetTriggerLevel(StreamBufferHandle_t buffer, size_t trigger);
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t buffer);
size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t buffer);
size_t xStreamBufferNextMessageLengthBytes(StreamBufferHandle_t buffer);
#endif
