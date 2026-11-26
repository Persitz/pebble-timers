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
# Adapter tests exercise the TESTLOG_ENABLED build of the adapter tier (the
# one the emulator harness runs; release builds compile testlog() to a no-op).
ADAPTER_DEFS := -DTESTLOG_ENABLED
INCLUDES  := -I$(UNITY_DIR) -I$(LIB_DIR) -Ivendor

LIB_SRCS  := $(wildcard $(LIB_DIR)/*.c)
UNITY_SRC := $(UNITY_DIR)/unity.c
TEST_SRCS := $(wildcard *_test.c)
BINS      := $(patsubst %_test.c,build/%_test,$(TEST_SRCS))

# Generate and verify parse_nl's resource, offsets/checksum header, and
# byte-identical host include before any host binary compiles the parser.
.PHONY: nl_tables
nl_tables:
	python3 ../scripts/generate_nl_tables.py

$(BINS): | nl_tables

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
	$(CC) $(CFLAGS) $(ADAPTER_DEFS) -I$(FAKES_DIR) -I$(ADAPTER_DIR) $(INCLUDES) $^ -o $@

# The second adapter file (after testlog_am.c) let into the host test build,
# to test hold.c's
# up-event-past-threshold fire against FFF fakes of the click-config/AppTimer
# surface it calls -- test/fakes/pebble.h.
build/hold_test: hold_test.c $(ADAPTER_DIR)/hold.c $(LIB_DIR)/set_accel.c $(UNITY_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(ADAPTER_DEFS) -I$(FAKES_DIR) -I$(ADAPTER_DIR) $(INCLUDES) $^ -o $@

# The third adapter file (after testlog_am.c, hold.c) let into the host test
# build, to test input_guard.c's AppTimer-gated boolean against FFF fakes of
# the tiny AppTimer surface it calls -- test/fakes/pebble.h.
build/input_guard_test: input_guard_test.c $(ADAPTER_DIR)/input_guard.c $(UNITY_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(ADAPTER_DEFS) -I$(FAKES_DIR) -I$(ADAPTER_DIR) $(INCLUDES) $^ -o $@

# The fourth adapter file let into the host test build, to test
# dictation_am.c's session lifecycle against
# FFF fakes of the Dictation surface it calls -- test/fakes/pebble.h. Links
# the REAL lib/parse_nl.c + lib/voice_outcome.c (not faked, same convention
# as hold_test.c linking the real lib/set_accel.c) so the FULL-scope success
# tests exercise the actual parser, not a stand-in.
build/dictation_am_test: dictation_am_test.c $(ADAPTER_DIR)/dictation_am.c $(LIB_DIR)/parse_nl.c $(LIB_DIR)/voice_outcome.c $(UNITY_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(ADAPTER_DEFS) -I$(FAKES_DIR) -I$(ADAPTER_DIR) $(INCLUDES) $^ -o $@

# The fifth adapter file let into the host test build, to test
# wakeup_adapter.c's two-pass, status-aware retry scheduling against FFF
# fakes of the wakeup_schedule/wakeup_cancel_all surface it calls --
# test/fakes/pebble.h -- plus fakes of testlog()/testlog_init() (this test
# cares about scheduling decisions, not the test-log channel, so those two
# are faked directly in wakeup_adapter_test.c rather than linking
# testlog_am.c). Links the REAL lib/sched.c + lib/store.c + lib/timer.c so
# the retry-ladder/spacing/priority logic under test is the actual
# production code. -DWAKEUP_ADAPTER_TEST_HOOKS is scoped to this one binary
# (never defined for a release/emulator build) -- it exists solely so this
# test can reset wakeup_adapter.c's internal "durable diagnostic already
# dumped" latch between test functions, since Unity links every *_test.c
# function into one shared process where that latch would otherwise leak
# across unrelated tests; a real device gets a fresh, unset static on every
# actual launch and needs no equivalent (see wakeup_adapter.h's own comment
# on wakeup_adapter_test_reset_diag_dump()).
build/wakeup_adapter_test: wakeup_adapter_test.c $(ADAPTER_DIR)/wakeup_adapter.c $(LIB_DIR)/sched.c $(LIB_DIR)/store.c $(LIB_DIR)/timer.c $(UNITY_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(ADAPTER_DEFS) -DWAKEUP_ADAPTER_TEST_HOOKS -I$(FAKES_DIR) -I$(ADAPTER_DIR) $(INCLUDES) $^ -o $@

# The sixth adapter file let into the host test build (reuses
# wakeup_adapter_test's rationale for linking wakeup_adapter.c +
# lib/sched.c + lib/store.c + lib/timer.c against the real production code)
# -- the firmware-ORACLE property test. Also links
# fakes/wakeup_oracle.c, the host-side model of the real firmware's
# admission rule this test drives the adapter against instead of
# hand-written per-scenario canned FFF return values.
build/wakeup_property_test: wakeup_property_test.c $(ADAPTER_DIR)/wakeup_adapter.c $(LIB_DIR)/sched.c $(LIB_DIR)/store.c $(LIB_DIR)/timer.c $(FAKES_DIR)/wakeup_oracle.c $(UNITY_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(ADAPTER_DEFS) -I$(FAKES_DIR) -I$(ADAPTER_DIR) $(INCLUDES) $^ -o $@

# The seventh adapter file let into the host test build -- persist_adapter.c.
# Its save/load round trip can silently lose data on a torn write with no
# observable signal. See
# test/persist_adapter_test.c's own top comment. Links the REAL
# lib/serialize.c (+ its lib/store.c, lib/timer.c, lib/set_machine.c
# dependencies) so the wire format under test is the actual production
# encode/decode, against a small stateful fake flash (not canned returns)
# defined in the test file itself.
build/persist_adapter_test: persist_adapter_test.c $(ADAPTER_DIR)/persist_adapter.c $(LIB_DIR)/serialize.c $(LIB_DIR)/store.c $(LIB_DIR)/timer.c $(LIB_DIR)/set_machine.c $(LIB_DIR)/format.c $(UNITY_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(ADAPTER_DEFS) -I$(FAKES_DIR) -I$(ADAPTER_DIR) $(INCLUDES) $^ -o $@

# Persistence-adapter seams: each writes Pebble storage directly, so their
# checked-write behavior is tested against stateful fake flash rather than
# treating a successful call as proof that a write landed.
build/persist_preset_adapter_test: persist_preset_adapter_test.c $(ADAPTER_DIR)/persist_preset_adapter.c $(LIB_DIR)/preset_serialize.c $(LIB_DIR)/preset_store.c $(LIB_DIR)/timer.c $(UNITY_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(ADAPTER_DEFS) -I$(FAKES_DIR) -I$(ADAPTER_DIR) $(INCLUDES) $^ -o $@

build/persist_kind_adapter_test: persist_kind_adapter_test.c $(ADAPTER_DIR)/persist_kind_adapter.c $(UNITY_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(ADAPTER_DEFS) -I$(FAKES_DIR) -I$(ADAPTER_DIR) $(INCLUDES) $^ -o $@

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
