// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Pure Timer type + derivations + mutation contract. NEVER #include <pebble.h>.
// time_t SECONDS throughout -- no ms state here. `now` is always injected by
// the caller (host-testable).
#pragma once

#include <stdbool.h>
#include <stddef.h>  // size_t (timer_name_copy)
#include <stdint.h>
#include <time.h>

#include "set_machine.h"

#define MAX_LAPS 16

// Fixed display-name cap, bytes incl. NUL. Matches lib/parse_nl.h's
// PARSE_NL_NAME_MAX (24) by design -- a voice-heard/added name always fits
// into Timer.name without a second, tighter truncation pass. Kept as an
// independent constant (not a shared header) since lib/timer.h must not
// depend on the higher-level lib/parse_nl.h (parse_nl already depends on
// timer.h for TimerKind).
#define TIMER_NAME_MAX 24

// timer_add_quarter() below grows duration_s COMPOUNDING (`+= duration_s/4`
// each call) -- unbounded repeated Up-holds (~62-80 in practice) overflow
// int32_t (UB) and the corrupt value gets persisted, so a saturating cap is
// required.
//
// This cap is deliberately derived from the SAME named constant SET's own
// dial (lib/set_machine.c's clamp_h) clamps against (SET_MACHINE_MAX_HOURS =
// 99h), rather than an independent magic number. An earlier version used a
// fresh, independent 24h constant unrelated to the dial's actual ceiling --
// that mismatch meant a timer dialed above 24h (fully reachable through the
// ordinary SET screen) could get its duration SHRUNK (not grown) by its very
// first add-1/4 hold, since the old saturation clamp only ever pulled a
// value DOWN to 24h. Deriving the cap from the dial's own constant makes
// that class of mismatch structurally impossible to reintroduce -- the two
// can no longer silently drift apart. The dial's absolute ceiling is h=99,
// m=59, s=59 (359999s); see timer_add_quarter()'s own doc for the separate,
// belt-and-braces "never shrink" guard that makes this cap safe even for a
// timer that somehow already exceeds it.
#define TIMER_MAX_DURATION_S ((int32_t)SET_MACHINE_MAX_HOURS * 3600 + 59 * 60 + 59)

typedef enum {
  KIND_COUNTDOWN,
  KIND_STOPWATCH,  // also used for the post-timer stopwatch (original_total_s > 0)
  KIND_REPEAT,
} TimerKind;

// Field ORDER below is deliberately packed for binary size, not declaration
// order. A struct's only padding rules are "each member aligned to its own
// alignment" and "total size a multiple of the struct's
// own alignment" (its most-aligned member, 4 here, from the int32_t/time_t
// fields) -- single-byte fields scattered among 4-byte fields each cost up
// to 3 bytes of padding. `kind` (TimerKind) is itself only 1 byte under the
// ARM EABI arm-none-eabi-gcc targets (an enum is sized to the smallest type
// that holds every enumerator when it can be, unlike the 4-byte int the host
// gcc build uses for the same type -- confirmed empirically via the real ARM
// sizeof, not assumed) -- so grouping `is_paused`/`fired`/`lap_count`
// (3 more single-byte fields) immediately after `kind` fills that same
// 4-byte alignment slot with ZERO padding, rather than costing padding of
// their own further down the struct. Measured real saving on the actual ARM
// build: sizeof(Timer) 72->60 (12B/Timer), x MAX_TIMERS=16 = 192B bss, with
// NO `.text` regression (an earlier attempt that grouped the same 3 fields
// at the struct's END instead, next to `name`, saved only 128B bss AND cost
// +56B `.text` from codegen sensitivity to field offsets elsewhere in the
// tree -- this arrangement, measured, has neither cost). Purely a layout
// change -- every field keeps its exact type/width/semantics; serialize.c
// encodes/decodes each field individually by name (never memcpy's the
// struct), so wire format is unaffected and no schema bump is needed. No
// positional (non-designated) Timer struct literals exist anywhere in the
// tree (grep-verified) that could depend on declaration order.
typedef struct {
  int32_t cookie;               // stable id (== wakeup cookie); 0 means "unused slot"
  TimerKind kind;
  bool is_paused;
  bool fired;  // durable "reached its boundary" -- see keepCounting's contract
  uint8_t lap_count;
  int32_t duration_s;           // countdown target / repeat interval; 0 for a plain stopwatch
  time_t started_at;            // absolute epoch seconds of the current run/cycle
  time_t paused_at;             // epoch seconds when paused; meaningless unless is_paused
  int32_t accumulated_pause_s;  // total seconds already spent paused across this run
  int32_t cycles;                // repeat: completed intervals ("done x N"); 0 otherwise
  // The amount the user actually DIALED/spoke -- the durable "original SET
  // amount," written ONCE at create (store_add()) and re-written by
  // timer_edit_interval() (a deliberate interval edit IS a new SET). Every
  // other mutation in this file's contract leaves it untouched by
  // construction: pause/resume/add_minute/add_quarter/snooze/snooze_seconds/
  // snooze_repeat/keep_counting/advance_cycle/toggle_repeat all preserve it
  // -- see each function's own doc for why. timer_restart() (below) is the
  // one reader that matters: Restart/re-arm target THIS field, not the
  // (possibly since-grown) duration_s. This field also retires an earlier
  // "quarter-amount cache" role original_total_s used to multiplex for a
  // countdown (see that field's own doc below) -- timer_quarter_amount() now
  // reads THIS field directly for a countdown, with no lazy-caching
  // machinery needed, since it's always populated by create time.
  // For KIND_STOPWATCH this field is inert: keep-counting preserves the
  // prior dialed amount, but no stopwatch operation reads it.
  int32_t set_total_s;
  // original_total_s used to be triple-multiplexed (countdown quarter-cache /
  // post-timer-stopwatch provenance / repeat snooze marker) -- set_total_s's
  // introduction retires the quarter-cache role, leaving this field
  // DUAL-purpose only, disambiguated purely by `kind` (never both at once
  // for a single Timer, since a Timer is exactly one kind at a time):
  //   - KIND_STOPWATCH: post-timer provenance ("from a 15:00 timer"), set
  //     once by timer_keep_counting(), 0 = a plain (never-was-a-countdown)
  //     stopwatch. Untouched by everything else in this file.
  //   - KIND_REPEAT: the "currently snoozing" marker (timer_snooze_repeat()/
  //     timer_is_snoozing() below) -- deliberately NOT re-purposed into
  //     set_total_s's role even though it happens to hold the interval value
  //     while snoozing, because timer_is_snoozing()'s own kind check is what
  //     disambiguates it from the STOPWATCH meaning above, not the field's
  //     value.
  //   - KIND_COUNTDOWN: dead/unused -- nothing writes it for a plain
  //     countdown any more (the earlier quarter-cache role that used to live
  //     here is retired).
  int32_t original_total_s;
  // Pure DISPLAY metadata -- "" (the zero-init/store_add default) means
  // unnamed. No derivation, sched, wakeup, ringing, or interruption path
  // reads this field; every
  // mutation function below leaves it untouched by construction (a name
  // persists across every run-state mutation -- pause/resume/restart/
  // snooze/keep-counting/advance-cycle/toggle-repeat/edit-interval -- and is
  // cleared only when the Timer itself is removed, store_remove()). Set via
  // lib/store.h's store_set_name(), which routes through timer_name_copy()
  // below for the trim+cap+UTF-8-safety contract.
  char name[TIMER_NAME_MAX];
} Timer;

// Copies `src` into the fixed `dst[dst_size]` buffer, always NUL-terminated:
// trims leading/trailing ASCII whitespace, then truncates at dst_size-1
// bytes WITHOUT splitting a multi-byte UTF-8 sequence (a codepoint that
// wouldn't fully fit is dropped whole, never left dangling) -- a display
// name is user-facing text with accents preserved (do NOT accent-fold),
// unlike this file's numeric/duration fields, so a raw byte cut isn't safe
// here. Zeroes the WHOLE destination buffer first (not just
// up to the terminator), so no stale bytes from a previous, longer name ever
// linger past the new NUL. `src` NULL, or empty after trimming, -> `dst` is
// all-zero (name-less). Shared by lib/store.c's store_set_name() (a parser
// residue, already <= this same cap) and screen_voice.c's own Add-name
// dictation arc (a raw, longer dictation transcript taken verbatim as the
// name -- no parse_nl pass) so both copy a candidate name through the exact
// same safe rule.
void timer_name_copy(char *dst, size_t dst_size, const char *src);

// -- Derivations (pure; `now` required) --------------------------------------

bool timer_is_paused(const Timer *t);

// max(0, (is_paused ? paused_at : now) - started_at - accumulated_pause_s)
int32_t timer_effective_elapsed_s(const Timer *t, time_t now);

// countdown & repeat: duration_s - effective_elapsed (MAY be negative -- caller
// clamps for display). Not meaningful for a stopwatch.
int32_t timer_remaining_s(const Timer *t, time_t now);

// stopwatch AND post-timer stopwatch: effective_elapsed_s. Do NOT add
// original_total_s -- a post-timer stopwatch's elapsed continues seamlessly
// through the original countdown's zero-crossing.
int32_t timer_elapsed_s(const Timer *t, time_t now);

// max(0, effective_elapsed - duration_s); countdown & repeat alarm/overtime
// value. Also the correct derivation for a post-timer stopwatch's overrun
// chip: timer_keep_counting() preserves duration_s unchanged
// (only original_total_s is newly set, to the same value) precisely so this
// formula keeps working after the kind flips to STOPWATCH.
int32_t timer_overrun_s(const Timer *t, time_t now);

// started_at + accumulated_pause_s + duration_s -- VALID ONLY while running
// (the scheduler excludes paused timers; callers must not call this on a
// paused timer and expect a meaningful answer).
time_t timer_deadline(const Timer *t);

// (countdown|repeat) && !is_paused && !fired && effective_elapsed >= duration_s
bool timer_is_firing(const Timer *t, time_t now);

// clamp(effective_elapsed / duration_s, 0, 1) as a 0-100 integer -- repeat's
// segmented-bar fill (and reusable for the countdown draining-bar fraction).
int timer_progress_pct(const Timer *t, time_t now);

// -- Mutation contract (pure; mutates *t in place; returns void). ------------
// Each mutation's before/after behavior is defined by the contract comments below; every field
// change is expressed and computed in whole seconds (time_t), consistent with the rest of this
// module's units.
//
// set_total_s: UNTOUCHED by every mutation below EXCEPT the two explicitly documented at
// their own declarations -- timer_restart() (reads it) and
// timer_edit_interval() (re-writes it). In particular timer_pause/
// timer_resume/timer_add_minute/timer_add_quarter/timer_snooze/timer_
// snooze_seconds/timer_snooze_repeat/timer_keep_counting/timer_
// advance_cycle/timer_toggle_repeat/timer_add_lap all leave it exactly as
// it was -- this is what makes it a stable "what the user actually dialed"
// value across any amount of transient run-state churn.

void timer_pause(Timer *t, time_t now);   // !paused -> paused_at = now (idempotent if already paused)
void timer_resume(Timer *t, time_t now);  // paused -> accumulated_pause_s += now - paused_at; is_paused = false
void timer_add_minute(Timer *t);          // duration_s += 60; fired = false (re-arms at the new deadline)
// started_at=now; accumulated_pause_s=0; is_paused=false; fired=false; cycles=0;
// lap_count=0.
//
// duration_s: for KIND_COUNTDOWN and KIND_REPEAT, RESET to set_total_s --
// Restart/re-arm returns to what the user actually DIALED, dropping any
// transient growth from +1:00 (timer_add_minute), add-1/4 (timer_
// add_quarter), or a snooze (timer_snooze/timer_snooze_seconds/timer_
// snooze_repeat) that ran since. (An earlier version instead reset to the
// current, possibly-grown duration_s -- i.e. "a Restart does not re-dial" --
// which was wrong: a Restart should return to what was originally set, not
// re-run whatever it had grown to.) For KIND_STOPWATCH, duration_s is left
// UNCHANGED (out of scope here -- "Reset to 0" only zeroes elapsed; a
// post-timer stopwatch's own duration_s is load-bearing for
// timer_overrun_s() and has no "set_total_s" semantics of its own).
//
// This SAME mutation is reused, unchanged, as the "Restart" affordance for
// KIND_REPEAT too (live ACTIONS row, the running hero's own Up-hold
// gesture, and the fired-repeat options' own row) -- cycles=0 above already
// gives repeat the correct "begin fresh, done x0" semantics (distinct from
// timer_advance_cycle()'s "Keep going", which increments cycles instead of
// zeroing them). The ONE kind-conditional exception (beyond the duration_s
// rule above): for KIND_REPEAT only, this ALSO clears original_total_s --
// that field is repeat's "currently snoozing" marker (timer_snooze_repeat/
// timer_is_snoozing below), and a timer that's just been restarted is by
// definition no longer snoozing (same clear timer_advance_cycle() already
// does on every natural re-arm).
void timer_restart(Timer *t, time_t now);

// kind=COUNTDOWN; duration_s=minutes*60; started_at=now; accumulated_pause_s=0;
// is_paused=false; fired=false; original_total_s=0 (clears the repeat
// snooze marker, if the *source* timer was a snoozing repeat -- meaningless
// once the kind is no longer KIND_REPEAT). Deliberately does NOT touch
// set_total_s: a snooze is a TRANSIENT adjustment to the current run, not a
// new SET -- the pre-snooze set_total_s survives so that a LATER
// timer_restart() on this now-snoozed countdown returns to the pre-snooze
// SET amount, not the snooze length.
void timer_snooze(Timer *t, int minutes, time_t now);

// countdown -> post-timer stopwatch: kind=STOPWATCH; original_total_s=duration_s;
// KEEP started_at/accumulated_pause_s/duration_s (elapsed() continues seamlessly
// through the zero-crossing); fired=false [load-bearing -- a stopwatch must never
// read fired=true; every kind-agnostic `fired` reader is a bug].
void timer_keep_counting(Timer *t, time_t now);

// repeat "Keep going": cycles += 1; started_at=now; accumulated_pause_s=0;
// is_paused=false; fired=false; original_total_s=0 -- this last field is the
// snooze marker timer_snooze_repeat() below sets; a natural "Keep going"
// re-arm is exactly the event that ends a snooze window, so it clears the
// marker here.
void timer_advance_cycle(Timer *t, time_t now);

// countdown -> repeat, or repeat -> countdown (kind flips): ONLY `kind`,
// `cycles` (=0), `fired` (=false), and `original_total_s` (=0) change.
// started_at/paused_at/accumulated_pause_s/is_paused are left UNTOUCHED --
// an earlier version RESET the clock in both directions on a toggle
// (started_at=now, accumulated_pause_s=0, is_paused=false); this was changed
// so the running clock (elapsed/deadline, and the paused/not-paused state)
// is preserved across a kind flip -- only the kind itself changes.
// duration_s stays UNCHANGED too, unaffected by this change. cycles=0
// unconditionally: a just-flipped-to-repeat timer has by definition
// completed no intervals yet (countdown -> repeat direction); the repeat ->
// countdown direction doesn't read cycles at all, so resetting it there is
// moot, not meaningful. fired=false is still required (a freshly-flipped
// timer must not read as already fired -- note that if the pre-toggle
// elapsed already exceeds duration_s, this can make the flipped timer
// immediately eligible to fire again; ACCEPTABLE, not a defect to guard
// against here). original_total_s=0 -- a kind flip must not let one kind's
// private meaning of this field (STOPWATCH provenance is moot here since
// neither direction is STOPWATCH; REPEAT's snooze marker) leak into the
// other: flipping TO repeat with a stale marker would make
// timer_is_snoozing() misreport "snoozing" on a timer that was never
// snoozed.
void timer_toggle_repeat(Timer *t, time_t now);

// repeat only: duration_s=new_duration_s; set_total_s=new_duration_s -- "the
// new interval IS the new original," since a deliberate interval edit
// re-dials, so it re-sets what a later timer_restart() targets, same as a
// fresh create; started_at=now; accumulated_pause_s=0; is_paused=false;
// fired=false; cycles UNCHANGED; original_total_s=0 -- a deliberate
// interval edit ends any snooze window the same way a natural
// timer_advance_cycle() re-arm does (see that function's own doc).
void timer_edit_interval(Timer *t, int32_t new_duration_s, time_t now);


// Every "1/4" in the app is 1/4 of the timer's ORIGINAL SET amount -- the
// duration the user actually dialed via SET -- NOT 1/4 of a since-grown
// current duration_s. This makes repeated Up-holds a CONSTANT increment
// (20:00 -> +5:00 -> 25:00 -> +5:00 -> 30:00, linear), never a compounding
// one. For KIND_COUNTDOWN this reads set_total_s DIRECTLY -- the dedicated
// field is always populated by create time (store_add()) and re-set by
// timer_edit_interval(), so no lazy-caching-into-original_total_s dance or
// "first call" special case is needed; duration_s never needed as a
// fallback either, since set_total_s is never zero/unset in practice. Every
// quarter computation in the app -- timer_add_quarter(),
// timer_add_quarter_available(), and the alarm snooze-1/4 (gesture + menu
// rows, screen_alarm.c) -- routes through this ONE pure helper so they can
// never disagree about what "1/4" means.
//
// For KIND_REPEAT this reads duration_s (the interval) DIRECTLY instead, via
// its own explicit branch, rather than set_total_s -- a repeat's interval
// never grows on its own (the one function that ever changes it,
// timer_edit_interval, always keeps set_total_s in lockstep too), so the
// two would agree either way, but reading duration_s directly documents
// that repeat's "1/4" is simply "1/4 of the interval," no SET-vs-grown
// distinction applicable.
int32_t timer_quarter_amount(const Timer *t);

// Countdown Up-hold -- adds timer_quarter_amount(t) (1/4 of set_total_s, the
// ORIGINAL set amount -- see timer_quarter_amount's own doc), SATURATING at
// TIMER_MAX_DURATION_S (unbounded repeated holds used to compound duration_s
// exponentially and eventually overflow int32_t -- saturates rather than
// rejects, so a hold at the cap simply leaves duration_s unchanged);
// fired=false (re-arms at the new deadline, same contract as
// timer_add_minute). `now` is unused by the mutation itself (kept in the
// signature for consistency with this file's other now-taking mutations /
// future use -- mirrors timer_add_minute's sibling shape). Caller
// (screen_hero.c) is expected to only invoke this when
// timer_add_quarter_available() below is true, but this function itself
// does not re-check that gate (pure, unconditional mutation, matching every
// other function in this contract) -- the saturation cap is a SEPARATE,
// always-on safety net independent of that availability gate.
//
// The saturation additionally NEVER lets the result fall below the
// pre-mutation duration_s -- a timer dialed above the (now dial-derived)
// cap must still only ever GROW from an add-1/4 hold, never shrink, even
// though clamping the raw sum to the cap alone could otherwise produce a
// smaller number than what was already running. Belt-and-braces:
// TIMER_MAX_DURATION_S now matches the dial's own ceiling exactly (see that
// macro's doc), so this guard should be unreachable in practice, but it
// costs one comparison and removes the failure mode entirely regardless of
// how the two constants relate in the future.
void timer_add_quarter(Timer *t, time_t now);

// The Up-hold gesture's availability gate: timer_quarter_amount(t) > 10
// seconds ("gated 1/4 > 10s -- hidden otherwise", both the gesture AND its
// hint); the gate is on the ORIGINAL set amount (set_total_s for a
// countdown), same as the mutation -- a countdown that's already grown past
// the gate via prior holds stays available forever, since set_total_s never
// changes from add_quarter/add_minute/snooze. Pure predicate, no mutation.
// Deliberately takes a `const Timer*` (no kind check), so the alarm hero's
// snooze-1/4 gate reuses this SAME predicate rather than duplicating it.
bool timer_add_quarter_available(const Timer *t);

// Repeat-only, non-destructive snooze -- re-arms to fire again in
// timer_quarter_amount(t) seconds from `now` (for a repeat, duration_s --
// the interval -- never grows on its own, so it already IS the set amount;
// timer_quarter_amount reads it directly), staying KIND_REPEAT (unlike
// timer_snooze() above, which always converts to a one-shot countdown).
// started_at = now - (duration_s - quarter) so timer_remaining_s()
// immediately reads the quarter amount; accumulated_pause_s=0;
// is_paused=false; fired=false; cycles UNCHANGED (still mid-run, not a new
// interval). Caller is expected to only invoke this when
// timer_add_quarter_available() is true (same >10s gate), matching
// timer_add_quarter()'s own "gate is the caller's job" contract.
//
// Also sets original_total_s to a nonzero marker -- no new persisted field
// needed, since that field is otherwise "0 = n/a" for a repeat timer (its
// own doc: only ever set by timer_keep_counting() for a post-timer
// STOPWATCH), so reusing it here costs no new field and is already
// serialized by persist_adapter. timer_is_snoozing() below is the read side
// of this marker; timer_advance_cycle() clears it on the next natural
// re-arm ("Keep going" ends a snooze the same way it ends a normal cycle).
// The marker's VALUE is always set to the current duration_s (an interval
// never mutates while snoozing), so it happens to double as a valid
// timer_quarter_amount() cache too -- a real coincidence of this field
// meaning the same number under both of its reused purposes for a repeat,
// not a source of ambiguity (timer_is_snoozing's own kind check is what
// actually disambiguates the marker's TWO possible meanings across kinds).
// Does NOT touch set_total_s or duration_s (the interval itself never
// changes while snoozing -- a transient adjustment).
void timer_snooze_repeat(Timer *t, time_t now);

// The one-off alarm's snooze-1/4 -- a seconds-precise sibling of
// timer_snooze(t, minutes, now) above (which only takes whole minutes; the
// alarm Up-hold/menu-row snooze quantity is timer_quarter_amount(t)
// seconds). Converts to KIND_COUNTDOWN exactly like timer_snooze():
// duration_s=secs; started_at=now; accumulated_pause_s=0; is_paused=false;
// fired=false; original_total_s=0 (same reasoning as timer_snooze()'s own
// doc -- clears the snooze marker if the *source* timer was a snoozing
// repeat). Deliberately does NOT touch set_total_s (transient adjustment)
// -- see timer_snooze()'s own doc for why.
void timer_snooze_seconds(Timer *t, uint32_t secs, time_t now);

// The snooze marker's read side: true only for a repeat timer currently
// inside a timer_snooze_repeat() window (the original_total_s marker is
// nonzero). Always false for a non-repeat kind, even if original_total_s
// happens to be nonzero for an unrelated reason (e.g. a post-timer
// STOPWATCH's own provenance value) -- the kind check keeps this marker's
// two meanings from ever being confused, since a snoozed repeat by
// definition stays KIND_REPEAT and a post-timer conversion by definition
// leaves KIND_REPEAT forever. False again once timer_advance_cycle() clears
// the marker.
bool timer_is_snoozing(const Timer *t);
