// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// lib/sched's candidates <-> wakeup_cancel_all/wakeup_schedule/wakeup_query
// (cancel-all + reschedule + immediate-verify adapter pattern). Because the
// plan is always recomputed from the store, this is idempotent, needs no
// persisted cookie<->WakeupId map, and self-heals on relaunch.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "lib/store.h"

// Not a real StatusCode -- wakeup_schedule() only documents -3/-4/-7/-8
// (E_INTERNAL/E_INVALID_ARGUMENT/E_OUT_OF_RESOURCES/E_RANGE). This sentinel
// is DIAGNOSTIC ONLY and means:
//
//     "firmware ACCEPTED this wakeup and the timer IS armed, but the
//      immediate read-back never confirmed it."
//
// It is NOT a scheduling failure and must not be read as one. It never
// appears as a candidate's scheduling status (that carries the positive
// WakeupId firmware returned); it appears only in the durable field record
// persisted at the end of a replan, where it is strictly more informative
// than the bare id -- it is the one durable way to learn that a device's
// wakeup read-back path is misbehaving, on hardware where testlog() reaches
// nobody.
//
// Whoever reads `wakeup_diag last_status=-100` off `pebble logs` should
// conclude "armed, unconfirmed", not "failed" -- and the APP_LOG line
// spells that out in words rather than relying on this comment being found.
// Deliberately far from the documented StatusCode range (and from a real
// WakeupId, always >=0) so it can never be confused with either.
#define WAKEUP_QUERY_FAILED (-100)

// Cancels every previously-scheduled wakeup, replans from `store`/`now`,
// attempts every lead-eligible primary's own on-time deadline in deadline
// order -- ALL of them, up to MAX_TIMERS, not capped at 8 -- stopping new
// attempts only once our own 8-entry-per-app firmware cap is genuinely full
// (real, firmware-accepted entries, never a guess; firmware's cap is on
// ACCEPTED entries, not on attempts). Retries
// only E_RANGE via the negative-first/short-lead-forward ladder, and
// immediately reads every positive WakeupId back with wakeup_query(). A
// FAILED read-back never costs the timer its entry: firmware's acceptance is
// what arms the timer, and a false read-back is not evidence of absence, so
// the entry is kept and merely recorded as unconfirmed. Nothing is ever
// cancelled on an ambiguous read -- see wakeup_adapter.c's try_schedule()
// for why cancel-and-re-arm cannot establish ownership safely. Emits a
// testLog marker per real outcome
// (wakeup:schedule / wakeup:schedule_unverified / wakeup:fallback_armed /
// wakeup:unschedulable).
//
// WHEN THIS IS CALLED, exactly: on app launch, on a wakeup firing, and on
// any schedule-affecting store mutation. That is the complete list.
//
// It is deliberately NOT called on background/exit (an earlier version of
// this comment claimed it was -- it never has been; see main.c's deinit()
// and handle_will_focus(), neither of which replans), and there is no
// periodic replan, because a watchapp that is not running has no execution
// context in which to schedule one and this app ships no background worker.
//
// The consequence, stated plainly rather than left implied by
// wakeup_adapter.c's "heals on the very next replan" comment: healing is
// driven by the user opening the app or by a mutation, NOT by the passage
// of time. A timer left fully unarmed by a foreign collider (all three
// ranks refused) stays unarmed until one of those happens, even if the
// collider clears seconds later. The user's only in-app signal is
// screen_hero.c's no-background-alarm indicator. Closing this would need a
// driver that does not exist today (a background worker, or a keep-alive
// wakeup purely to re-plan). This is a limitation of the current design,
// not a guarantee that time alone heals an unarmed timer.
void wakeup_adapter_replan(const TimerStore *store, time_t now);

// True if firmware ACCEPTED a wakeup for `cookie` during the most recent
// wakeup_adapter_replan() -- real OS state, never the pure planner's guess.
// This drives screen_hero.c's no-background-alarm indicator. False for any
// cookie not in the last replan's real accepted set: unarmed, excluded, or
// simply not a candidate at all (e.g.
// paused/fired/stopwatch) right now.
//
// PRECISE MEANING (this used to say "query-confirmed", which the
// implementation no longer guarantees):
// "accepted by firmware, and read-back-confirmed WHERE THE READ-BACK WAS
// AVAILABLE." When wakeup_query() fails twice on an entry
// firmware accepted, this returns true on firmware's acceptance alone. That
// is deliberate and is the same thing this function reported before any
// query existed: the alternative -- reporting false for a timer firmware
// accepted -- would show the user a no-background-alarm warning for an alarm
// that is, in all likelihood, armed. Erring toward the warning would be the
// louder falsehood, not the safer one.
bool wakeup_adapter_is_armed(int32_t cookie);

#ifdef EMU_TEST_HOOKS
// Emulator test hook: a fault injector that drops the most recently
// accepted primary after its redundant entry is armed, for exercising
// fallback-rung behavior. Never compiled into a release binary.
bool wakeup_adapter_test_drop_primary(void);
#endif

#ifdef GALLERY_MODE
// Debug-only entry points for the screen gallery (src/c/gallery.c): let a
// scene declare directly whether the timer it is building would show as
// armed or unarmed on a real device, by seeding/clearing the SAME
// `s_accepted` set wakeup_adapter_is_armed() reads -- WITHOUT calling
// wakeup_adapter_replan() (no wakeup_schedule()/wakeup_cancel_all(), no OS
// wakeup-table entries, no side effects). The gallery's fixture builder
// deliberately never calls wakeup_adapter_replan() at all (see gallery.c's
// gallery_fire_all_due() comment, which lists it among the side effects a
// deterministic, single-process scene builder must avoid -- a real replan
// can fire during scene construction and interfere with it) -- these two
// functions are how a scene supplies the state that omission otherwise
// loses, instead of poking wakeup_adapter.c's private AcceptedSet fields
// directly from gallery.c. Only cookie membership is seeded/read; no
// WakeupId or timestamp is recorded (wakeup_adapter_is_armed() never reads
// either, and nothing in a gallery build ever calls wakeup_adapter_replan()
// to disagree with it). Mirrors the hold_debug_force()/
// screen_hero_debug_force_up_hint() "force the visual state directly, skip
// the real mechanism" shape used elsewhere in this build. Never compiled
// into a release build.
void wakeup_adapter_gallery_reset_armed(void);
void wakeup_adapter_gallery_seed_armed(int32_t cookie);
#endif

#ifdef WAKEUP_ADAPTER_TEST_HOOKS
// Host-test-only: re-arms the "dump the previous process's durable wakeup
// diagnostic once" latch (see wakeup_adapter.c's wakeup_diag_dump_previous()).
// Real builds (release AND the emulator/EMU_TEST_HOOKS build) never define
// WAKEUP_ADAPTER_TEST_HOOKS, so this never compiles into anything shipped or
// installed -- it exists only because Unity links every *_test.c function
// into ONE process, where the adapter's internal "already dumped" static
// would otherwise leak across unrelated test functions. A real device gets a
// fresh, unset static on every actual app launch and needs no equivalent.
void wakeup_adapter_test_reset_diag_dump(void);
#endif
