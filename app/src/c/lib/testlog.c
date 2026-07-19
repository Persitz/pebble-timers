// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "testlog.h"

#include <stdio.h>
#include <string.h>

size_t testlog_format_line(char *out, size_t out_size, const char *tag, const char *data) {
  if (out == NULL || out_size == 0) return 0;
  if (tag == NULL) tag = "";

  int written;
  if (data == NULL) {
    written = snprintf(out, out_size, "%s", tag);
  } else {
    written = snprintf(out, out_size, "%s:%s", tag, data);
  }

  if (written < 0) {
    out[0] = '\0';
    return 0;
  }
  size_t written_len = (size_t)written;
  return written_len < out_size ? written_len : out_size - 1;
}

void testlog_queue_init(TestLogQueue *queue) {
  queue->head = 0;
  queue->count = 0;
}

bool testlog_queue_is_empty(const TestLogQueue *queue) {
  return queue->count == 0;
}

bool testlog_queue_is_full(const TestLogQueue *queue) {
  return queue->count >= TESTLOG_QUEUE_CAPACITY;
}

bool testlog_queue_push(TestLogQueue *queue, const char *line) {
  if (testlog_queue_is_full(queue)) return false;
  int tail = (queue->head + queue->count) % TESTLOG_QUEUE_CAPACITY;
  strncpy(queue->lines[tail], line, TESTLOG_MAX_LINE_LEN);
  queue->lines[tail][TESTLOG_MAX_LINE_LEN] = '\0';
  queue->count++;
  return true;
}

const char *testlog_queue_peek(const TestLogQueue *queue) {
  if (testlog_queue_is_empty(queue)) return NULL;
  return queue->lines[queue->head];
}

void testlog_queue_pop(TestLogQueue *queue) {
  if (testlog_queue_is_empty(queue)) return;
  queue->head = (queue->head + 1) % TESTLOG_QUEUE_CAPACITY;
  queue->count--;
}
