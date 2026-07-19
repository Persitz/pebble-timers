// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "testlog.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// --- testlog_format_line ---------------------------------------------------

void test_format_line_tag_only(void) {
  char buf[32];
  testlog_format_line(buf, sizeof buf, "app_init", NULL);
  TEST_ASSERT_EQUAL_STRING("app_init", buf);
}

void test_format_line_tag_and_data(void) {
  char buf[32];
  testlog_format_line(buf, sizeof buf, "home", "empty");
  TEST_ASSERT_EQUAL_STRING("home:empty", buf);
}

void test_format_line_null_tag_is_empty_prefix(void) {
  char buf[32];
  testlog_format_line(buf, sizeof buf, NULL, "x");
  TEST_ASSERT_EQUAL_STRING(":x", buf);
}

void test_format_line_truncates_to_fit(void) {
  char buf[6];
  size_t written = testlog_format_line(buf, sizeof buf, "abcdef", NULL);
  TEST_ASSERT_EQUAL_size_t(5, written);
  TEST_ASSERT_EQUAL_STRING("abcde", buf);
}

// --- TestLogQueue ------------------------------------------------------------

void test_queue_starts_empty(void) {
  TestLogQueue q;
  testlog_queue_init(&q);
  TEST_ASSERT_TRUE(testlog_queue_is_empty(&q));
  TEST_ASSERT_FALSE(testlog_queue_is_full(&q));
  TEST_ASSERT_NULL(testlog_queue_peek(&q));
}

void test_queue_push_then_peek_is_fifo_order(void) {
  TestLogQueue q;
  testlog_queue_init(&q);
  TEST_ASSERT_TRUE(testlog_queue_push(&q, "first"));
  TEST_ASSERT_TRUE(testlog_queue_push(&q, "second"));
  TEST_ASSERT_EQUAL_STRING("first", testlog_queue_peek(&q));
  testlog_queue_pop(&q);
  TEST_ASSERT_EQUAL_STRING("second", testlog_queue_peek(&q));
}

void test_queue_pop_on_empty_is_a_no_op(void) {
  TestLogQueue q;
  testlog_queue_init(&q);
  testlog_queue_pop(&q);  // must not crash / underflow
  TEST_ASSERT_TRUE(testlog_queue_is_empty(&q));
}

void test_queue_fills_to_capacity_then_refuses_push_no_drop(void) {
  TestLogQueue q;
  testlog_queue_init(&q);
  for (int i = 0; i < TESTLOG_QUEUE_CAPACITY; i++) {
    TEST_ASSERT_TRUE(testlog_queue_push(&q, "line"));
  }
  TEST_ASSERT_TRUE(testlog_queue_is_full(&q));
  // The queue is full: the already-accepted lines must not be dropped to
  // make room -- the push is simply refused.
  TEST_ASSERT_FALSE(testlog_queue_push(&q, "overflow"));
  TEST_ASSERT_TRUE(testlog_queue_is_full(&q));
}

void test_queue_wraps_around_ring_buffer(void) {
  TestLogQueue q;
  testlog_queue_init(&q);
  for (int i = 0; i < TESTLOG_QUEUE_CAPACITY; i++) {
    testlog_queue_push(&q, "a");
  }
  // Drain 3, push 3 more -- exercises the wraparound of head+count % CAPACITY.
  for (int i = 0; i < 3; i++) testlog_queue_pop(&q);
  TEST_ASSERT_TRUE(testlog_queue_push(&q, "b1"));
  TEST_ASSERT_TRUE(testlog_queue_push(&q, "b2"));
  TEST_ASSERT_TRUE(testlog_queue_push(&q, "b3"));
  TEST_ASSERT_TRUE(testlog_queue_is_full(&q));
  for (int i = 0; i < TESTLOG_QUEUE_CAPACITY - 3; i++) testlog_queue_pop(&q);
  TEST_ASSERT_EQUAL_STRING("b1", testlog_queue_peek(&q));
}

void test_queue_push_truncates_long_lines(void) {
  TestLogQueue q;
  testlog_queue_init(&q);
  char long_line[TESTLOG_MAX_LINE_LEN + 50];
  memset(long_line, 'x', sizeof long_line - 1);
  long_line[sizeof long_line - 1] = '\0';
  TEST_ASSERT_TRUE(testlog_queue_push(&q, long_line));
  TEST_ASSERT_EQUAL_size_t((size_t)TESTLOG_MAX_LINE_LEN, strlen(testlog_queue_peek(&q)));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_format_line_tag_only);
  RUN_TEST(test_format_line_tag_and_data);
  RUN_TEST(test_format_line_null_tag_is_empty_prefix);
  RUN_TEST(test_format_line_truncates_to_fit);
  RUN_TEST(test_queue_starts_empty);
  RUN_TEST(test_queue_push_then_peek_is_fifo_order);
  RUN_TEST(test_queue_pop_on_empty_is_a_no_op);
  RUN_TEST(test_queue_fills_to_capacity_then_refuses_push_no_drop);
  RUN_TEST(test_queue_wraps_around_ring_buffer);
  RUN_TEST(test_queue_push_truncates_long_lines);
  return UNITY_END();
}
