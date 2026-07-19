// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// Pure bilingual (es/en) transcript parser for voice-create (v0.1.8).
// NEVER #include <pebble.h> -- must stay host-compilable for host unit testing.
//
// Pipeline: normalize (lowercase, fold es accents, strip punctuation, split hyphens, collapse whitespace) -> tokenize (fixed array, cap 24 tokens) -> classify each token against a merged es/en closed vocab (numbers, units, kind keywords, fillers, the "y"/"and" joiner) by exact match only (no edit-distance correction into any vocab class -- dictation emits whole real words, not character typos) + a small curated homophone table (position-aware: only coerced when adjacent to a unit token, so "for minutes" -> "four minutes" but "pasta for eight minutes" leaves "for" as the filler it actually is) -> one left-to-right number accumulator over any contiguous number run, guarded by a complete en+es magnitude lexicon + a structural adjacency catch ("one hundred twenty minutes" fails soft rather than silently matching "twenty minutes") -> a first-match-wins scan for an [K] N U [y|and N U] [K] group, with kind resolved by clause-scoped adjacency (a kind keyword governs the duration match iff it is reachable from it through only already-consumed connective tokens -- filler/joiner/unit/kind -- without crossing a name/clause boundary; this is what makes both "20 minutes repeat" and "repeat for five minutes" resolve REPEAT without two code paths, while "cancel the repeat timer actually just 5 minutes" still resolves COUNTDOWN) -> whatever tokens were never consumed by a recognized vocab element becomes the name residue, in original order, using each token's raw (un-normalized, un-folded) text (names keep their accents).
//
// Cheap-tier natural-time-expression fallback rules: three additional fallback rules run after the main search, each gated so it can never touch a case the base grammar already handles: (1) unit + trailing sub-count ("una hora veinte" / "an hour twenty" / "hora y veinte" -> N*3600 + a bare 0-59 minutes field) -- generalized from an hours-only rule to any unit whose own multiplier is a whole multiple of 60 (hour OR minute), so "6 minutos 45" / "six minutes forty five" -> N*60 + a bare 0-59 seconds field resolves via the exact same structural rule, parameterized on the matched unit's multiplier, not a second parallel copy; (2) a spoken M:SS pair ("seis cuarenta y cinco" / "six forty five" -> group1*60 + group2), which fires ONLY when no UNIT token exists anywhere in the utterance -- the exact boundary that keeps this rule from ever engaging on an utterance the unit-bearing accumulator already resolves. Also: diminutive unit synonyms ("minutito"/"horita"/"segundito", the Spanish colloquial forms) are additional UNIT_WORDS entries, and approximation markers ("unos"/"como"/"about"/"around"/"like") are a dedicated CLS_APPROX class -- dropped from the name residue like a filler, but deliberately kept OUT of FILLER_WORDS/CLS_FILLER so clause-scoped kind logic still treats them as opaque (FILLER-minimality invariant). The unit+trailing-sub-count rule additionally carries a residue guard: a trailing bare number binds ONLY if it is utterance-final or everything after it is pure connective glue (filler/joiner/approx) -- a real unclaimed word after it ("one hour 5 people") means those digits belong to the NAME, not this rule.
//
// A residue consisting only of number tokens (word-form or digit, no real word token anywhere in it) is never surfaced as `name` when a duration was found -- it is a missed time component (an unbound number, e.g. a trailing "45" whose duration rule didn't fire, or "sesenta" left over after "una hora" because 60 is out of the 0-59 minutes-field range), not a real user-given name. A mixed residue (>=1 real word token alongside the numbers, e.g. "5 people") is unaffected and keeps naming exactly as before.
//
// Fractions engine: three more fallback rules, unit-anchored (so ambiguity is bounded) -- (1) `<fraction> [connectives] <unit>` ("media hora"/"un cuarto de hora"/"tres cuartos de hora"/"half an hour"/"quarter of an hour"/"three quarters of an hour"); (2)/(3) a trailing HALF-word right after an already-resolved unit, either implicit N=1 ("hora y media"/"an hour and a half") or an explicit N from the main search's own match ("cinco minutos y medio"/"two minutes and a half"). A NUMBER immediately followed by a QUARTER word ("un cuarto", "tres cuartos") resolves as the fraction, not number-plus-residue -- the only legitimate reading a QUARTER word ever has in this grammar. Unit-less fraction forms ("media" alone, "cinco y medio", no unit anywhere) are deliberately left fail-soft -- they simply never reach a unit token, so no special-casing was needed to keep them dead.
//
// Four more cheap fallback additions, all gated so none can touch a case the base grammar already handles: (1) the compound sum is generalized from exactly one extra [N U] pair to an unbounded descending-unit chain (hour(3600) > minute(60) > second(1)) -- "1 hour 20 minutes 30 seconds" -> 4830s, not 4800s with the 3rd unit dropped; the old "different unit" guard is tightened into "strictly smaller unit" in the same change, which also fixes a previously-unguarded ascending-reorder bug ("5 minutes 3 hours" no longer silently sums to 11100s). (2) "in"/"within" (en only -- Spanish "en" collides with real name content far more than it serves as duration glue) join FILLER_WORDS, so "in 5 minutes"/"within ten minutes" stop leaking into the name residue. (3) "couple" (en only) joins NUMBER_WORDS as value 2; the base N-U pair search additionally skips a single FILLER immediately after it (narrowly scoped to the literal word "couple", never a general number-unit filler skip, so it cannot reopen the "once"(es 11)/"once a ___"(en idiom) collision guard) -- "a couple of minutes" -> 120s. (4) a decimal fail-soft guard: the tokenizer now splits a '.'/','  that sits between two ASCII digits exactly like a hyphen, so "5.5 minutes"/"5,5 minutos" no longer silently concatenate into one "55" token (3300s) -- the duration search instead binds to whichever digit group sits against the unit, and the other group is dropped as pure-numeric residue, producing a visibly-wrong-ish, never silently-wrong, result.
//
// Hundred-class composition: "ciento veinte segundos"/"two hundred seconds"/"one hundred twenty minutes" used to compose to a real 100-999 value (unit-anchored, range-bounded to TIMER_MAX_DURATION_S) but was removed for size optimization. Every hundred-word utterance now refuses (fail-soft, hundred word(s) left as visible residue) via the retained `hundred_present` guard -- see that guard's own doc comment in parse_nl.c and HUNDRED_WORDS_BLOB's doc comment for the guard-retention rationale. Thousand/million/billion (+ es equivalents) remain guarded as before.
//
// Absolute clock targets: "a las seis y media"/"at six forty five"/"until 6" now recognize a wall-clock instant, discriminated from the existing bare-pair duration reading purely by a preposition/article immediately preceding the clock expression ("seis cuarenta y cinco" stays the 405s duration; "a las seis cuarenta y cinco" is a clock target instead -- never both). parse_nl() itself only recognizes the target (NlParse.clock, below) -- it never reads the clock, staying pure/host-testable; resolving it to an actual countdown duration is nl_clock_target_to_duration() (also below), a second pure function the caller (dictation_am.c, or a gallery scene) invokes with its own now()-read seconds-of-day. A clock target is countdown-only by construction (see the kind-resolution comment in parse_nl.c for why a co-occurring REPEAT/STOPWATCH keyword is forced to countdown rather than guessed at or failed soft).
//
// A digit-flanked ':'/'/'/'.'/',' is preserved inside its own token instead of dropped/split -- CLS_COLONTIME ("6:45"/"18:30"/"1:20:30"), CLS_SLASHFRAC ("1/2"/"3/4"), CLS_DECIMAL ("1.5"/"5,5") -- each with its own duration-routing arm (unit-anchored, exactness-guarded, matching the existing fraction-engine style). Clock-context routing of a colon token is deferred (guarded back to fail-soft); the clock sub-grammar (immediately below) is what actually reads it as a wall-clock instant.
//
// The clock sub-grammar is extended in multiple ways, none of which touch the core "a preposition/article precedes" discriminator rule: (1) a subtractive reading -- "las seis menos cuarto"/"un cuarto para las seis"/"at ten to six" all resolve H-1:60-N instead of confidently landing on the bare hour with "menos cuarto"/"para las..." dropped as residue (this addresses a severe cross-cutting bug: "hasta las X menos cuarto" used to ring an alarm late, never early); (2) an arbitrary post-hour minute joined by "y" is read like any other NUMBER, not just "cuarto"/"media" ("a las diez y veinte"->10:20, was silently truncated to 10:00); (3) a bare trailing meridiem (no preposition needed at all) is now itself enough clock context -- "6:45 pm"/"5 pm"/"8pm"/"seis cuarenta y cinco de la tarde"/bare "noon"/"midnight"/"mediodia"/"medianoche" all resolve, and a trailing "y pico"/"y algo"/"y monedas" hedge after a resolved hour is dropped rather than leaking into the name or derailing resolution; (4) the PM branch's midnight/noon flip -- "doce de la noche" is midnight (00:00), mirroring the AM branch's pre-existing "doce"->0 special case; (5) "for" stays a duration filler by default ("timer for six thirty"->390s), but promotes to a clock target when an alarm/wake cue is present ("set an alarm for six thirty"->6:30). None of this widens the discriminator's own precondition for a bare additive hour (a prep/article must still precede it) -- see parse_nl.c's match_clock_target/match_clock_expr_ext for the exact per-shape gating (each new reading has its own narrow structural signature, not a general loosening).
//
// No allocation; fixed stack buffers only. Never crashes on any input --
// unparseable input degrades to `ok=false` (fail-soft; the caller shows the
// dial), never undefined behavior.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "timer.h"

// Matches the 24-character Timer.name cap.
#define PARSE_NL_NAME_MAX 24

// An absolute clock target, recognized (not yet resolved) by parse_nl() itself. has_clock_target false means "this utterance wasn't a clock target at all" -- the ordinary duration grammar applies instead, completely unaffected (see the discriminator's own doc comment in parse_nl.c).
// hour is 0-23 when hour24_known (an explicit am/pm marker, a direct
// 24h-range word/digit like "dieciocho"/"18", or digit "0"); otherwise
// 1-12 (a bare spoken 12h form -- AM/PM is genuinely ambiguous;
// nl_clock_target_to_duration resolves it by nearest-future occurrence).
typedef struct {
  bool   has_clock_target;
  int8_t hour;
  int8_t minute;
  bool   hour24_known;
} NlClockTarget;

typedef struct {
  TimerKind kind;         // KIND_COUNTDOWN default; REPEAT/STOPWATCH from a kind keyword
  int32_t   duration_s;   // countdown target / repeat interval; 0 for stopwatch or a failed parse
  bool      has_duration; // false => no valid N U pair found (stopwatch, clock target, or fail)
  char      name[PARSE_NL_NAME_MAX]; // residue tokens, raw UTF-8 (accents/case preserved), "" if none
  bool      has_name;
  bool      ok;           // true => a valid parse (has_duration, KIND_STOPWATCH, or a recognized
                           // clock target -- see clock.has_clock_target's own doc comment for why a
                           // clock target is "ok" before it's resolved)
  NlClockTarget clock;         // set by parse_nl() itself when recognized; {0} otherwise
  int32_t       clock_target_sod;  // NOT set by parse_nl() -- the CALLER fills this in via
                                     // nl_clock_target_to_duration()'s out_target_sod, for a confirm
                                     // screen's "-> HH:MM" annotation. Meaningless until the caller has
                                     // actually resolved clock.has_clock_target.
} NlParse;

// Parses one UTF-8 transcript into `*out`. `transcript` may be NULL or
// empty (=> ok=false, *out zeroed to defaults). Deterministic, no side
// effects, no allocation. Returns out->ok.
bool parse_nl(const char *transcript, NlParse *out);

// Resolves a recognized clock target (`target->has_clock_target` must be true) to a countdown duration -- seconds until its NEXT occurrence from `now_sod` (0..86399, local seconds-of-day; the caller reads the real clock -- lib/ stays pure). A 12h-ambiguous target (hour24_known==false) picks whichever of its AM/PM occurrence is sooner (nearest-future, the standard alarm-clock convention); an exact now==target match resolves to the NEXT occurrence (+24h if hour24_known, else the nearer of +12h/+24h), never a 0-second duration. Writes the chosen target's own seconds-of-day to *out_target_sod (for a confirm-screen "-> HH:MM" annotation; `out_target_sod` may be NULL if the caller doesn't need it). Pure, deterministic, host-testable -- never reads the clock itself, never crashes (hour/minute are clamped into range defensively). Always returns a value in 1..86400.
int32_t nl_clock_target_to_duration(const NlClockTarget *target, int now_sod,
                                     int32_t *out_target_sod);

// -- Exposed for direct unit testing --

// Resolves `tokens[0]` (a digit or an exact es/en number word -- NOT fuzzy/homophone-aware; that resolution happens in parse_nl's classify step before this is called) to a value, then applies one structural compounding rule (magnitude-class validation, not "any adjacent number tokens sum"): only when `tokens[0]` is itself a genuine word-form "tens" value (20/30/.../90 -- en twenty/thirty/..., es veinte/treinta/..., never a digit token) does it look at `tokens[1]` (optionally past a lone "y"/"and" joiner) for a "ones" (1-9) continuation to add ("treinta y cinco" = 30+5=35, "twenty five" = 20+5=25). Every other shape -- a digit token, a ones/teens word, an already-lexicalized es 21-29 token (veintitres etc.) -- resolves to exactly one token and stops there, so "twenty twenty five" / "sixty ten" / "5 5" never silently sum (the caller is left to either re-attempt matching from the next token or leave the leftover as name residue). A lone digit "45" = 45. Sets `*consumed` to how many tokens (including any interior joiner) were used; 0 if `tokens[0]` itself isn't a number token. Returns 0 if nothing matched.
int32_t nl_words_to_number(const char *const *tokens, int n, int *consumed);

// Lowercases ASCII, folds the six Spanish accented letters this app's
// vocabulary needs (a-e-i-o-u-n with acute/tilde, upper or lower) to their
// plain ASCII base letter, drops all other punctuation bytes outright (no
// boundary inserted -- "isn't" -> "isnt", "cronometro!" -> "cronometro"),
// turns whitespace AND hyphens into single-space word boundaries, and
// collapses/trims to a single space between words with no leading/trailing
// space. Truncates safely to fit `out_size` (always NUL-terminated when
// out_size > 0). `in` may be NULL (=> "").
void nl_normalize(const char *in, char *out, size_t out_size);
