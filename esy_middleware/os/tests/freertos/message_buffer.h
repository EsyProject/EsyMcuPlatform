/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TEST_MESSAGE_BUFFER_H
#define TEST_MESSAGE_BUFFER_H
#include "stream_buffer.h"
typedef StaticStreamBuffer_t StaticMessageBuffer_t;
typedef StreamBufferHandle_t MessageBufferHandle_t;
MessageBufferHandle_t xMessageBufferCreateStatic(size_t capacity, uint8_t *data, StaticMessageBuffer_t *storage);
#define vMessageBufferDelete vStreamBufferDelete
#define xMessageBufferReset xStreamBufferReset
#define xMessageBufferSpaceAvailable xStreamBufferSpacesAvailable
size_t xMessageBufferSend(MessageBufferHandle_t buffer, const void *data, size_t size, TickType_t ticks);
size_t xMessageBufferReceive(MessageBufferHandle_t buffer, void *data, size_t size, TickType_t ticks);
size_t xMessageBufferSendFromISR(MessageBufferHandle_t buffer, const void *data, size_t size, BaseType_t *woken);
size_t xMessageBufferReceiveFromISR(MessageBufferHandle_t buffer, void *data, size_t size, BaseType_t *woken);
#endif
