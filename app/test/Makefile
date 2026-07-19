# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Herman Persitz

# Host-side unit test build. Uses the HOST gcc (aarch64/whatever the CI
# runner is), entirely separate from `pebble build` (waf/arm-none-eabi-gcc).
# Pure src/c/lib/*.c compiles here because it never includes <pebble.h> --
# that omission is the mechanical guard for the project's lib/-purity rule (a
# lib/ file that pulls in pebble.h simply won't compile: no pebble.h on the
# host include path).
#
# One *_test.c per src/c/lib/ unit; each links against the whole lib/ (small
# enough not to bother with per-unit object selection) + Unity, and runs as
# its own binary. `make test` builds and runs every *_test.c.
CC       ?= gcc
CFLAGS   ?= -std=c11 -Wall -Wextra -Werror -g -O0
UNITY_DIR := vendor/unity
LIB_DIR   := ../src/c/lib
ADAPTER_DIR := ../src/c
FAKES_DIR := fakes
INCLUDES  := -I$(UNITY_DIR) -I$(LIB_DIR) -Ivendor

LIB_SRCS  := $(wildcard $(LIB_DIR)/*.c)
UNITY_SRC := $(UNITY_DIR)/unity.c
TEST_SRCS := $(wildcard *_test.c)
BINS      := $(patsubst %_test.c,build/%_test,$(TEST_SRCS))

build/%_test: %_test.c $(LIB_SRCS) $(UNITY_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@

# The one adapter file (not lib/) let into the host test build, specifically
# to test testlog_am.c's
# self-driven AppTimer retry path against FFF fakes of the tiny AppMessage/
# AppTimer surface it calls -- test/fakes/pebble.h, NOT the real SDK header.
# An explicit (non-pattern) rule always takes precedence over build/%_test
# above for this one target.
build/testlog_am_test: testlog_am_test.c $(ADAPTER_DIR)/testlog_am.c $(LIB_DIR)/testlog.c $(UNITY_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(FAKES_DIR) -I$(ADAPTER_DIR) $(INCLUDES) $^ -o $@

# The second adapter file (after testlog_am.c) let into the host test build,
# to test hold.c's
# up-event-past-threshold fire against FFF fakes of the click-config/AppTimer
# surface it calls -- test/fakes/pebble.h.
build/hold_test: hold_test.c $(ADAPTER_DIR)/hold.c $(LIB_DIR)/set_accel.c $(UNITY_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(FAKES_DIR) -I$(ADAPTER_DIR) $(INCLUDES) $^ -o $@

# The third adapter file (after testlog_am.c, hold.c) let into the host test
# build, to test input_guard.c's AppTimer-gated boolean against FFF fakes of
# the tiny AppTimer surface it calls -- test/fakes/pebble.h.
build/input_guard_test: input_guard_test.c $(ADAPTER_DIR)/input_guard.c $(UNITY_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(FAKES_DIR) -I$(ADAPTER_DIR) $(INCLUDES) $^ -o $@

# The fourth adapter file let into the host test build, to test
# dictation_am.c's session lifecycle against
# FFF fakes of the Dictation surface it calls -- test/fakes/pebble.h. Links
# the REAL lib/parse_nl.c + lib/voice_outcome.c (not faked, same convention
# as hold_test.c linking the real lib/set_accel.c) so the FULL-scope success
# tests exercise the actual parser, not a stand-in.
build/dictation_am_test: dictation_am_test.c $(ADAPTER_DIR)/dictation_am.c $(LIB_DIR)/parse_nl.c $(LIB_DIR)/voice_outcome.c $(UNITY_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(FAKES_DIR) -I$(ADAPTER_DIR) $(INCLUDES) $^ -o $@

test: $(BINS)
	@status=0; \
	for bin in $(BINS); do \
		echo "--- $$bin ---"; \
		./$$bin || status=1; \
	done; \
	exit $$status

clean:
	rm -rf build

.PHONY: test clean
