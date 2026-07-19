// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

// The bilingual (es/en) host corpus for lib/parse_nl -- the load-bearing
// artifact of the original voice-parsing implementation. Every transcript
// here was run through the real parse_nl() before being locked in as an
// expectation, so this file doubles as living documentation of exactly what
// the parser accepts.
#include "parse_nl.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// Asserts parse_nl(transcript) produces exactly {exp_ok, exp_kind,
// exp_duration_s, exp_name}. `_MESSAGE` variants are used throughout so a
// failure names the offending transcript even though the assertion site is
// this shared helper, not the individual test_ function.
static void assert_parse(const char *transcript, bool exp_ok, TimerKind exp_kind,
                          int32_t exp_duration_s, const char *exp_name) {
  NlParse r;
  parse_nl(transcript, &r);

  char msg[96];
  snprintf(msg, sizeof msg, "transcript: \"%s\"", transcript ? transcript : "(null)");

  TEST_ASSERT_EQUAL_INT_MESSAGE(exp_ok, r.ok, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(exp_kind, r.kind, msg);
  TEST_ASSERT_EQUAL_INT32_MESSAGE(exp_duration_s, r.duration_s, msg);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(exp_name, r.name, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(strlen(exp_name) > 0, r.has_name, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(exp_ok, r.has_duration || exp_kind == KIND_STOPWATCH, msg);
}

// -- Template A: en basic `N U` --------------------------------------------

void test_en_basic_number_unit_pairs(void) {
  assert_parse("five minutes", true, KIND_COUNTDOWN, 300, "");
  assert_parse("30 seconds", true, KIND_COUNTDOWN, 30, "");
  assert_parse("one hour", true, KIND_COUNTDOWN, 3600, "");
  assert_parse("2 hours", true, KIND_COUNTDOWN, 7200, "");
  assert_parse("ten seconds", true, KIND_COUNTDOWN, 10, "");
  assert_parse("1 minute", true, KIND_COUNTDOWN, 60, "");
  assert_parse("fifteen minutes", true, KIND_COUNTDOWN, 900, "");
  assert_parse("3 mins", true, KIND_COUNTDOWN, 180, "");
  assert_parse("45 secs", true, KIND_COUNTDOWN, 45, "");
  assert_parse("2 hrs", true, KIND_COUNTDOWN, 7200, "");
}

// -- Template A: es basic `N U` ---------------------------------------------

void test_es_basic_number_unit_pairs(void) {
  assert_parse("cinco minutos", true, KIND_COUNTDOWN, 300, "");
  assert_parse("30 segundos", true, KIND_COUNTDOWN, 30, "");
  assert_parse("una hora", true, KIND_COUNTDOWN, 3600, "");
  assert_parse("2 horas", true, KIND_COUNTDOWN, 7200, "");
  assert_parse("diez segundos", true, KIND_COUNTDOWN, 10, "");
  assert_parse("un minuto", true, KIND_COUNTDOWN, 60, "");
  assert_parse("quince minutos", true, KIND_COUNTDOWN, 900, "");
  // "segs" (es plural abbreviation), symmetric with the already-covered
  // "seg"/en "secs".
  assert_parse("45 segs", true, KIND_COUNTDOWN, 45, "");
  assert_parse("tres minutos", true, KIND_COUNTDOWN, 180, "");
  assert_parse("cuarenta y cinco segundos", true, KIND_COUNTDOWN, 45, "");
  assert_parse("dos horas", true, KIND_COUNTDOWN, 7200, "");
}

// -- Template B: `N U K` / `K N U` -- kind keyword is position-independent, en

void test_en_repeat_kind_position_independent(void) {
  assert_parse("5 minute timer", true, KIND_COUNTDOWN, 300, "");  // explicit countdown keyword, redundant
  assert_parse("20 minutes repeat", true, KIND_REPEAT, 1200, "");
  assert_parse("repeat 20 minutes", true, KIND_REPEAT, 1200, "");
  assert_parse("every 20 minutes", true, KIND_REPEAT, 1200, "");
  assert_parse("10 minute interval", true, KIND_REPEAT, 600, "");
  assert_parse("countdown 5 minutes", true, KIND_COUNTDOWN, 300, "");
}

// -- Template B: es --------------------------------------------------------

void test_es_repeat_kind_position_independent(void) {
  assert_parse("20 minutos repetir", true, KIND_REPEAT, 1200, "");
  assert_parse("cada 20 minutos", true, KIND_REPEAT, 1200, "");
  assert_parse("repetir 20 minutos", true, KIND_REPEAT, 1200, "");
  assert_parse("10 minutos intervalo", true, KIND_REPEAT, 600, "");
  assert_parse("temporizador 5 minutos", true, KIND_COUNTDOWN, 300, "");
  assert_parse("cuenta atras 5 minutos", true, KIND_COUNTDOWN, 300, "");  // two-token es countdown phrase
}

// -- Recurring-kind vocabulary gap fix --------------------------------------
// "cinco minutos recurrente" defaulted to COUNTDOWN because "recurrente" was
// missing from the es REPEAT vocabulary -- the exact repro, both
// languages, plus kind-after-unit and kind-before-unit position variants
// (the clause-scoped kind-resolution walk must reach a trailing/leading kind
// word either way), plus a variant with an EXTRA, lower-precedence COUNTDOWN
// keyword in the same utterance (REPEAT still wins per the parser's
// STOPWATCH > REPEAT > COUNTDOWN precedence -- COUNTDOWN never overrides,
// it's the transparent default).
void test_vocab_recurrente_recurring_fold(void) {
  assert_parse("cinco minutos recurrente", true, KIND_REPEAT, 300, "");    // exact repro
  assert_parse("diez minutos recurrentes", true, KIND_REPEAT, 600, "");    // es plural
  assert_parse("recurrente cinco minutos", true, KIND_REPEAT, 300, "");    // kind-before-unit
  assert_parse("recurring five minutes", true, KIND_REPEAT, 300, "");      // en, kind-before-unit
  assert_parse("five minutes recurring", true, KIND_REPEAT, 300, "");      // en, kind-after-unit
  assert_parse("five minute recurring timer", true, KIND_REPEAT, 300, "");  // REPEAT still wins over the trailing, redundant "timer"
}

// -- "cuenta regresiva" (the other common es phrasing for "countdown,"
// alongside the already-covered "cuenta atras") and the ALARM-words ->
// COUNTDOWN mapping's article-leak fix.
void test_vocab_regresiva_and_alarma_article_fold(void) {
  assert_parse("cuenta regresiva 5 minutos", true, KIND_COUNTDOWN, 300, "");

  // ALARM-words -> COUNTDOWN mapping: "alarma"/"alarm" were already in
  // KIND_COUNTDOWN_WORDS (the fired countdown IS the alarm -- no standalone
  // alarm-creation concept exists), so kind/duration already resolved
  // correctly -- but a leading Spanish "un"/"una" (simultaneously the
  // indefinite article AND the number 1) directly in front of the kind word
  // used to leak into the name residue verbatim ("una") since English "a"/
  // "an" are FILLER_WORDS but Spanish has no equivalent filler-only article.
  // A value-1 NUMBER immediately before any kind keyword is always the
  // article, never a real quantity (this app has no "N timers from one
  // utterance" concept).
  assert_parse("ponme una alarma de cinco minutos", true, KIND_COUNTDOWN, 300, "");  // exact repro, was name="una"
  assert_parse("set an alarm for five minutes", true, KIND_COUNTDOWN, 300, "");      // en: already clean ("an" is FILLER)
  assert_parse("una alarma de cinco minutos", true, KIND_COUNTDOWN, 300, "");
  assert_parse("un temporizador de cinco minutos", true, KIND_COUNTDOWN, 300, "");   // same collision, "timer" family
  assert_parse("one alarm for five minutes", true, KIND_COUNTDOWN, 300, "");         // en "one" has the same article/number ambiguity
  // Protection: a real, bound number+unit match is untouched by this rule
  // (only STRAY, unclaimed value-1 numbers directly before a kind word are
  // affected) -- "un cronometro" alone (no duration) still resolves via the
  // pre-existing stopwatch_wins number-drop rule, unchanged.
  assert_parse("un cronometro", true, KIND_STOPWATCH, 0, "");
}

// -- Template C: `K` alone -- stopwatch, en/es -------------------------------

void test_stopwatch_keyword_alone_en_es(void) {
  assert_parse("stopwatch", true, KIND_STOPWATCH, 0, "");
  assert_parse("chrono", true, KIND_STOPWATCH, 0, "");
  assert_parse("chronometer", true, KIND_STOPWATCH, 0, "");
  assert_parse("cronometro", true, KIND_STOPWATCH, 0, "");
  assert_parse("cronómetro", true, KIND_STOPWATCH, 0, "");  // accent-folded form matches too
  assert_parse("crono", true, KIND_STOPWATCH, 0, "");
}

// -- Template D: compound sum `N U [y|and] N U`, en/es -----------------------

void test_compound_duration_sum_en_es(void) {
  assert_parse("one minute thirty seconds", true, KIND_COUNTDOWN, 90, "");
  assert_parse("two hours thirty minutes", true, KIND_COUNTDOWN, 9000, "");
  assert_parse("one minute and thirty seconds", true, KIND_COUNTDOWN, 90, "");
  assert_parse("un minuto treinta segundos", true, KIND_COUNTDOWN, 90, "");
  assert_parse("dos horas treinta minutos", true, KIND_COUNTDOWN, 9000, "");
  assert_parse("un minuto y treinta segundos", true, KIND_COUNTDOWN, 90, "");
}

// -- Templates E/F: name residue both orders, en/es --------------------------

void test_name_residue_both_orders_en_es(void) {
  assert_parse("pasta for eight minutes", true, KIND_COUNTDOWN, 480, "pasta");
  assert_parse("eight minutes pasta", true, KIND_COUNTDOWN, 480, "pasta");
  assert_parse("tea five minutes", true, KIND_COUNTDOWN, 300, "tea");
  assert_parse("five minutes tea", true, KIND_COUNTDOWN, 300, "tea");
  assert_parse("pasta ocho minutos", true, KIND_COUNTDOWN, 480, "pasta");
  assert_parse("ocho minutos pasta", true, KIND_COUNTDOWN, 480, "pasta");
  assert_parse("te cinco minutos", true, KIND_COUNTDOWN, 300, "te");
  assert_parse("cinco minutos te", true, KIND_COUNTDOWN, 300, "te");
}

// -- Number-word coverage, en 1-99 (0 is covered under range guards below) --

void test_en_number_word_coverage(void) {
  assert_parse("one minute", true, KIND_COUNTDOWN, 60, "");
  assert_parse("nine minutes", true, KIND_COUNTDOWN, 540, "");
  assert_parse("ten minutes", true, KIND_COUNTDOWN, 600, "");
  assert_parse("eleven minutes", true, KIND_COUNTDOWN, 660, "");
  assert_parse("fifteen minutes", true, KIND_COUNTDOWN, 900, "");
  assert_parse("nineteen minutes", true, KIND_COUNTDOWN, 1140, "");
  assert_parse("twenty minutes", true, KIND_COUNTDOWN, 1200, "");
  assert_parse("twenty one minutes", true, KIND_COUNTDOWN, 1260, "");
  assert_parse("twenty five minutes", true, KIND_COUNTDOWN, 1500, "");
  assert_parse("twenty-five minutes", true, KIND_COUNTDOWN, 1500, "");  // hyphen split
  assert_parse("thirty minutes", true, KIND_COUNTDOWN, 1800, "");
  assert_parse("thirty-five minutes", true, KIND_COUNTDOWN, 2100, "");
  assert_parse("forty minutes", true, KIND_COUNTDOWN, 2400, "");
  assert_parse("fifty minutes", true, KIND_COUNTDOWN, 3000, "");
  assert_parse("fifty nine minutes", true, KIND_COUNTDOWN, 3540, "");
  assert_parse("sixty minutes", true, KIND_COUNTDOWN, 3600, "");
  assert_parse("ninety minutes", true, KIND_COUNTDOWN, 5400, "");
  assert_parse("ninety nine minutes", true, KIND_COUNTDOWN, 5940, "");
}

// -- Number-word coverage, es 1-99 (veinti- single tokens + tens-y-ones) ----

void test_es_number_word_coverage(void) {
  assert_parse("un minuto", true, KIND_COUNTDOWN, 60, "");
  assert_parse("nueve minutos", true, KIND_COUNTDOWN, 540, "");
  assert_parse("diez minutos", true, KIND_COUNTDOWN, 600, "");
  assert_parse("once minutos", true, KIND_COUNTDOWN, 660, "");
  assert_parse("quince minutos", true, KIND_COUNTDOWN, 900, "");
  assert_parse("diecinueve minutos", true, KIND_COUNTDOWN, 1140, "");
  assert_parse("veinte minutos", true, KIND_COUNTDOWN, 1200, "");
  assert_parse("veintiuno minutos", true, KIND_COUNTDOWN, 1260, "");  // lexicalized single token
  assert_parse("veinticinco minutos", true, KIND_COUNTDOWN, 1500, "");
  assert_parse("treinta minutos", true, KIND_COUNTDOWN, 1800, "");
  assert_parse("treinta y cinco minutos", true, KIND_COUNTDOWN, 2100, "");  // tens-y-ones compound
  assert_parse("cuarenta minutos", true, KIND_COUNTDOWN, 2400, "");
  assert_parse("cincuenta minutos", true, KIND_COUNTDOWN, 3000, "");
  assert_parse("cincuenta y nueve minutos", true, KIND_COUNTDOWN, 3540, "");
  assert_parse("sesenta minutos", true, KIND_COUNTDOWN, 3600, "");
  assert_parse("noventa minutos", true, KIND_COUNTDOWN, 5400, "");
  assert_parse("noventa y nueve minutos", true, KIND_COUNTDOWN, 5940, "");
}

// -- Homophone robustness (position-aware: adjacent-to-UNIT only) -----------

void test_homophones_position_aware(void) {
  assert_parse("for minutes", true, KIND_COUNTDOWN, 240, "");   // for -> four
  assert_parse("to minutes", true, KIND_COUNTDOWN, 120, "");    // to -> two
  assert_parse("too minutes", true, KIND_COUNTDOWN, 120, "");   // too -> two
  assert_parse("ate minutes", true, KIND_COUNTDOWN, 480, "");   // ate -> eight
  assert_parse("won minute", true, KIND_COUNTDOWN, 60, "");     // won -> one
  // The SAME word, NOT adjacent to a unit, stays the filler it actually is
  // -- this is the case the position-aware rule exists to get right.
  assert_parse("pasta for eight minutes", true, KIND_COUNTDOWN, 480, "pasta");
}

void test_es_para_stays_filler_not_a_homophone(void) {
  // Deliberate deviation: "para"<->"cuatro" was NOT added to the homophone
  // table (unlike the EN entries) because "para" is also the ordinary
  // Spanish filler "for", used constantly in exactly this phrasing -- the
  // position rule can't tell that apart from a mis-heard "cuatro" the way
  // it can for the EN homophones.
  assert_parse("temporizador para cinco minutos", true, KIND_COUNTDOWN, 300, "");
}

// -- Regression lock: known-tricky inputs ------------------------------------
// Every case below pins the correct parse for an input that once produced a
// wrong result (or the digit-token bug below), asserting the fixed behavior
// rather than the old, incorrect one.

// Fuzzy false positives: the edit-distance fuzzy layer is deleted entirely,
// so EVERY word that used to sit at Levenshtein-1 from a vocab word
// (not just the 3 originally blocklisted) now stays CLS_WORD -- closing
// this as a CLASS, not a growing blocklist. Real dictation-plausible words
// that
// previously miscorrected (night->eight, even->seven, threw->three,
// thirsty->thirty, fifth->fifty, sixth->sixty, mighty->eighty,
// fort/forte->forty, nuevo->nueve, cinto->cinco, crece->trece, sieve->siete,
// yours->hours) all land as name residue (or fail-soft with no unit) rather
// than fabricating a duration from nothing.
void test_fuzzy_false_positives_fixed(void) {
  assert_parse("night minutes", false, KIND_COUNTDOWN, 0, "night");
  assert_parse("even minutes", false, KIND_COUNTDOWN, 0, "even");
  assert_parse("even five minutes", true, KIND_COUNTDOWN, 300, "even");
  assert_parse("not even five minutes", true, KIND_COUNTDOWN, 300, "not even");
  assert_parse("nuevo cinco minutos", true, KIND_COUNTDOWN, 300, "nuevo");
  assert_parse("temporizador nuevo cinco minutos", true, KIND_COUNTDOWN, 300, "nuevo");
  assert_parse("threw minutes", false, KIND_COUNTDOWN, 0, "threw");
  assert_parse("thirsty minutes", false, KIND_COUNTDOWN, 0, "thirsty");
  assert_parse("fifth minutes", false, KIND_COUNTDOWN, 0, "fifth");
  assert_parse("sixth minutes", false, KIND_COUNTDOWN, 0, "sixth");
  assert_parse("mighty minutes", false, KIND_COUNTDOWN, 0, "mighty");
  assert_parse("forte minutes", false, KIND_COUNTDOWN, 0, "forte");
  assert_parse("fort minutes", false, KIND_COUNTDOWN, 0, "fort");
  assert_parse("cinto minutos", false, KIND_COUNTDOWN, 0, "cinto");
  assert_parse("crece minutos", false, KIND_COUNTDOWN, 0, "crece");
  assert_parse("sieve minutes", false, KIND_COUNTDOWN, 0, "sieve");
  // Confirms the exposure closed isn't limited to the number-fuzz table --
  // the (now-deleted) unit-word fuzz had the identical shape and no
  // blocklist of its own at all. "yours" != "hours" under exact-only match;
  // a bare "5" with no recognized unit fails soft.
  assert_parse("5 yours", false, KIND_COUNTDOWN, 0, "5 yours");
}

// Number accumulator structural validation: nl_words_to_number now
// only compounds a genuine WORD-form "tens" value (20-90) with a following
// "ones" (1-9) -- never tens+tens, tens+teens, or digit+digit -- and the
// compound-sum template (D) now requires its second N-U pair to use a
// DIFFERENT unit than the first. The legit compounds this must NOT break
// ("twenty five minutes", "veinticinco minutos", "treinta y cinco
// minutos", "one minute thirty seconds") are already locked in by
// test_en_number_word_coverage / test_es_number_word_coverage /
// test_compound_duration_sum_en_es above -- re-asserted here isn't needed,
// but this function documents that they remain correct against the current
// nl_words_to_number implementation.
void test_number_accumulator_structural_validation(void) {
  // Tens directly followed by another tens -- rejected; the parser instead
  // finds the second "twenty five" as a genuine tens+ones compound, leaving
  // the first, unrelated "twenty" unconsumed -- a PURE-numeric leftover, so
  // it is dropped rather than surfaced as name (silently summing to 45
  // minutes is still correctly avoided;
  // only the leftover's fate as a *name* changed, not the duration).
  assert_parse("twenty twenty five minutes", true, KIND_COUNTDOWN, 1500, "");
  // Ones followed by tens -- "five" can't extend into "twenty" (only a
  // TENS word may compound, and only with a following ONES word) -- the
  // parser instead matches "twenty minutes" and leaves "five" unconsumed;
  // pure-numeric, dropped (not surfaced as name).
  assert_parse("five twenty minutes", true, KIND_COUNTDOWN, 1200, "");
  // Tens followed by a teen (ten=10 is not a 1-9 "ones" value) -- rejected;
  // the parser falls through to matching "ten minutes" alone, leaving
  // "sixty" unconsumed; pure-numeric, dropped.
  assert_parse("sixty ten minutes", true, KIND_COUNTDOWN, 600, "");
  // Digit tokens never compound with each other at all (unlike word-form
  // tens+ones) -- the second "5" is left unconsumed, not summed; pure-
  // numeric, dropped.
  assert_parse("5 5 minutes", true, KIND_COUNTDOWN, 300, "");
  // Compound-sum template D now requires a DIFFERENT unit for the second
  // pair -- a restated/repeated duration ("5 minutes... 5 minutes") is no
  // longer silently summed into 10 minutes; the leftover "5" is pure-
  // numeric, dropped.
  assert_parse("5 minutes 5 minutes", true, KIND_COUNTDOWN, 300, "");
}

// Kind resolution is CLAUSE-SCOPED: a kind keyword governs the
// duration match iff it is reachable from it through only already-consumed
// connective tokens (filler/joiner/unit/kind), stopping at the first
// unconsumed name-word. This closes the resolution in BOTH directions at
// once: the anywhere-scan regression ("cancel the repeat timer actually
// just 5 minutes" must NOT lock REPEAT) AND the exactly-one-token-adjacency
// regression ("repeat for five minutes" / "repeat timer for five minutes"
// must NOT fall to COUNTDOWN just because a filler/kind token sits between
// the keyword and the match).
void test_kind_resolution_is_clause_scoped(void) {
  assert_parse("repeat 5 minutes", true, KIND_REPEAT, 300, "");
  assert_parse("repeat five minutes", true, KIND_REPEAT, 300, "");
  assert_parse("repeat for five minutes", true, KIND_REPEAT, 300, "");
  assert_parse("repeat timer for five minutes", true, KIND_REPEAT, 300, "");
  assert_parse("repeating timer for 5 minutes", true, KIND_REPEAT, 300, "");
  assert_parse("repeat alarm for five minutes", true, KIND_REPEAT, 300, "");
  assert_parse("make a repeating timer for 5 minutes", true, KIND_REPEAT, 300, "make");
  // "want" now joins NAMEDROP_WORDS (the carriers/politeness list) -- it
  // is dropped from the residue like "please"/"could"/"just" elsewhere in
  // this file, so the name shrinks from "i want" to "i". The KIND result
  // (REPEAT) and the LEFT-walk boundary reasoning are completely
  // unaffected: CLS_NAMEDROP is just as opaque to the clause-scoped kind
  // walk as the CLS_WORD "want" used to be (see the CLS_NAMEDROP enum comment) -- only
  // residue CONSUMPTION changed.
  assert_parse("i want a repeat timer for five minutes", true, KIND_REPEAT, 300, "i");
  assert_parse("5 minute repeat timer", true, KIND_REPEAT, 300, "");
  assert_parse("20 minutes repeat", true, KIND_REPEAT, 1200, "");
  assert_parse("every 20 minutes", true, KIND_REPEAT, 1200, "");
  assert_parse("cada 20 minutos", true, KIND_REPEAT, 1200, "");
  assert_parse("repetir temporizador para cinco minutos", true, KIND_REPEAT, 300, "");
  assert_parse("timer five minutes", true, KIND_COUNTDOWN, 300, "");
  assert_parse("5 minute timer", true, KIND_COUNTDOWN, 300, "");
  // Name/clause boundary: "just" is CLS_NAMEDROP -- deliberately NOT a
  // filler (see the FILLER_WORDS invariant comment) -- so it is just as
  // OPAQUE to the LEFT walk as the CLS_WORD it used to be (see the
  // CLS_NAMEDROP enum comment: never added to the walk's "continue" set) --
  // the walk still stops there before ever reaching "repeat", the
  // regression this clause-scoping must still close. The name itself shrinks from
  // "cancel the actually jus" (truncated "just") to "cancel the actually"
  // (no truncation needed) now that "just" is consumed/dropped instead of
  // surviving as residue.
  assert_parse("cancel the repeat timer actually just 5 minutes", true, KIND_COUNTDOWN, 300,
               "cancel the actually");
  // "to" sits FARTHER from the match than "repeat" does ("for" is the only
  // token directly between "repeat" and the match), so the walk reaches and
  // adds "repeat" to the consumed set before it ever reaches "to" -- REPEAT
  // is what the clause-scoped algorithm, applied literally with no special
  // case for "to", actually produces here. Pinned as REPEAT (not the
  // alternative COUNTDOWN reading a naive rewrite might expect), since
  // REPEAT is the one the plain, unmodified algorithm actually yields.
  assert_parse("set it to repeat for five minutes", true, KIND_REPEAT, 300, "it to");
  assert_parse("stopwatch 5 minutes", true, KIND_STOPWATCH, 0, "");
  assert_parse("cronometro 20 minutos", true, KIND_STOPWATCH, 0, "");
}

// "set" (and es "pon"/"ponme") are now FILLER_WORDS entries -- the
// single most canonical assistant phrasing no longer pollutes the name.
void test_set_and_pon_are_fillers(void) {
  assert_parse("set a timer for five minutes", true, KIND_COUNTDOWN, 300, "");
  assert_parse("set timer for 5 minutes", true, KIND_COUNTDOWN, 300, "");
  assert_parse("pon temporizador de cinco minutos", true, KIND_COUNTDOWN, 300, "");
}

// NL_MAX_TOKENS raised 12 -> 24 -- a realistic politeness preamble no
// longer silently swallows the actual duration before it's ever tokenized.
// "please"/"could"/"kindly"/"just" all join NAMEDROP_WORDS (the carriers/
// politeness + disfluency lists) -- every one of them is now dropped from
// the residue instead of surviving as name text. The remaining unconsumed
// real words ("you"/"go"/"ahead"/"exactly"/"thanks") are what now fills
// the name, truncated at PARSE_NL_NAME_MAX (24, so 23 usable bytes + NUL)
// exactly as before -- this only changes WHICH words survive to be
// truncated, not the cap mechanism itself (still proven live: "thanks"
// itself is cut to "th").
void test_token_cap_handles_long_preamble(void) {
  assert_parse(
      "please could you kindly go ahead and just set a timer for exactly five minutes thanks",
      true, KIND_COUNTDOWN, 300, "you go ahead exactly th");
}

// The magnitude guard: the COMPLETE en+es magnitude lexicon adjacent to a
// number token -- directly, or across a single optional joiner -- fails the
// WHOLE duration search soft, rather than letting an unrelated, independent
// number+unit pair elsewhere in the utterance silently "rescue" a
// wrong-magnitude match. "thousand"/"mil" (previously unlisted) close this
// way. "pasta veinte minutos" is the required negative case: an ordinary
// name word that is NOT a magnitude must keep parsing normally.
//
// "hundred"/"cien"/"ciento" are NO LONGER part of this guard's blanket
// coverage -- they used to COMPOSE (match_hundred_composite) instead of
// blocking, but the composer was REMOVED for size in v0.1.13 (1,063 B for
// 10 rarely-used cases, the worst size-per-case tradeoff on the cull
// ladder) -- the `hundred_present` GUARD stays, so these rows are re-pinned
// back to REFUSE (never a wrong value: "hundred" stays visible, unconsumed,
// as residue -- "veinte"/"twenty" never silently "rescues" the phrase).
// "thousand"/"mil" stay exactly as guarded as before (unaffected by the
// composer's removal -- see also test_hundred_composition's own dedicated
// magnitude-guard re-assert).
void test_magnitude_guard_complete_lexicon(void) {
  // culled-v0.1.13: was `true, KIND_COUNTDOWN, 7200, ""` pre-cull (the
  // composed this to 7200s); the composer is gone, `hundred_present` alone
  // now refuses the whole phrase, leaving "hundred twenty" unconsumed.
  assert_parse("one hundred twenty minutes", false, KIND_COUNTDOWN, 0, "one hundred twenty");
  // culled-v0.1.13: was `true, KIND_COUNTDOWN, 7200, ""` pre-cull.
  assert_parse("ciento veinte minutos", false, KIND_COUNTDOWN, 0, "ciento veinte");
  assert_parse("one thousand twenty minutes", false, KIND_COUNTDOWN, 0, "one thousand twenty");
  assert_parse("mil veinte minutos", false, KIND_COUNTDOWN, 0, "mil veinte");
  // "and" is a JOINER token, unconditionally consumed regardless of the
  // (now-removed) hundred composer (same residue-dropping convention as
  // everywhere else in this file).
  // culled-v0.1.13: was `true, KIND_COUNTDOWN, 7200, ""` pre-cull (the
  // composed this to 7200s); "hundred" itself is CLS_WORD, never consumed
  // by the JOINER "and", so it survives as the sole residue.
  assert_parse("a hundred and twenty minutes", false, KIND_COUNTDOWN, 0, "hundred twenty");
  assert_parse("cien horas", false, KIND_COUNTDOWN, 0, "cien");
  assert_parse("pasta veinte minutos", true, KIND_COUNTDOWN, 1200, "pasta");
}

// A structural "#-WORD-#" catch used to fire for ANY unrecognized word
// between two numbers, not just a plausible missed magnitude morpheme --
// failing the ENTIRE parse on ordinary phrasings like "5 people 5 minutes"
// (a headcount/name token, not a magnitude claim). Narrowed to only fire
// when the intervening word is itself close (bounded edit distance <=1) to
// a KNOWN magnitude morpheme (word_near_magnitude_lexicon(), parse_nl.c) --
// these repros must now parse normally: duration + the embedded-number
// name as residue.
void test_benign_embedded_number_names_no_longer_fail_the_whole_parse(void) {
  assert_parse("5 people 5 minutes", true, KIND_COUNTDOWN, 300, "5 people");
  assert_parse("5 rounds 5 minutes", true, KIND_COUNTDOWN, 300, "5 rounds");
  assert_parse("table 5 people 5 minutes", true, KIND_COUNTDOWN, 300, "table 5 people");
  assert_parse("5 friends 20 minutes", true, KIND_COUNTDOWN, 1200, "5 friends");
  assert_parse("cinco amigos veinte minutos", true, KIND_COUNTDOWN, 1200, "cinco amigos");
}

// The magnitude-protection re-guard: the narrowing above must NOT reopen the
// THOUSAND+ repros (all already caught upstream by the EXACT lexicon
// guard, unaffected by this change) -- re-asserted here as an explicit
// regression pin, plus ONE new case the exact lexicon guard alone would
// miss: a magnitude word with a single-character typo ("thousnd"), which
// the belt-and-suspenders catch -- now narrowed to "near the lexicon"
// rather than "any word" -- must still fail soft (this is the exact class
// the catch exists for). The thousand-and-larger check is a separate, still-live guard
// from the hundred-class `hundred_present` guard (see
// test_magnitude_guard_complete_lexicon and test_hundred_composition
// for the hundred-class rows) and is unaffected by the hundred composer's
// removal.
void test_magnitude_protections_still_fail_soft(void) {
  assert_parse("one thousand twenty minutes", false, KIND_COUNTDOWN, 0, "one thousand twenty");
  assert_parse("mil veinte minutos", false, KIND_COUNTDOWN, 0, "mil veinte");
  assert_parse("one thousnd twenty minutes", false, KIND_COUNTDOWN, 0, "one thousnd twenty");
}

// Digit-token bug: parse_nl("cada 20 minutos") must parse to
// (KIND_REPEAT, 1200s). Root cause: parse_digits() used strtol(), which
// intermittently misbehaved on-device (ARM/-Os) depending on unrelated
// stack layout in the caller -- confirmed via the real emulator (not just
// the host build, which never reproduced it) that the SAME transcript
// through the SAME parse_nl() call returned duration_s=0 on-device while
// the host build always returned 1200. Replaced with a hand-rolled digit
// loop (no libc numeric-conversion call at all). These digit-token +
// "cada"/repeat-keyword shapes are the exact ones that exercised the buggy
// path -- kept together as their own regression group.
void test_digit_token_repeat_regression(void) {
  assert_parse("cada 20 minutos", true, KIND_REPEAT, 1200, "");
  assert_parse("cada 5 minutos", true, KIND_REPEAT, 300, "");
  assert_parse("20 minutos", true, KIND_COUNTDOWN, 1200, "");
}

// -- Low-priority corpus gaps (5, priority-ordered) -------------------------

// Bare article + unit never resolves to 1x (fails soft, unchanged product
// scope) -- but "a"/"an" are now BOTH fillers, so the two closely related
// phrasings leave the same (empty) residue instead of differing.
// "a"/"an" immediately before a UNIT now resolves the implicit-count-1
// idiom ("an hour"->3600, "a minute"->60), closing the en/es symmetry hole
// (es "un minuto"/"una hora" already worked). The old boundary this test
// locked in (bare "a"/"an"+unit stays fail-soft) has since been re-specced
// from false to true/60/3600.
void test_bare_article_plus_unit(void) {
  assert_parse("a minute", true, KIND_COUNTDOWN, 60, "");
  assert_parse("an hour", true, KIND_COUNTDOWN, 3600, "");
}

// Fractional durations -- UPDATED: the fractions engine now parses the
// unit-ANCHORED forms (this exact repro included -- "an hour and a half"
// -> 5400s, see test_fraction_acceptance_matrix below). What remains a
// deliberate scope boundary (not in the 0-99 integer grammar) is the
// UNIT-LESS fraction form -- no unit token anywhere in the utterance --
// which stays fail-soft, resolved that way deliberately. Kept as its own
// function (rather than deleted) so this scope boundary still reads as
// intentional, just narrower than before.
void test_unitless_fractional_durations_unsupported(void) {
  assert_parse("half", false, KIND_COUNTDOWN, 0, "half");
  assert_parse("media", false, KIND_COUNTDOWN, 0, "media");
  assert_parse("cinco y medio", false, KIND_COUNTDOWN, 0, "cinco medio");
}

// Merged-vocab code-switch collision: EN "once" (as in "check it once") is
// identical to ES "once"=11. The immediate-adjacency requirement for a
// number-run mostly protects against this in practice (a filler breaks
// adjacency, as in "once a minute" below), but a directly-adjacent
// phrasing does resolve to 11x -- a known, narrow, accepted trade-off of
// this cross-language vocabulary collision.
void test_once_en_es_collision(void) {
  assert_parse("once minutes", true, KIND_COUNTDOWN, 660, "");   // es "once"=11 -- 11 min
  assert_parse("once a minute", false, KIND_COUNTDOWN, 0, "once");  // filler breaks adjacency
}

// nl_normalize's non-accent multi-byte UTF-8 handling silently merges
// words with no boundary inserted when they're joined by an unsupported
// multi-byte character (e.g. a non-breaking space, UTF-8 C2 A0) with no
// adjacent ASCII whitespace -- a data-quality note, not a safety one (never
// reads out of bounds, confirmed by fuzzing).
void test_normalize_merges_across_unsupported_multibyte(void) {
  char buf[64];
  nl_normalize("cafe\xC2\xA0late", buf, sizeof buf);  // "cafe" + NBSP + "late", no ASCII boundary
  TEST_ASSERT_EQUAL_STRING("cafelate", buf);
}

// -- Fuzzy-match removal: dictation emits whole real words, never
// character-level typos, so these former "typo tolerance" cases now
// correctly fail soft (or fall to a plain exact-match parse treating the
// un-recognized word as a name). This test function replaces the deleted
// test_levenshtein_typo_tolerance and the former fuzzy-gate boundary test,
// documenting the removal rather than silently losing coverage.
void test_dictation_emits_real_words_not_typos(void) {
  assert_parse("5 minut", false, KIND_COUNTDOWN, 0, "5 minut");     // no fuzzy rescue -- minut isn't a vocab word
  assert_parse("5 hras", false, KIND_COUNTDOWN, 0, "5 hras");  // former fuzzy-gate boundary case
  assert_parse("stopwach", false, KIND_COUNTDOWN, 0, "stopwach");   // no longer fuzzes to "stopwatch"
  // "repet" no longer fuzzes to "repeat" -- it's an unrecognized CLS_WORD,
  // so the utterance still parses (the number+unit pair is untouched) but
  // the kind stays the COUNTDOWN default and "repet" lands in the name.
  assert_parse("repet 20 minutos", true, KIND_COUNTDOWN, 1200, "repet");
}

// -- Code-switching (mixed es/en in one utterance) ---------------------------

void test_code_switch_mixed_language(void) {
  assert_parse("timer de cinco minutos", true, KIND_COUNTDOWN, 300, "");
  assert_parse("temporizador for 5 minutes", true, KIND_COUNTDOWN, 300, "");
  assert_parse("5 minutos timer", true, KIND_COUNTDOWN, 300, "");
  assert_parse("cronometro stopwatch", true, KIND_STOPWATCH, 0, "");
}

// -- Spanish "y": number-joiner vs plain filler, resolved by position -------

void test_spanish_y_joiner_vs_filler(void) {
  assert_parse("treinta y cinco minutos", true, KIND_COUNTDOWN, 2100, "");  // joiner: between two numbers
  // Not between two numbers -- "y" is dropped like any other filler/joiner
  // (never in the name), but since nothing forms an N U pair here, the
  // parse itself still fails soft.
  assert_parse("pasta y te", false, KIND_COUNTDOWN, 0, "pasta te");
  assert_parse("cafe y pastel cinco minutos", true, KIND_COUNTDOWN, 300, "cafe pastel");
}

// -- Ambiguous / unreliable shapes -- fallback (ok=false), never a crash ----

void test_ambiguous_shapes_fall_back(void) {
  assert_parse("twenty", false, KIND_COUNTDOWN, 0, "twenty");    // bare number, no unit
  assert_parse("veinte", false, KIND_COUNTDOWN, 0, "veinte");
  // "set" is a FILLER_WORDS entry so it no longer pollutes the residue --
  // this free-form sentence now fits under PARSE_NL_NAME_MAX without
  // truncation.
  assert_parse("set a timer for when the pasta is done", false, KIND_COUNTDOWN, 0,
               "when the pasta is done");
  // Unit-before-number is not a supported shape either way. Removing the
  // fuzzy layer changes the residue here: "minut" no longer fuzzes to the UNIT "minuto" (and so
  // is no longer silently consumed) -- it now stays an unrecognized CLS_WORD
  // and shows up in the name alongside "cinco", which is the more honest
  // result (nothing about this utterance was actually understood).
  assert_parse("minut cinco", false, KIND_COUNTDOWN, 0, "minut cinco");
  // Kind+number conflict: an explicit stopwatch keyword wins outright, and
  // the number is DROPPED (not left over as name residue either).
  assert_parse("stopwatch 5 minutes", true, KIND_STOPWATCH, 0, "");
  assert_parse("cronometro 20 minutos", true, KIND_STOPWATCH, 0, "");
  // "cien" (100, word form) isn't in the 0-99 vocab (scope: word-form
  // coverage is 0-99 + hours; hundreds are out of grammar) -- contrast with
  // the DIGIT form "100 hours" below, which parses fine but is rejected by
  // the range guard instead.
  assert_parse("cien horas", false, KIND_COUNTDOWN, 0, "cien");
}

// -- Range guards: reject 0, reject > 99h, accept exactly 99h ---------------

void test_range_guards_zero_and_over_99h(void) {
  assert_parse("cero minutos", false, KIND_COUNTDOWN, 0, "");
  assert_parse("zero minutes", false, KIND_COUNTDOWN, 0, "");
  assert_parse("0 minutes", false, KIND_COUNTDOWN, 0, "");
  assert_parse("100 hours", false, KIND_COUNTDOWN, 0, "");       // digit form: parses, then range-rejected
  assert_parse("99 hours", true, KIND_COUNTDOWN, 356400, "");    // exactly at the boundary -- accepted
  assert_parse("noventa y nueve horas", true, KIND_COUNTDOWN, 356400, "");
}

// -- Name residue preserves raw UTF-8 (accents/case), matching only folds --

void test_name_residue_preserves_raw_accents_and_case(void) {
  assert_parse("Tea for five minutes", true, KIND_COUNTDOWN, 300, "Tea");
  assert_parse("Té por cinco minutos", true, KIND_COUNTDOWN, 300, "Té");
  assert_parse("cafe con leche diez minutos", true, KIND_COUNTDOWN, 600, "cafe con leche");
  assert_parse("café con leche diez minutos", true, KIND_COUNTDOWN, 600, "café con leche");
}

// -- Garbage / empty / null -- must never crash, always fail soft ----------

void test_garbage_and_edge_inputs_never_crash(void) {
  assert_parse("", false, KIND_COUNTDOWN, 0, "");
  assert_parse(NULL, false, KIND_COUNTDOWN, 0, "");
  assert_parse("   ", false, KIND_COUNTDOWN, 0, "");
  assert_parse("!!!", false, KIND_COUNTDOWN, 0, "");
  assert_parse("uhh", false, KIND_COUNTDOWN, 0, "uhh");
  assert_parse("umm five minutes", true, KIND_COUNTDOWN, 300, "umm");
}

// -- nl_normalize (direct) ---------------------------------------------------

void test_normalize_folds_accents_lowercases_and_strips_punct(void) {
  char buf[64];
  nl_normalize("Cronómetro!", buf, sizeof buf);
  TEST_ASSERT_EQUAL_STRING("cronometro", buf);
}

void test_normalize_splits_hyphens_into_spaces(void) {
  char buf[64];
  nl_normalize("Twenty-Five", buf, sizeof buf);
  TEST_ASSERT_EQUAL_STRING("twenty five", buf);
}

void test_normalize_collapses_and_trims_whitespace(void) {
  char buf[64];
  nl_normalize("  Multiple   Spaces  ", buf, sizeof buf);
  TEST_ASSERT_EQUAL_STRING("multiple spaces", buf);
}

void test_normalize_null_input_is_empty_string(void) {
  char buf[8];
  nl_normalize(NULL, buf, sizeof buf);
  TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_normalize_folds_all_six_spanish_accents(void) {
  char buf[64];
  nl_normalize("Ñoño más café", buf, sizeof buf);
  TEST_ASSERT_EQUAL_STRING("nono mas cafe", buf);
}

void test_normalize_drops_punctuation_without_inserting_a_boundary(void) {
  char buf[64];
  nl_normalize("isn't", buf, sizeof buf);
  TEST_ASSERT_EQUAL_STRING("isnt", buf);
}

void test_normalize_truncates_safely_never_overflows(void) {
  char buf[4];
  nl_normalize("abcdefgh", buf, sizeof buf);
  TEST_ASSERT_EQUAL_STRING("abc", buf);  // 3 chars + NUL fits a 4-byte buffer
}

// -- nl_words_to_number (direct) ---------------------------------------------

void test_words_to_number_plain_digit(void) {
  const char *toks[] = {"45"};
  int consumed = -1;
  TEST_ASSERT_EQUAL_INT32(45, nl_words_to_number(toks, 1, &consumed));
  TEST_ASSERT_EQUAL_INT(1, consumed);
}

void test_words_to_number_en_compound_tens_and_ones(void) {
  const char *toks[] = {"twenty", "five"};
  int consumed = -1;
  TEST_ASSERT_EQUAL_INT32(25, nl_words_to_number(toks, 2, &consumed));
  TEST_ASSERT_EQUAL_INT(2, consumed);
}

void test_words_to_number_es_veinti_single_token(void) {
  const char *toks[] = {"veintitres"};
  int consumed = -1;
  TEST_ASSERT_EQUAL_INT32(23, nl_words_to_number(toks, 1, &consumed));
  TEST_ASSERT_EQUAL_INT(1, consumed);
}

void test_words_to_number_es_tens_y_ones_compound(void) {
  const char *toks[] = {"treinta", "y", "cinco"};
  int consumed = -1;
  TEST_ASSERT_EQUAL_INT32(35, nl_words_to_number(toks, 3, &consumed));
  TEST_ASSERT_EQUAL_INT(3, consumed);
}

void test_words_to_number_non_number_first_token_consumes_nothing(void) {
  const char *toks[] = {"pasta", "five"};
  int consumed = -1;
  TEST_ASSERT_EQUAL_INT32(0, nl_words_to_number(toks, 2, &consumed));
  TEST_ASSERT_EQUAL_INT(0, consumed);
}

void test_words_to_number_joiner_not_followed_by_number_is_not_consumed(void) {
  const char *toks[] = {"treinta", "y", "pasta"};
  int consumed = -1;
  TEST_ASSERT_EQUAL_INT32(30, nl_words_to_number(toks, 3, &consumed));
  TEST_ASSERT_EQUAL_INT(1, consumed);  // only "treinta" -- the joiner is left for the caller
}

// -- Spoken M:SS pair rule ---------------------------------------------------
// New behavior: unit-less two-group spoken M:SS forms (below) are now
// accepted. The unchanged unit-bearing accumulator shapes are the cases the
// pair rule must NEVER touch (every one of them carries a unit token, which
// is the pair rule's exact precondition boundary) -- kept here, re-asserted
// under their own values, as this test's own explicit pair-rule/accumulator
// boundary protection, alongside test_number_accumulator_structural_validation
// above (unchanged, still the primary accumulator regression lock).
void test_spoken_pair_rule_acceptance_matrix(void) {
  // New: unit-less two-group spoken M:SS.
  assert_parse("seis cuarenta y cinco", true, KIND_COUNTDOWN, 405, "");
  assert_parse("six forty five", true, KIND_COUNTDOWN, 405, "");
  assert_parse("cero cinco", true, KIND_COUNTDOWN, 5, "");  // leading-zero pair, 0:05 = 5s
  assert_parse("twenty twenty five", true, KIND_COUNTDOWN, 1225, "");  // no unit: pair rule applies

  // Repeated same-value number groups are a correct, symmetric consequence
  // of this rule plus the existing accumulator group-splitting logic -- locked here
  // as a deliberate outcome so a future refactor can't silently change it
  // either direction.
  assert_parse("veinte veinte", true, KIND_COUNTDOWN, 1220, "");  // 20:20
  assert_parse("cinco cinco", true, KIND_COUNTDOWN, 305, "");     // 5:05

  // Accumulator cases -- duration/kind unchanged (every row carries a unit; the pair
  // rule's no-unit-anywhere precondition keeps it inert here). Name text
  // updated: the leftover token in each row is pure-numeric, so the
  // residue rule now drops it instead of surfacing it as a name (see
  // test_number_accumulator_structural_validation for the full reasoning per
  // row -- this is the same corpus, restated).
  assert_parse("twenty twenty five minutes", true, KIND_COUNTDOWN, 1500, "");
  assert_parse("five twenty minutes", true, KIND_COUNTDOWN, 1200, "");
  assert_parse("sixty ten minutes", true, KIND_COUNTDOWN, 600, "");
  assert_parse("5 5 minutes", true, KIND_COUNTDOWN, 300, "");

  // Reject: group-2 > 59 (not a valid seconds/minutes field; also guards
  // year-like mis-reads).
  assert_parse("diez sesenta", false, KIND_COUNTDOWN, 0, "diez sesenta");
  assert_parse("nineteen ninety", false, KIND_COUNTDOWN, 0, "nineteen ninety");

  // Reject: one group (bare number, no unit) -- unchanged fail-soft.
  assert_parse("five", false, KIND_COUNTDOWN, 0, "five");

  // Reject: three groups, no unit -- no H:MM:SS guessing.
  assert_parse("six forty five thirty", false, KIND_COUNTDOWN, 0, "six forty five thirty");

  // Unit present -> stays on the existing (non-pair) path deliberately.
  // Leftover "six" is pure-numeric -- dropped, not surfaced as name.
  assert_parse("six forty five minutes", true, KIND_COUNTDOWN, 2700, "");

  // Kind keyword wins outright; numbers dropped (unchanged, pair rule's
  // internal match gets overridden the same way any number match does).
  assert_parse("stopwatch six forty five", true, KIND_STOPWATCH, 0, "");
}

// -- Hours + trailing minutes -----------------------------------------------
// "<N> <hour-unit> <M>" with M a bare (unit-less) 0-59 number -> N*3600 +
// M*60. Three shapes: an explicit number+hour match extended ("una hora
// veinte"), an implicit-1-hour bare unit with a filler article ("an hour
// twenty"), and an implicit-1-hour bare unit with a joiner ("hora y
// veinte") -- all three land on the same 4800s. This matrix is the pinned
// regression-lock for the hour side of the unit + trailing sub-count rule
// in parse_nl.c (parameterized on the matched unit's own multiplier) --
// every row here is re-asserted UNTOUCHED by that generalization (same
// duration/kind/ok for every row; only one row's *name* text changed, per
// the separate pure-numeric-residue fix, noted at its own assertion
// below). See test_minutes_trailing_seconds below for the symmetric
// minute-level rows the generalization adds.
void test_hours_trailing_minutes(void) {
  assert_parse("una hora veinte", true, KIND_COUNTDOWN, 4800, "");
  assert_parse("an hour twenty", true, KIND_COUNTDOWN, 4800, "");
  assert_parse("hora y veinte", true, KIND_COUNTDOWN, 4800, "");

  // Extra coverage: the same rule generalizes to
  // an explicit hour count > 1 (not just the implicit-1 idiom).
  assert_parse("dos horas veinte", true, KIND_COUNTDOWN, 8400, "");
  assert_parse("two hours twenty", true, KIND_COUNTDOWN, 8400, "");

  // Protections: bare "an hour"/"una hora" alone (no trailing number) must
  // stay exactly as test_bare_article_plus_unit/test_en_basic/
  // test_es_basic already lock in -- re-asserted here as this rule's own
  // boundary check, since it is the rule most likely to accidentally reach
  // into these. "an hour" now resolves to 3600 (see
  // test_bare_article_plus_unit's own comment) via its own dedicated
  // fallback, not this rule (this rule's bare-unit scan never binds
  // without a trailing sub-count, unchanged).
  assert_parse("an hour", true, KIND_COUNTDOWN, 3600, "");
  assert_parse("una hora", true, KIND_COUNTDOWN, 3600, "");
  assert_parse("one hour", true, KIND_COUNTDOWN, 3600, "");
  // A trailing number > 59 does not qualify as a bare minutes field; the
  // hour alone still resolves, the extra token is left unconsumed --
  // pure-numeric, so the residue rule drops it (was surfaced as name
  // "sesenta" before that fix).
  assert_parse("una hora sesenta", true, KIND_COUNTDOWN, 3600, "");

  // A residue guard refinement: the trailing number binds ONLY if it is
  // utterance-final or everything after it is pure connective glue
  // (filler/joiner/approx). A real unclaimed word after it means those
  // digits belong to the NAME, not the hour rule -- the base "one hour"
  // match still stands, and the trailing number + everything after it falls
  // out as residue.
  assert_parse("one hour 5 people", true, KIND_COUNTDOWN, 3600, "5 people");
  // "una hora veinte" (nothing follows -- vacuously trailing_ok) is
  // unaffected, re-asserted here as this guard's own explicit boundary.
  assert_parse("una hora veinte", true, KIND_COUNTDOWN, 4800, "");
  // "para" alone after the trailing number is pure filler -- still binds.
  assert_parse("una hora veinte para", true, KIND_COUNTDOWN, 4800, "");
  // "para el te" -- "para" is filler, but "el"/"te" are real unclaimed
  // words, so the guard fails here too: base "una hora" (3600s) stands, and
  // "veinte el te" (raw accents preserved) falls out as residue.
  assert_parse("una hora veinte para el te", true, KIND_COUNTDOWN, 3600, "veinte el te");
  assert_parse("una hora veinte para el t\xc3\xa9", true, KIND_COUNTDOWN, 3600, "veinte el t\xc3\xa9");
}

// -- Minutes + trailing seconds ----------------------------------------------
// The symmetric analog of test_hours_trailing_minutes, one level down
// the unit hierarchy, closing a v0.1.10 on-device defect: "6 minutos 45"
// was creating a 6:00 timer NAMED "45" instead of a 6:45 timer. "<N>
// <minute-unit> <M>" with M a bare (unit-less) 0-59 number -> N*60 + M*1
// (seconds) -- exactly the same structural rule as the hour case above
// (parse_nl.c's unit+trailing-sub-count block is now ONE rule covering
// both, parameterized on the matched unit's own multiplier), so the same
// shapes, same guards (residue guard, unit-bearing-trailing-number
// exclusion, !magnitude_adjacent) apply unchanged.
void test_minutes_trailing_seconds(void) {
  // The exact motivating case, es + en, word-form + digit-form.
  assert_parse("6 minutos 45", true, KIND_COUNTDOWN, 405, "");
  assert_parse("seis minutos cuarenta y cinco", true, KIND_COUNTDOWN, 405, "");
  assert_parse("six minutes forty five", true, KIND_COUNTDOWN, 405, "");
  assert_parse("6 minutes 45", true, KIND_COUNTDOWN, 405, "");

  // Extra coverage beyond the exact repro: explicit count > 1 other than 6,
  // and the implicit-count-1 idiom (bare "a minute"/"un minuto" with no
  // preceding number, mirroring "an hour twenty"/"hora y veinte" above).
  assert_parse("dos minutos veinte", true, KIND_COUNTDOWN, 140, "");
  assert_parse("two minutes twenty", true, KIND_COUNTDOWN, 140, "");
  assert_parse("a minute twenty", true, KIND_COUNTDOWN, 80, "");
  assert_parse("un minuto y veinte", true, KIND_COUNTDOWN, 80, "");

  // Protections: bare "a minute"/"un minuto" alone (no trailing number) must
  // stay exactly as test_bare_article_plus_unit/test_en_basic/
  // test_es_basic already lock in. "a minute" now resolves to 60 (see
  // test_bare_article_plus_unit's own comment) via its own dedicated
  // fallback.
  assert_parse("a minute", true, KIND_COUNTDOWN, 60, "");
  assert_parse("un minuto", true, KIND_COUNTDOWN, 60, "");
  assert_parse("one minute", true, KIND_COUNTDOWN, 60, "");
  // A trailing number > 59 does not qualify as a bare seconds field; the
  // minute alone still resolves, the extra token is unconsumed -- pure-
  // numeric ("sesenta"), so it is dropped by the residue rule, not
  // surfaced as name.
  assert_parse("un minuto sesenta", true, KIND_COUNTDOWN, 60, "");

  // Residue guard (same mechanism as the hour case): a real unclaimed word
  // after the trailing number means those digits belong to the NAME (mixed
  // residue -- keeps naming), not this rule.
  assert_parse("6 minutos 45 pasta", true, KIND_COUNTDOWN, 360, "45 pasta");

  // Accumulator/pair-rule boundary protections re-asserted for this rule specifically
  // (not just the pre-existing accumulator/pair-rule tests): a genuine two-unit
  // compound ("6 minutes 45 seconds") stays on the pre-existing compound-sum
  // template (D), never double-booked by this rule (`used_compound` guard);
  // a unit-LESS pair ("6 45", no unit anywhere) still belongs to the pair
  // rule above, not this one (this rule's own precondition -- a qualifying
  // unit token -- is simply absent).
  assert_parse("6 minutes 45 seconds", true, KIND_COUNTDOWN, 405, "");
  assert_parse("6 45", true, KIND_COUNTDOWN, 405, "");  // pair rule: 6*60+45
  assert_parse("seis cuarenta y cinco", true, KIND_COUNTDOWN, 405, "");  // pair rule, unchanged
}

// -- Pure-numeric vs mixed residue --------------------------------------------
// A residue consisting ONLY of CLS_NUMBER tokens (word-form or digit) is
// dropped rather than surfaced as `name` -- it is a missed time component,
// not a real name; a name like "45" or "sesenta" is silent-wrong metadata
// (looks deliberate, isn't). A MIXED residue (>=1 real word token alongside
// the numbers) is untouched -- these rows re-assert the already-pinned
// guard case ("5 people" etc.) stays exactly as before, alongside fresh
// pure-numeric rows from elsewhere in this file.
void test_pure_numeric_residue_dropped(void) {
  // Pure-numeric -- dropped (name-less, parse still succeeds).
  assert_parse("una hora sesenta", true, KIND_COUNTDOWN, 3600, "");
  assert_parse("6 minutos 45", true, KIND_COUNTDOWN, 405, "");
  assert_parse("5 5 minutes", true, KIND_COUNTDOWN, 300, "");
  assert_parse("six forty five minutes", true, KIND_COUNTDOWN, 2700, "");

  // Mixed -- keeps naming (the fraction guard case, must stay).
  assert_parse("5 people 5 minutes", true, KIND_COUNTDOWN, 300, "5 people");
  assert_parse("5 rounds 5 minutes", true, KIND_COUNTDOWN, 300, "5 rounds");
  assert_parse("table 5 people 5 minutes", true, KIND_COUNTDOWN, 300, "table 5 people");
  assert_parse("cinco amigos veinte minutos", true, KIND_COUNTDOWN, 1200, "cinco amigos");
  assert_parse("one hour 5 people", true, KIND_COUNTDOWN, 3600, "5 people");
  assert_parse("6 minutos 45 pasta", true, KIND_COUNTDOWN, 360, "45 pasta");
}

// -- Diminutive units ---------------------------------------------------------
// Spanish colloquial diminutive unit synonyms, added as plain UNIT_WORDS
// entries (same multiplier as the base unit) -- no new parsing rule, just
// vocabulary, so these resolve through the ordinary N-U search.
void test_diminutive_units(void) {
  assert_parse("un minutito", true, KIND_COUNTDOWN, 60, "");
  assert_parse("una horita", true, KIND_COUNTDOWN, 3600, "");
  assert_parse("dos minutitos", true, KIND_COUNTDOWN, 120, "");
  assert_parse("un segundito", true, KIND_COUNTDOWN, 1, "");
  // Plural forms, added for symmetry with every other unit table entry.
  assert_parse("dos horitas", true, KIND_COUNTDOWN, 7200, "");
  assert_parse("tres segunditos", true, KIND_COUNTDOWN, 3, "");
}

// -- Approximation markers -----------------------------------------------------
// A dedicated CLS_APPROX class (NOT FILLER_WORDS -- see the clause-scoped
// kind-walk minimality-invariant comment on APPROX_DROP_WORDS in parse_nl.c):
// consumed like a filler so it never leaks into the name residue, while the
// duration itself was already parseable before this change (only the
// residue was dirty).
void test_approximation_markers(void) {
  assert_parse("unos cinco minutos", true, KIND_COUNTDOWN, 300, "");  // marker dropped, not "unos"
  assert_parse("about ten minutes", true, KIND_COUNTDOWN, 600, "");
  assert_parse("like five minutes", true, KIND_COUNTDOWN, 300, "");
  assert_parse("around ten minutes", true, KIND_COUNTDOWN, 600, "");
  assert_parse("como cinco minutos", true, KIND_COUNTDOWN, 300, "");
  assert_parse("unas cinco horas", true, KIND_COUNTDOWN, 18000, "");
}

// -- Regression-lock rows -----------------------------------------------------
// These already work and are already asserted by
// test_en_number_word_coverage/test_es_number_word_coverage above;
// re-asserted here so this feature's full acceptance matrix is represented
// in the corpus in one place.
void test_ninety_minutes_regression(void) {
  assert_parse("noventa minutos", true, KIND_COUNTDOWN, 5400, "");
  assert_parse("ninety minutes", true, KIND_COUNTDOWN, 5400, "");
}

// -- Fractions engine ---------------------------------------------------------
// The three unit-anchored patterns (fraction-before-unit; implicit-N/
// explicit-N trailing-half-after-unit) all ship. The two unit-LESS rows
// ("cinco y medio", "media" alone) are pinned to their FAIL-SOFT outcome
// (see test_unitless_fractional_durations_unsupported above) -- a
// deliberate decision, not the alternate "default-minutes" outcome.
void test_fraction_acceptance_matrix(void) {
  // Pattern 1: <fraction> [connectives] <unit>.
  assert_parse("media hora", true, KIND_COUNTDOWN, 1800, "");
  assert_parse("half an hour", true, KIND_COUNTDOWN, 1800, "");
  assert_parse("un cuarto de hora", true, KIND_COUNTDOWN, 900, "");
  assert_parse("quarter of an hour", true, KIND_COUNTDOWN, 900, "");
  assert_parse("tres cuartos de hora", true, KIND_COUNTDOWN, 2700, "");
  assert_parse("three quarters of an hour", true, KIND_COUNTDOWN, 2700, "");
  assert_parse("three-quarters of an hour", true, KIND_COUNTDOWN, 2700, "");  // hyphen split

  // Patterns 2/3: <unit> y media / <N> <unit> y medio (+ en).
  assert_parse("hora y media", true, KIND_COUNTDOWN, 5400, "");
  assert_parse("an hour and a half", true, KIND_COUNTDOWN, 5400, "");
  assert_parse("a minute and a half", true, KIND_COUNTDOWN, 90, "");
  assert_parse("cinco minutos y medio", true, KIND_COUNTDOWN, 330, "");
  assert_parse("two minutes and a half", true, KIND_COUNTDOWN, 150, "");
}

// -- Fix: "<N> y medio/and a half <unit>" ------------------------------------
// number FIRST, then the fraction connective, then the unit LAST. The old
// code had no NUMBER+HALF combination at all (only NUMBER+QUARTER, and
// only when directly adjacent with no connective) -- a bare HALF word
// matched alone regardless of what preceded it, silently dropping the
// leading number ("cinco y medio minutos" -> 30s instead of 330s). Every
// known repro string, both languages, plus the QUARTER-side generalization
// of the same fix, plus the already-correct orders staying correct --
// proving the class is closed, not just the two named repros.
void test_number_and_half_or_quarter(void) {
  // Exact regression strings -- previously silently wrong (30s/1800s/
  // 900s, discarding the leading number), now correct.
  assert_parse("cinco y medio minutos", true, KIND_COUNTDOWN, 330, "");
  assert_parse("five and a half minutes", true, KIND_COUNTDOWN, 330, "");
  assert_parse("diez y medio minutos", true, KIND_COUNTDOWN, 630, "");
  assert_parse("cinco y medio horas", true, KIND_COUNTDOWN, 19800, "");
  assert_parse("twenty and a half minutes", true, KIND_COUNTDOWN, 1230, "");
  // QUARTER-side generalization (same root cause, same fix): "veinte y
  // cuarto de hora" = 20+1/4 hours = 72900s, not 900s (the leading "veinte"
  // no longer silently discarded).
  assert_parse("veinte y cuarto de hora", true, KIND_COUNTDOWN, 72900, "");

  // No connective at all between the number and a bare HALF word is a
  // DIFFERENT, pre-existing shape this fix generalizes to for symmetry with
  // the QUARTER multiplicative form ("veinte cuartos"=20/4): "twenty
  // media hora" reads as 20 * (1/2 hour) = 10 hours (36000s) -- ambiguous,
  // but the leading number is used, never silently erased into a bare
  // "media hora" (1800s) answer the expected way.
  assert_parse("twenty media hora", true, KIND_COUNTDOWN, 36000, "");

  // Correct-order forms (fraction/unit-anchored before the number, or the
  // number+unit already matched with a trailing half) stay exactly correct
  // -- re-asserted here as this fix's own non-regression boundary, not just
  // trusted from test_fraction_acceptance_matrix above.
  assert_parse("cinco minutos y medio", true, KIND_COUNTDOWN, 330, "");
  assert_parse("media hora", true, KIND_COUNTDOWN, 1800, "");
  assert_parse("un cuarto de hora", true, KIND_COUNTDOWN, 900, "");
  assert_parse("tres cuartos de hora", true, KIND_COUNTDOWN, 2700, "");
}

// -- The "un cuarto" vs "un"=1 collision -- the subtlest fraction
// interaction, worth enumerating explicitly. ------------------------------
void test_un_cuarto_precedence(void) {
  // With a unit present, the fraction reading wins outright -- "un" never
  // resolves to a bare number-1 residue-adjacent match here.
  assert_parse("un cuarto de hora", true, KIND_COUNTDOWN, 900, "");
  assert_parse("una cuarto de hora", true, KIND_COUNTDOWN, 900, "");  // gender-loose, still resolves
  // With NO unit following, the fraction pattern is unit-anchored and
  // declines (per this file's fail-soft-unit-less-fractions choice) --
  // "un"/"cuarto" both fall out as residue, exactly like any other
  // unrecognized two-word utterance. This is NOT the same case as a
  // unit-anchored miss; it simply never reaches a unit token.
  assert_parse("un cuarto", false, KIND_COUNTDOWN, 0, "un cuarto");
  assert_parse("cuarto", false, KIND_COUNTDOWN, 0, "cuarto");
}

// -- Multi-unit 3+ chains -----------------------------------------------------
// The live on-device-class bug this closes: the old compound-sum block
// matched EXACTLY ONE additional [N U] pair, so a 3rd unit was silently
// dropped -- "una hora veinte minutos y treinta segundos" landed at 4800
// (the 30s lost), not 4830. The fix generalizes the 2-pair block into an
// unbounded descending-unit CHAIN (hour(3600) > minute(60) > second(1)),
// tightening the old "different unit" guard into "STRICTLY SMALLER unit"
// in the same change -- every pre-existing pinned compound row was already
// descending, so this loses nothing pinned while fixing both the 3rd-unit
// drop AND a previously-unguarded ascending-reorder bug (see the "5 minutes
// 3 hours" row below).
void test_multiunit_chains(void) {
  // The exact headline repro, both languages, plus a mixed digit/es form.
  assert_parse("1 hour 20 minutes 30 seconds", true, KIND_COUNTDOWN, 4830, "");
  assert_parse("una hora veinte minutos y treinta segundos", true, KIND_COUNTDOWN, 4830, "");
  assert_parse("1 hora 20 minutos 30 segundos", true, KIND_COUNTDOWN, 4830, "");

  // 2-chain regressions (test_compound_duration_sum_en_es's own corpus,
  // re-asserted here as this generalization's explicit non-regression
  // boundary) -- unchanged.
  assert_parse("one hour thirty minutes", true, KIND_COUNTDOWN, 5400, "");
  assert_parse("1 minute 30 seconds", true, KIND_COUNTDOWN, 90, "");
  assert_parse("two hours thirty minutes", true, KIND_COUNTDOWN, 9000, "");

  // Accumulator regression: a repeated SAME unit is a restatement, not a compound --
  // 60 is not strictly < 60, so the chain stops at the first pair; the
  // leftover second "5" is pure-numeric residue, dropped per the
  // already-shipped residue rule (not a name).
  assert_parse("5 minutes 5 minutes", true, KIND_COUNTDOWN, 300, "");

  // Additional protection: the OLD 2-pair block's guard was "different
  // unit" (mult2 != mult), which let an ASCENDING reorder like "5 minutes 3
  // hours" silently sum to 11100s -- the strictly-descending tightening now
  // correctly rejects it (3600 is not < 60): the chain stops after "5
  // minutes" (300s), leaving "3" as dropped
  // pure-numeric residue (same drop mechanism as the accumulator row above).
  assert_parse("5 minutes 3 hours", true, KIND_COUNTDOWN, 300, "");

  // A 3rd pair that is itself non-descending relative to the 2nd stops the
  // chain at 2 pairs, exactly the same mechanism -- the trailing "5 minutes"
  // is left as (dropped) residue, not silently added a 3rd time.
  assert_parse("1 minute 30 seconds 5 minutes", true, KIND_COUNTDOWN, 90, "");

  // The clause-scoped kind walk treats a chain's UNIT tokens as
  // transparent glue exactly like a 2-pair match -- REPEAT still resolves
  // across the whole 3-unit chain.
  assert_parse("repeat 1 hour 20 minutes 30 seconds", true, KIND_REPEAT, 4830, "");

  // Range guard: sums in int64_t across the whole chain, same
  // TIMER_MAX_DURATION_S boundary as every other rule -- 99:59:59 is the
  // exact ceiling (359999s); one second over any component fails soft.
  assert_parse("99 hours 59 minutes 59 seconds", true, KIND_COUNTDOWN, 359999, "");
  assert_parse("99 hours 59 minutes 60 seconds", false, KIND_COUNTDOWN, 0, "");

  // Boundary re-asserts: the base Template D 2-pair compound, the unit-less
  // spoken-pair rule (no UNIT token anywhere), and its es equivalent are all
  // untouched by this generalization (disjoint by the unit-presence test).
  assert_parse("6 minutes 45 seconds", true, KIND_COUNTDOWN, 405, "");
  assert_parse("6 45", true, KIND_COUNTDOWN, 405, "");
  assert_parse("seis cuarenta y cinco", true, KIND_COUNTDOWN, 405, "");
}

// -- "in"/"within" as fillers, en only --------------------------------------
// Stops "in 5 minutes"/"within ten minutes" (very common EN timer phrasing)
// from leaking "in"/"within" into the name residue. Spanish "en"
// deliberately NOT added ("en" is far more often real name content than
// duration glue) -- re-asserted here as this rule's explicit non-goal, not
// an oversight.
void test_in_within_fillers(void) {
  assert_parse("in 5 minutes", true, KIND_COUNTDOWN, 300, "");
  assert_parse("within ten minutes", true, KIND_COUNTDOWN, 600, "");

  // The clause-scoped kind walk crosses the new filler exactly like any
  // other -- REPEAT resolves whether "repeat" leads or the kind keyword
  // trails past "in".
  assert_parse("repeat in 5 minutes", true, KIND_REPEAT, 300, "");
  assert_parse("in ten minute interval", true, KIND_REPEAT, 600, "");

  // Spanish "en" stays CLS_WORD (unclaimed, real residue) -- unaffected,
  // proving the EN-only scope is exact, not a partial/leaky fold.
  assert_parse("en cinco minutos", true, KIND_COUNTDOWN, 300, "en");
}

// -- "couple" = 2, en only ---------------------------------------------------
// "par"/"pair" (es/en) and "dozen" deliberately NOT added, per a
// collision-risk/frequency call.
void test_couple_equals_two(void) {
  assert_parse("a couple of minutes", true, KIND_COUNTDOWN, 120, "");
  assert_parse("couple minutes", true, KIND_COUNTDOWN, 120, "");
  assert_parse("a couple minutes", true, KIND_COUNTDOWN, 120, "");

  // The "of"-skip is narrowly scoped to the literal word "couple" (see the
  // comment at its call site in parse_nl.c) specifically so it cannot
  // reopen the "once" collision guard: es "once"=11 immediately followed
  // by a filler then a unit must keep failing, since that adjacency break
  // is the ONLY thing distinguishing the Spanish number from the English
  // frequency idiom "once a ___". Re-asserted here as this rule's own
  // explicit boundary, not just trusted from test_once_en_es_collision.
  assert_parse("once a minute", false, KIND_COUNTDOWN, 0, "once");
  assert_parse("once minutes", true, KIND_COUNTDOWN, 660, "");
}

// -- Decimal quantity, RE-FIXED ----------------------------------------------
// Supersedes an earlier split-into-two-tokens guard this test used to
// document -- the old fix split a digit-flanked '.'/',' into a token
// boundary (like a hyphen), so the duration search bound to whichever
// digit group sat against the unit and silently dropped the other as
// pure-numeric residue ("5.5 minutes" -> 300s, not 330s -- visibly
// wrong-ish, never silently wrong, but still imperfect). This was replaced
// with a REAL decimal quantity (CLS_DECIMAL, parse_decimal) routed through
// the same unit-anchored, exactness-guarded fraction arithmetic as this
// file's other fraction rules -- "5.5 minutes" is now exactly 5.5*60=330s,
// not a same-value coincidence.
void test_decimal_failsoft_guard(void) {
  assert_parse("5.5 minutes", true, KIND_COUNTDOWN, 330, "");  // corrected from 300 (split path)
  assert_parse("5,5 minutos", true, KIND_COUNTDOWN, 330, "");     // es decimal comma, same re-spec
  assert_parse("12.5 minutes", true, KIND_COUNTDOWN, 750, "");    // multi-digit both sides of the point

  // The old test's own "1.5 hours" row was ALREADY a documented numeric
  // slip (the split path actually produced 18000s, not the illustrative
  // 3600s) -- this fixes it for real: 1.5*3600=5400s exactly.
  assert_parse("1.5 hours", true, KIND_COUNTDOWN, 5400, "");      // was 18000s (split path)/54000s (pre-fix)

  // A '.'/',' NOT flanked by digits on both sides is unaffected -- still
  // falls through to the pre-existing edge-punct trim, unchanged.
  assert_parse("five minutes.", true, KIND_COUNTDOWN, 300, "");
  assert_parse("1 hour, 20 minutes, and 30 seconds", true, KIND_COUNTDOWN, 4830, "");
}

// -- Tokenizer/ITN -- colon-time duration routing -----------------------------
// The motivating datum ("6:45" -> "Didn't catch a time") and the
// silent-wrong class ("6:45 minutes" used to read as 38700s). A
// digit-flanked ':' now survives nl_normalize whole (CLS_COLONTIME),
// routed into a dedicated duration block that mirrors the existing
// fraction-engine shape (unit-anchored, range-guarded, fail-soft on
// anything out of range). Clock-context routing of a colon token ("at
// 6:45", "a las 18:30") is explicitly OUT of scope here -- covered
// separately below -- so every row below is a bare/duration reading only.
void test_colon_time_duration_routing(void) {
  // Bare pair, no unit, no clock context -> M:SS default (the
  // shipped "seis cuarenta y cinco"/"six forty five"->405 spoken-pair
  // precedent, now also true of the colon ITN rendering of the same
  // utterance).
  assert_parse("6:45", true, KIND_COUNTDOWN, 405, "");
  assert_parse("90:00", true, KIND_COUNTDOWN, 5400, "");

  // Triple -> H:M:S. The one genuinely new shape (no existing engine
  // covers an H:M:S triple).
  assert_parse("1:20:30", true, KIND_COUNTDOWN, 4830, "");

  // Pair + a trailing duration unit -> unit-scaled (resolves the same
  // silent-wrong class above: never 38700, never 2700).
  assert_parse("6:45 minutes", true, KIND_COUNTDOWN, 405, "");
  assert_parse("6:45 hours", true, KIND_COUNTDOWN, 24300, "");

  // A "seconds"-labelled colon pair has no sane major:minor reading
  // -- fail-soft, never guess. The unit itself is still consumed (a
  // CLS_UNIT token is always pre-marked consumed), so only the colon token
  // itself is left as residue.
  assert_parse("6:45 seconds", false, KIND_COUNTDOWN, 0, "6:45");

  // A second group over 59 ("6:75") fails soft, like "diez sesenta"
  // group-2>59 guard elsewhere in this grammar.
  assert_parse("6:75", false, KIND_COUNTDOWN, 0, "6:75");

  // Kind resolution reaches a CLS_COLONTIME match exactly like any
  // other duration anchor -- the clause-scoped kind walk doesn't special-
  // case the anchor's token class.
  assert_parse("repeat 6:45", true, KIND_REPEAT, 405, "");
}

// -- Slash-fraction duration routing ------------------------------------------
// Resolves a silent-wrong class ("1/2 hour" used to silently read as
// 43200s/12h -- the whole N/D family via the identical
// dropped-then-concatenated path as the colon case above).
void test_slash_fraction_duration_routing(void) {
  assert_parse("1/2 hour", true, KIND_COUNTDOWN, 1800, "");
  assert_parse("1/2 hora", true, KIND_COUNTDOWN, 1800, "");  // es
  assert_parse("3/4 hour", true, KIND_COUNTDOWN, 2700, "");
  assert_parse("1/4 hour", true, KIND_COUNTDOWN, 900, "");

  // Unsupported denominator -- fail-soft, never guess (this app's fraction
  // vocab only covers halves/quarters).
  assert_parse("1/3 hour", false, KIND_COUNTDOWN, 0, "1/3");

  // No unit at all -- fail-soft (unit genuinely unknown, same reasoning as
  // every unit-less word-form fraction in this grammar).
  assert_parse("1/2", false, KIND_COUNTDOWN, 0, "1/2");
}

// -- Decimal quantity -- see test_decimal_failsoft_guard
// (above) for the primary "5.5 minutes"/"1.5 hours"/"12.5 minutes" rows;
// this covers the remaining decimal sub-matrix rows.
void test_decimal_duration_routing(void) {
  assert_parse("2.5 hours", true, KIND_COUNTDOWN, 9000, "");
  assert_parse("10.5 minutes", true, KIND_COUNTDOWN, 630, "");

  // A bare decimal with no unit is now fail-soft -- a deliberate,
  // documented behavior change from the old split path's coincidental 405s
  // (that path no longer exists; see is_digit_flanked_symbol's own doc
  // comment in parse_nl.c). The word form "six forty five"->405 and the
  // colon form "6:45"->405 both still give the same value another way.
  assert_parse("6.45", false, KIND_COUNTDOWN, 0, "6.45");
}

// -- Hundred-class composition -- bounded: hundred-class ONLY
// ("hundred"/"cien"/"ciento"/"cientos"), unit-anchored, result <=
// TIMER_MAX_DURATION_S.
//
// The positive COMPOSER (match_hundred_composite) was REMOVED for size in
// v0.1.13 (1,063 B ARM cost for 10 rarely-used cases, the worst size-per-case
// tradeoff on the cull ladder -- a deliberate capability trade for permanent
// size headroom). Every row below that used to compose a real value is re-pinned
// to REFUSE instead -- this function is now a REGRESSION LOCK on the
// fail-soft contract: the `hundred_present` GUARD (kept, parse_nl.c) must
// keep forcing every hundred-word utterance to decline rather than let some
// unrelated number+unit pair silently "rescue" it into a wrong value (the
// one outcome this design forbids). Rows that were ALREADY false before the
// cull (the range-guard/stray-number declines) are unaffected and stay
// exactly as pinned.
// Thousand/million/billion (plus es equivalents) use a wholly separate,
// still-live guard, re-asserted at the end of this function, plus
// test_magnitude_guard_complete_lexicon's/test_magnitude_
// protections_still_fail_soft's own rows above.
void test_hundred_composition(void) {
  // The former full acceptance matrix -- every row here used to compose a
  // real value; all now REFUSE, "hundred"/"cien"/etc. left as visible,
  // unconsumed residue (never silently dropped, never a wrong value).
  assert_parse("ciento veinte segundos", false, KIND_COUNTDOWN, 0, "ciento veinte");
  assert_parse("one hundred twenty seconds", false, KIND_COUNTDOWN, 0, "one hundred twenty");
  assert_parse("two hundred seconds", false, KIND_COUNTDOWN, 0, "two hundred");
  // "a" is FILLER (always consumed regardless of use) and "and" is a
  // JOINER (same); "hundred" itself is CLS_WORD and is the only survivor.
  assert_parse("a hundred and twenty seconds", false, KIND_COUNTDOWN, 0, "hundred twenty");
  assert_parse("one hundred twenty minutes", false, KIND_COUNTDOWN, 0, "one hundred twenty");
  // Range safety: 500*3600s is wildly over TIMER_MAX_DURATION_S -- the
  // (now-removed) composer already declined this before the cull too (not
  // a "match then range-reject" the way a plain N-U pair works), so BOTH
  // tokens stayed fully unclaimed, VISIBLE residue, unaffected by the cull
  // -- this row's expectation is UNCHANGED.
  assert_parse("five hundred hours", false, KIND_COUNTDOWN, 0, "five hundred");

  // Bare "cien"/"ciento" (H defaults to 1, no leading number token) --
  // "cien horas" is a pre-existing pinned row (100h = 360000s, one second
  // over TIMER_MAX_DURATION_S), ALREADY false before the cull for the same
  // range reason as "five hundred hours" above -- UNCHANGED by the cull.
  assert_parse("cien horas", false, KIND_COUNTDOWN, 0, "cien");
  // culled-v0.1.13: was `true, KIND_COUNTDOWN, 6000, ""` (100 min, well in
  // range) pre-cull.
  assert_parse("cien minutos", false, KIND_COUNTDOWN, 0, "cien");

  // The plural "cientos" form (included even though "dos cientos" isn't
  // idiomatic spoken Spanish -- "doscientos" is the real single-word form;
  // "doscientos" as one token was never in HUNDRED_WORDS_BLOB and stays its
  // own pre-existing GAP, unaffected here).
  // culled-v0.1.13: was `true, KIND_COUNTDOWN, 200, ""` pre-cull.
  assert_parse("dos cientos segundos", false, KIND_COUNTDOWN, 0, "dos cientos");

  // "Sole number content" precondition: an ordinary name word is NOT a
  // hundred word and must keep parsing normally, completely unaffected by
  // hundred_present's own presence check elsewhere in this
  // corpus -- "pasta veinte minutos" itself is already covered by
  // test_magnitude_guard_complete_lexicon; this adds the SAME shape
  // proximate to a genuine hundred word in a separate utterance, to confirm
  // the two never cross-contaminate (the hundred word's own decline does
  // not disturb "pasta" staying legible residue).
  // culled-v0.1.13: was `true, KIND_COUNTDOWN, 6000, "pasta"` pre-cull.
  assert_parse("pasta cien minutos", false, KIND_COUNTDOWN, 0, "pasta cien");

  // Re-asserted: thousand/million/billion are unaffected by the hundred
  // composer's removal, still fully guarded by the separate magnitude guard
  // covering that vocabulary.
  assert_parse("mil veinte minutos", false, KIND_COUNTDOWN, 0, "mil veinte");
  assert_parse("one thousand twenty minutes", false, KIND_COUNTDOWN, 0, "one thousand twenty");

  // The hundred-composite trailing sub-count parity ("cien minutos
  // 30"->6030s) no longer exists -- the whole composer, extension
  // included, is gone. Both tokens now stay fully unclaimed, visible
  // residue.
  // culled-v0.1.13: was `true, KIND_COUNTDOWN, 6030, ""` pre-cull.
  assert_parse("cien minutos 30", false, KIND_COUNTDOWN, 0, "cien 30");
  // culled-v0.1.13: was `true, KIND_COUNTDOWN, 6030, ""` pre-cull.
  assert_parse("one hundred minutes 30", false, KIND_COUNTDOWN, 0, "one hundred 30");
  // The extension only ever applied to minute/hour-class units -- a seconds
  // unit never had a "trailing sub-count" reading even pre-cull ("30" was
  // already a genuinely unrelated stray number next to the composite), so
  // this row's expectation is UNCHANGED by the cull.
  assert_parse("cien segundos 30", false, KIND_COUNTDOWN, 0, "cien 30");
  // Same reasoning as "cien horas" above (range-guard decline, pre-existing,
  // UNCHANGED by the cull) -- "cien horas 30" would be 361800s either way.
  assert_parse("cien horas 30", false, KIND_COUNTDOWN, 0, "cien 30");

  // The hundred-composite trailing-half parity ("cien minutos y
  // medio"->6030s) is also gone with the composer -- both the unit and the
  // half word now stay unclaimed, visible residue.
  // culled-v0.1.13: was `true, KIND_COUNTDOWN, 6030, ""` pre-cull.
  assert_parse("cien minutos y medio", false, KIND_COUNTDOWN, 0, "cien medio");
  // culled-v0.1.13: was `true, KIND_COUNTDOWN, 6030, ""` pre-cull.
  assert_parse("cien minutos y media", false, KIND_COUNTDOWN, 0, "cien media");
  // A seconds unit never composed a half even before the cull (mult must
  // be even); before the cull this row still composed the BASE 100s via
  // the plain composer (with "medio" left as unclaimed residue) -- with
  // the composer gone, the base no longer composes either, so "cien" joins
  // "medio" as residue.
  // culled-v0.1.13: was `true, KIND_COUNTDOWN, 100, "medio"` pre-cull.
  assert_parse("cien segundos y medio", false, KIND_COUNTDOWN, 0, "cien medio");
  // culled-v0.1.13: was `true, KIND_COUNTDOWN, 6030, "pasta"` pre-cull.
  assert_parse("cien minutos y medio pasta", false, KIND_COUNTDOWN, 0, "cien medio pasta");

  // The composer's "a couple OF hundred X" filler-skip extension is gone
  // with the rest of the composer -- both forms now REFUSE,
  // "couple"/"hundred" left as visible residue ("of"/"a" are FILLER,
  // always consumed regardless of use).
  // culled-v0.1.13: was `true, KIND_COUNTDOWN, 12000, ""` pre-cull.
  assert_parse("a couple of hundred minutes", false, KIND_COUNTDOWN, 0, "couple hundred");
  // culled-v0.1.13: was `true, KIND_COUNTDOWN, 12000, ""` pre-cull.
  assert_parse("couple hundred minutes", false, KIND_COUNTDOWN, 0, "couple hundred");
}

// v0.1.13 hundred-composer removal: adversarial regression lock. Extra
// hundred-adjacent shapes beyond the corpus's own 10 flipped rows (already
// locked in test_hundred_composition/test_magnitude_
// guard_complete_lexicon above) -- bare hundred words with no unit at all,
// the "y"-joiner variant, the plural-only form, and hundred combined with a
// create-cue/kind word -- verified against the real compiled parser both
// with and without match_hundred_composite present, confirming every one
// REFUSES (never a silently wrong duration) with the hundred word(s) left
// as visible, unconsumed residue.
void test_hundred_cull_adversarial_lock(void) {
  // Bare hundred words, no unit anchor at all -- refused even PRE-cull (the
  // composer itself required a unit right after H/R), so these rows are
  // unaffected by the cull; pinned here anyway as an explicit "still never
  // silent-wrong" lock for the guard alone.
  assert_parse("hundred", false, KIND_COUNTDOWN, 0, "hundred");
  assert_parse("cien", false, KIND_COUNTDOWN, 0, "cien");
  assert_parse("ciento", false, KIND_COUNTDOWN, 0, "ciento");
  assert_parse("cientos", false, KIND_COUNTDOWN, 0, "cientos");
  assert_parse("hundred and twenty", false, KIND_COUNTDOWN, 0, "hundred twenty");

  // culled-v0.1.13: "y"-joiner variant, was `true, KIND_COUNTDOWN, 7200, ""`
  // pre-cull (H defaults to 1 -- "cien y veinte minutos" -- composed same
  // as "ciento veinte minutos").
  assert_parse("cien y veinte minutos", false, KIND_COUNTDOWN, 0, "cien veinte");

  // culled-v0.1.13: the plural-only "cientos" combining with a leading
  // number, was `true, KIND_COUNTDOWN, 54000, ""` pre-cull (900 min).
  assert_parse("nueve cientos minutos", false, KIND_COUNTDOWN, 0, "nueve cientos");

  // culled-v0.1.13: "and"-joined trailing count, was
  // `true, KIND_COUNTDOWN, 15000, ""` pre-cull (250 min).
  assert_parse("two hundred and fifty minutes", false, KIND_COUNTDOWN, 0,
               "two hundred fifty");

  // culled-v0.1.13: create cue "pon" ahead of a hundred phrase, was
  // `true, KIND_COUNTDOWN, 6000, ""` pre-cull -- "pon" is a CREATE_CUE_WORDS
  // token, consumed regardless of use (same convention as FILLER), so it
  // never leaks into the residue either.
  assert_parse("pon cien minutos", false, KIND_COUNTDOWN, 0, "cien");

  // culled-v0.1.13: a KIND_COUNTDOWN keyword ahead of a hundred phrase (en
  // "timer"/es "temporizador"/"alarma"), was `true, KIND_COUNTDOWN, 6000,
  // ""` (es) pre-cull -- KIND tokens are consumed regardless of use, same
  // "never leaks into residue" convention as FILLER/UNIT.
  assert_parse("temporizador cien minutos", false, KIND_COUNTDOWN, 0, "cien");
  assert_parse("alarma de cien minutos", false, KIND_COUNTDOWN, 0, "cien");

  // A digit-form "100" is NOT a HUNDRED_WORDS token (that vocab is word-form
  // only) -- unaffected by the cull, unchanged pre/post: "doscientos" (also
  // not in HUNDRED_WORDS_BLOB, a pre-existing GAP) is picked up by the
  // ordinary main N-U search matching "veinte minutos" alone, leaving
  // "doscientos" as ordinary unrelated residue -- confirms the cull did not
  // touch the unrelated main-search path.
  assert_parse("doscientos veinte minutos", true, KIND_COUNTDOWN, 1200, "doscientos");

  // NOTE (out of scope for this cull, not pinned as a clock assertion here
  // since assert_clock_target/assert_clock_target_name are
  // declared further down this file): "at two hundred hours"/"alarm at 2
  // hundred" resolve via the CLOCK discriminator (match_clock_target, a
  // wholly separate function from the removed composer, checked FIRST and
  // unconditional of hundred_present) as an ordinary ambiguous-meridiem "at
  // two"/"at 2" clock hour, leaving "hundred" as unconsumed residue ("hours"
  // is CLS_UNIT, always consumed regardless of use) -- verified IDENTICAL
  // before and after this cull against both binaries (pre/post ablation),
  // so this is pre-existing clock-matcher behavior, not something this cull
  // introduces or could introduce (ct_matched is resolved before
  // hundred_present is even read). See test_ct_clock_target_discriminator's
  // own family below for the general clock-vs-residue pattern this follows.
}

// -- CT: absolute clock targets -- the discriminator. Asserts NlParse.clock
// directly (has_duration stays false; the caller resolves via
// nl_clock_target_to_duration, tested separately below) -- ok=true is the
// recognized-but-unresolved contract (parse_nl.h's own doc comment on
// NlParse.ok).
static void assert_clock_target(const char *transcript, bool exp_recognized, int exp_hour,
                                 int exp_minute, bool exp_hour24_known) {
  NlParse r;
  parse_nl(transcript, &r);

  char msg[96];
  snprintf(msg, sizeof msg, "transcript: \"%s\"", transcript);

  TEST_ASSERT_EQUAL_INT_MESSAGE(exp_recognized, r.clock.has_clock_target, msg);
  if (!exp_recognized) return;
  TEST_ASSERT_EQUAL_INT_MESSAGE(exp_hour, r.clock.hour, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(exp_minute, r.clock.minute, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(exp_hour24_known, r.clock.hour24_known, msg);
  TEST_ASSERT_TRUE_MESSAGE(r.ok, msg);
  TEST_ASSERT_FALSE_MESSAGE(r.has_duration, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(KIND_COUNTDOWN, r.kind, msg);
}

void test_clock_target_discriminator(void) {
  // EN prepositions.
  assert_clock_target("at six", true, 6, 0, false);
  assert_clock_target("at six thirty", true, 6, 30, false);
  assert_clock_target("at six forty five", true, 6, 45, false);
  assert_clock_target("until 6", true, 6, 0, false);
  assert_clock_target("until six pm", true, 18, 0, true);
  assert_clock_target("wake me at six thirty", true, 6, 30, false);  // residue unaffected

  // ES: "a las"/"a la" + "hasta las"/"hasta la".
  assert_clock_target("a las seis", true, 6, 0, false);
  assert_clock_target("a las seis y media", true, 6, 30, false);
  assert_clock_target("a las seis cuarenta y cinco", true, 6, 45, false);
  assert_clock_target("a la una", true, 1, 0, false);
  assert_clock_target("hasta las seis y media", true, 6, 30, false);
  assert_clock_target("las dieciocho", true, 18, 0, true);  // 24h word -- unambiguous, no "a" needed

  // am/pm / 24h resolution.
  // A documented widening of the preposition-only discriminator: a bare
  // trailing meridiem is now ITSELF enough clock-context, no preposition
  // needed -- "six pm" flips from fail-soft/bare-duration-path to a real
  // 18:00 clock target. Re-specced from `false` (its earlier behavior).
  assert_clock_target("six pm", true, 18, 0, true);
  assert_clock_target("at six am", true, 6, 0, true);
  assert_clock_target("at twelve pm", true, 12, 0, true);   // noon
  assert_clock_target("at twelve am", true, 0, 0, true);    // midnight
  assert_clock_target("a las seis de la tarde", true, 18, 0, true);
  assert_clock_target("a las seis de la manana", true, 6, 0, true);

  // The pinned discriminator contrast: the SAME digits, with vs without
  // the preposition/article, must resolve completely differently --
  // duration vs clock target, never both.
  assert_clock_target("seis cuarenta y cinco", false, 0, 0, false);  // stays the shipped 405s DURATION
  assert_parse("seis cuarenta y cinco", true, KIND_COUNTDOWN, 405, "");
  assert_clock_target("seis y media", false, 0, 0, false);  // stays unit-less-fraction fail-soft
  // "y" is a JOINER, unconditionally consumed/dropped from residue like
  // everywhere else in this file (not this rule's own doing) -- residue is
  // "seis media", not the full raw string.
  assert_parse("seis y media", false, KIND_COUNTDOWN, 0, "seis media");

  // Bare "hasta"/"a" with no las/la never trigger (the grammar's own
  // literal scope -- both fall through to the ordinary duration pipeline and
  // fail soft there, same as any unrecognized word).
  assert_clock_target("hasta seis", false, 0, 0, false);

  // "la"/"a" elsewhere in an utterance, NOT followed by a valid clock
  // expression, must never misfire -- the discriminator requires the VERY
  // NEXT token to be a 0-23 number.
  assert_clock_target("la alarma de cinco minutos", false, 0, 0, false);
  assert_parse("la alarma de cinco minutos", true, KIND_COUNTDOWN, 300, "la");

  // A clock target is COUNTDOWN-only, unconditionally -- a co-occurring
  // REPEAT keyword is silently ignored (forced to COUNTDOWN), never
  // guessed at or failed soft.
  assert_clock_target("repeat at six", true, 6, 0, false);
  {
    NlParse r;
    parse_nl("repeat at six", &r);
    TEST_ASSERT_EQUAL_INT(KIND_COUNTDOWN, r.kind);
  }

  // "H and a half"/"H and a quarter" -- the natural EN idiom, JOINER "and"
  // then FILLER "a" then the HALF/QUARTER word -- previously only skipped
  // a single optional JOINER, never the FILLER, so the FILLER "a" blocked
  // the match entirely and "at six and a half" silently resolved to 6:00
  // (with "half" leaking into residue as the sole hint) instead of 6:30.
  // Fixed to skip a full run of FILLER|JOINER tokens, mirroring
  // match_number_fraction_at's own already-correct loop for the identical
  // shape on the duration grammar's fraction engine.
  assert_clock_target("at six and a half", true, 6, 30, false);
  assert_clock_target("at six and a quarter", true, 6, 15, false);
  assert_clock_target("until six and a half", true, 6, 30, false);
  // No article ("and" alone, no "a") -- already worked before this fix
  // (proves the FILLER "a" was the only thing breaking it); re-pinned here
  // as a regression guard now that the skip is a loop, not a single check.
  assert_clock_target("at six and half", true, 6, 30, false);
  // ES "a las seis y media" (pinned above, unaffected) has no equivalent
  // article between "y" and "media" -- was never broken by this bug, and
  // stays correct here (no new assertion needed, already re-asserted).
}

// Asserts a CT parse's residue-derived `name`/`has_name` only -- some
// pinned rows need `r.name`, which assert_clock_target deliberately does
// not check (see its own comment: hour/minute/kind are its job, name is
// this helper's). Callers pair this with assert_clock_target when a row
// also needs to pin the hour/minute.
static void assert_clock_target_name(const char *transcript, const char *exp_name) {
  NlParse r;
  parse_nl(transcript, &r);

  char msg[96];
  snprintf(msg, sizeof msg, "transcript: \"%s\"", transcript);

  TEST_ASSERT_TRUE_MESSAGE(r.clock.has_clock_target, msg);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(exp_name, r.name, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(strlen(exp_name) > 0, r.has_name, msg);
}

// -- Colon-clock routing -- the 14-row flip. These 14 rows were pinned as
// GUARDED fail-soft (the former clock-context colon guard, now
// superseded); the guard's own discriminator word-sets
// (CLOCK_PREP_WORDS_EN/CLOCK_ART_WORDS_ES/meridiem markers) become the CT
// ROUTER instead, per match_clock_expr's own new CLS_COLONTIME branch.
// Every row here flips from `ok=false` to a real recognized clock target --
// has_duration stays false (a clock target is COUNTDOWN-only-once-resolved,
// per NlParse.ok's own doc comment), asserted via assert_clock_target
// (hour/minute/hour24_known), not assert_parse (which can't express a
// not-yet-resolved CT row -- see its own internal
// `exp_ok == has_duration || STOPWATCH` check).
void test_colon_clock_routing_flip(void) {
  // Preceding EN preposition.
  assert_clock_target("at 6:45", true, 6, 45, false);
  assert_clock_target("until 18:30", true, 18, 30, true);
  assert_clock_target("at 6:30", true, 6, 30, false);

  // Preceding ES article ("a las"/bare "las").
  assert_clock_target("a las 6:45", true, 6, 45, false);
  assert_clock_target("a las 18:30", true, 18, 30, true);
  assert_clock_target("a las 9:30", true, 9, 30, false);
  assert_clock_target("a las 20:00", true, 20, 0, true);

  // "hasta" (bare, directly before a COLON token specifically -- narrowly
  // scoped so this does NOT reopen the "hasta seis"->false regression lock
  // in test_ct_clock_target_discriminator, which must stay false for a
  // plain NUMBER; a colon-shaped continuation is unambiguous enough to
  // trigger on its own).
  assert_clock_target("hasta 18:30", true, 18, 30, true);

  // Following meridiem marker, no preposition at all ("6:45 pm"-shaped) --
  // The bare-meridiem discriminator.
  assert_clock_target("6:45 pm", true, 18, 45, true);
  assert_clock_target("6:45 am", true, 6, 45, true);
  assert_clock_target("at 6:45 pm", true, 18, 45, true);
  assert_clock_target("at 10:45 am", true, 10, 45, true);
  assert_clock_target("at 12:30 pm", true, 12, 30, true);

  // es meridiem via "de la tarde", through the "de"/"la" skip.
  assert_clock_target("6:45 de la tarde", true, 18, 45, true);
  assert_clock_target("Alarma a las 6:45.", true, 6, 45, false);

  // Names: prepositions/articles/meridiem markers are all consumed as part
  // of the CT span; "Alarma" is a pre-consumed CLS_KIND_COUNTDOWN token --
  // every row above resolves with an EMPTY name.
  assert_clock_target_name("at 6:45", "");
  assert_clock_target_name("a las 18:30", "");
  assert_clock_target_name("hasta 18:30", "");
  assert_clock_target_name("6:45 pm", "");
  assert_clock_target_name("6:45 de la tarde", "");
  {
    NlParse r;
    parse_nl("Alarma a las 6:45.", &r);
    TEST_ASSERT_TRUE(r.clock.has_clock_target);
    TEST_ASSERT_FALSE(r.has_name);
  }

  // Non-clock-context colon rows are UNAFFECTED -- re-asserted here as this
  // routing's own explicit boundary (a bare "6:45"/"repeat 6:45" still has
  // no clock-context signal at all, so it stays the duration reading).
  assert_parse("6:45", true, KIND_COUNTDOWN, 405, "");
  assert_parse("repeat 6:45", true, KIND_REPEAT, 405, "");

  // The one existing regression lock this flip must not touch: bare
  // "hasta seis" (a PLAIN number, not colon-shaped) still never triggers --
  // re-asserted here since it sits right next to the new "hasta"+colon
  // branch in match_clock_target's own loop.
  assert_clock_target("hasta seis", false, 0, 0, false);

  // Invalid-range colon-in-clock-context input still declines to fail-soft
  // (never guesses) -- colon_needs_clock_context_guard remains the
  // defense-in-depth safety net for exactly this residual case.
  assert_parse("at 6:75", false, KIND_COUNTDOWN, 0, "at 6:75");
}

// -- The pure-numeric-residue-drop rule is rescoped from `has_duration`-only
// to also cover a recognized clock target (`out->clock.has_clock_target`) --
// a CT parse is ALSO a VOICE_OK parse (`ok`=true) whose name/has_name is
// read AND persisted (`do_start()`'s `store_set_name`) by screen_voice.c on
// the exact same path as any other successful parse, even though
// has_duration itself stays false for CT by design. Before this fix,
// leftover unclaimed digits from a CT utterance got persisted verbatim as
// the created timer's name if the user pressed Start without editing --
// reproducing, for the newest feature, the identical defect class the
// original residue-drop rule exists to close for everything else.
void test_clock_target_pure_numeric_residue_drop(void) {
  // All-numeric residue: dropped, exactly like the has_duration case --
  // each of these previously surfaced its leftover digits verbatim as name.
  assert_clock_target("5 a las seis", true, 6, 0, false);
  assert_clock_target_name("5 a las seis", "");
  assert_clock_target("100 a las seis", true, 6, 0, false);
  assert_clock_target_name("100 a las seis", "");
  assert_clock_target("sesenta a las seis", true, 6, 0, false);
  assert_clock_target_name("sesenta a las seis", "");
  assert_clock_target("6 minutos 45 a las seis", true, 6, 0, false);
  assert_clock_target_name("6 minutos 45 a las seis", "");
  assert_clock_target("diez sesenta a las seis", true, 6, 0, false);
  assert_clock_target_name("diez sesenta a las seis", "");

  // The compound case worth calling out explicitly: "at six and a
  // half" -- the discriminator fix (above) resolves the
  // correct 6:30 target, and by the time it does, the WHOLE token span
  // (through "half") is already consumed by match_clock_expr itself, so no
  // residue is left for the ambiguity gate to even need to drop -- pinned here
  // for completeness (empty name), not because this rescoping does
  // anything extra on this specific utterance.
  assert_clock_target_name("at six and a half", "");

  // Mixed residue (>=1 real CLS_WORD token alongside the numbers) is
  // correctly NOT dropped -- unaffected by this rescoping, same "5 people"/
  // "mil veinte minutos"-style behavior as everywhere else in this file
  // (this gate only ever fires when EVERY unconsumed token is CLS_NUMBER).
  // "mil" is a MAGNITUDE_WORDS token -- deliberately never CLS_NUMBER (see
  // that blob's own doc comment) -- so "mil veinte" stays a MIXED residue
  // and is correctly preserved as name text, clock target or not.
  assert_clock_target("mil veinte a las seis", true, 6, 0, false);
  assert_clock_target_name("mil veinte a las seis", "mil veinte");

  // NOTE ("five and a half at six"): an earlier repro description assumed
  // this transcript resolves to "6:30 AND name-less" (the same outcome as
  // "at six and a half", above) -- independently re-verified against the
  // real compiled parser, this is NOT what this exact string does, before
  // or after either fix. The preposition "at" precedes only "six" here
  // ("five and a half" sits BEFORE "at", not after "six"), so
  // match_clock_expr's own H+half branch never sees "half" at all -- the
  // discriminator resolves hour=6/min=0, and "five"/"half" are left as
  // ordinary leftover tokens, one NUMBER and one real WORD (HALF_WORDS is
  // never CLS_NUMBER) -- a MIXED residue, correctly preserved by the
  // all-numeric-only gate, matching the "mil veinte"/"5 people" precedent
  // directly above. This looks like a conflation with a separate "at six
  // and a half" compound example elsewhere, not a distinct third bug --
  // pinned to the verified real value.
  assert_clock_target("five and a half at six", true, 6, 0, false);
  assert_clock_target_name("five and a half at six", "five half");
}

// -- CT: nl_clock_target_to_duration -- the pure math boundary table.
// Exhaustive over: midnight wrap, now==target (must resolve to the NEXT
// occurrence, never 0), 11:59->12:00, and 12h vs 24h phrasing.
static void assert_clock_duration(int hour, int minute, bool hour24_known, int now_sod,
                                   int32_t exp_duration_s, int32_t exp_target_sod) {
  NlClockTarget t = {true, (int8_t)hour, (int8_t)minute, hour24_known};
  int32_t target_sod = -1;
  int32_t dur = nl_clock_target_to_duration(&t, now_sod, &target_sod);
  TEST_ASSERT_EQUAL_INT32(exp_duration_s, dur);
  TEST_ASSERT_EQUAL_INT32(exp_target_sod, target_sod);
}

void test_clock_target_to_duration_boundary_table(void) {
  // hour24_known: plain same-day countdown.
  assert_clock_duration(18, 0, true, 9 * 3600, 9 * 3600, 18 * 3600);      // 9:00 -> 18:00 = 9h
  assert_clock_duration(6, 45, true, 6 * 3600, 45 * 60, 6 * 3600 + 45 * 60);  // 6:00 -> 6:45 = 45min

  // Midnight wrap: target already passed today -> +24h.
  assert_clock_duration(6, 0, true, 12 * 3600, 18 * 3600, 6 * 3600);  // 12:00 -> next 6:00 (tomorrow)
  assert_clock_duration(23, 59, true, 0, 23 * 3600 + 59 * 60, 23 * 3600 + 59 * 60);  // 00:00 -> 23:59 today

  // now==target exactly -> next occurrence (+24h for hour24_known), never 0.
  assert_clock_duration(6, 0, true, 6 * 3600, 86400, 6 * 3600);

  // 11:59 -> 12:00 boundary (hour24_known, no wrap needed).
  assert_clock_duration(12, 0, true, 11 * 3600 + 59 * 60, 60, 12 * 3600);

  // 12h ambiguous phrasing: nearest-future between the AM/PM slot.
  // now=09:00, target hour=6 (1-12 range) -> AM slot (6:00) already passed
  // today, PM slot (18:00) is 9h away -> PM wins.
  assert_clock_duration(6, 30, false, 9 * 3600, 9 * 3600 + 30 * 60, 18 * 3600 + 30 * 60);
  // now=03:00, target hour=6 -> AM slot (6:00) is 3h away, PM slot (18:00)
  // is 15h away -> AM wins (nearest future).
  assert_clock_duration(6, 0, false, 3 * 3600, 3 * 3600, 6 * 3600);
  // now=07:00, target hour=6 -> AM slot (6:00) already passed (-1h -> +24h
  // = 23h away), PM slot (18:00) is 11h away -> PM wins.
  assert_clock_duration(6, 0, false, 7 * 3600, 11 * 3600, 18 * 3600);

  // 12h ambiguous, "twelve" (noon/midnight) -- hour%12==0 collapses to the
  // 0:00/12:00 pair, not 12:00/24:00.
  // now=01:00, target hour=12 (ambiguous) -> midnight (0:00) already
  // passed (-1h -> +24h = 23h away), noon (12:00) is 11h away -> noon wins.
  assert_clock_duration(12, 0, false, 1 * 3600, 11 * 3600, 12 * 3600);
  // now=13:00, target hour=12 (ambiguous) -> midnight tonight (0:00, i.e.
  // the NEXT day's 00:00 = 86400) is 11h away, noon (12:00) already passed
  // (-1h -> +24h = 23h away) -> midnight (as +24h, i.e. delta 11h) wins.
  assert_clock_duration(12, 0, false, 13 * 3600, 11 * 3600, 0);

  // Defensive clamping -- an out-of-range NlClockTarget (never produced by
  // parse_nl() itself, but this function must never crash/UB on one) is
  // clamped into its documented range before computing.
  assert_clock_duration(30, 90, true, 0, 23 * 3600 + 59 * 60, 23 * 3600 + 59 * 60);  // clamped to 23:59
  // hour=0 (below the 1-12 floor for an ambiguous target) clamps to 1;
  // minute=-5 clamps to 0. now=0 -> AM slot (1:00=3600) is nearest-future.
  assert_clock_duration(0, -5, false, 0, 3600, 3600);
}

// -- Subtractive clock family -------------------------------------------------
// shape B ("<HOUR> menos <N|cuarto>") and shape A ("[menos]? N|cuarto para
// las <HOUR>" / en "N to H"). Every row here was SILENT-WRONG before this
// (confidently the bare hour, "menos cuarto"/"para las..." dropped as
// residue) -- see match_subtractive_shape_a/match_subtractive_shape_b's own
// doc comments for the exact mechanism.
void test_subtractive_clock_family(void) {
  // Shape B (es, "H menos N"), plus corpus siblings.
  assert_clock_target("las seis menos cuarto", true, 5, 45, false);
  assert_clock_target("a las seis menos cuarto", true, 5, 45, false);
  assert_clock_target("las ocho menos veinte", true, 7, 40, false);
  assert_clock_target("a las nueve menos diez", true, 8, 50, false);
  assert_clock_target("a la una menos veinte", true, 12, 40, false);      // H=1 wraps to 12
  // "hasta" glue.
  assert_clock_target("hasta las seis menos cuarto", true, 5, 45, false);
  assert_clock_target("despertame hasta las seis menos cuarto", true, 5, 45, false);
  // Trailing meridiem describes the FINAL (post-subtraction) instant, not H.
  assert_clock_target("alarma para las seis menos veinte de la manana", true, 5, 40, true);

  // Shape A (es, "[menos]? N|cuarto para las H", NO leading prep needed) --
  // Plus "diez para las ocho" (the family's own third example).
  assert_clock_target("un cuarto para las seis", true, 5, 45, false);
  assert_clock_target("veinte para las cinco", true, 4, 40, false);
  assert_clock_target("diez para las ocho", true, 7, 50, false);
  // Reversed idiom: "menos cuarto para las diez".
  assert_clock_target("menos cuarto para las diez", true, 9, 45, false);
  // A meridiem on the STATED hour resolves before the subtraction.
  assert_clock_target("un cuarto para las tres de la tarde", true, 14, 45, true);

  // Shape A (en, "N to H") -- ONLY inside an outer EN prep;
  // bare (no "at") stays fail-soft as the safe-abstention lock.
  assert_clock_target("at ten to six", true, 5, 50, false);
  assert_clock_target("ring me at ten to six", true, 5, 50, false);
  assert_clock_target("at twenty to nine", true, 8, 40, false);
  // "nine" (the stated hour) has no meridiem of its own here, so this
  // stays 12h-ambiguous (hour24_known false), even though the SUBTRACTOR
  // "twenty" happens to be a number that would itself be in the 13-23 range
  // if misread as an hour -- it never is one here, it's purely the N in
  // "twenty to nine".
  assert_clock_target("remind me at twenty to nine", true, 8, 40, false);
  assert_clock_target("trains at ten to eight", true, 7, 50, false);

  // Bare "ten to six"/"twenty to nine" (no prep at all) stay
  // fail-soft -- the discriminator's own safe-abstention line, unchanged.
  assert_clock_target("ten to six", false, 0, 0, false);
  assert_parse("ten to six", false, KIND_COUNTDOWN, 0, "ten to six");
  assert_clock_target("twenty to nine", false, 0, 0, false);

  // Names: every shape-A/B span (prep, glue, subtractor, hour) is fully
  // consumed -- empty name on the plain rows.
  assert_clock_target_name("las seis menos cuarto", "");
  assert_clock_target_name("un cuarto para las seis", "");
  assert_clock_target_name("at ten to six", "");

  // Must-not-break locks: the additive
  // forms, unaffected by the new subtractive attempt.
  assert_clock_target("a las seis y media", true, 6, 30, false);
  assert_clock_target("at six thirty", true, 6, 30, false);
  assert_clock_target("a las ocho de la tarde", true, 20, 0, true);
  assert_clock_target("a las seis y cuarto", true, 6, 15, false);
}

// -- Arbitrary post-hour minutes + meridiem completion/clock-hedge
// tolerance + midnight/noon flip. ------------------------------------------
void test_arbitrary_minutes_and_meridiem_and_midnight(void) {
  // A plain NUMBER after "y" is the minute too, not just cuarto/media.
  assert_clock_target("a las diez y veinte", true, 10, 20, false);  // was SW 10:00
  assert_clock_target("a las cinco y diez", true, 5, 10, false);
  assert_clock_target("a las seis y veinticinco", true, 6, 25, false);
  assert_clock_target("a las ocho y cinco", true, 8, 5, false);
  assert_clock_target("despiertame a las seis y veinte", true, 6, 20, false);
  // Must-not-break locks: cuarto/media still hit their own special cases
  // first, and a bare hour is unaffected.
  assert_clock_target("a las seis y cuarto", true, 6, 15, false);  // regression lock
  assert_clock_target("a las cinco y media", true, 5, 30, false);
  assert_clock_target("a las seis", true, 6, 0, false);

  // Bare trailing meridiem, no preposition at all.
  assert_clock_target("6:45 pm", true, 18, 45, true);
  assert_clock_target("seis cuarenta y cinco de la tarde", true, 18, 45, true);  // was SW duration 405
  assert_clock_target("6:45 am", true, 6, 45, true);
  assert_clock_target("5 pm", true, 17, 0, true);
  assert_clock_target("8pm", true, 20, 0, true);
  assert_clock_target("six pm", true, 18, 0, true);
  assert_clock_target("six forty five pm", true, 18, 45, true);
  // Bare noon/midnight nouns (en + es), with or without a prep.
  assert_clock_target("noon", true, 12, 0, true);
  assert_clock_target("midnight", true, 0, 0, true);
  assert_clock_target("mediodia", true, 12, 0, true);
  assert_clock_target("medianoche", true, 0, 0, true);
  assert_clock_target("at noon", true, 12, 0, true);
  assert_clock_target("until midnight", true, 0, 0, true);
  // Must-not-break: a bare number with no meridiem, no prep, stays a
  // duration -- "20 minutes" must never become clock=20:00 just because 20
  // is in the 13-23 range (the had_meridiem-strict gate).
  assert_clock_target("twenty minutes", false, 0, 0, false);
  assert_parse("twenty minutes", true, KIND_COUNTDOWN, 1200, "");
  assert_clock_target("eighteen minutes", false, 0, 0, false);

  // Clock-hedge tolerance -- "y pico"/"y algo" consumed, dropped,
  // resolved target kept exactly as-is.
  assert_clock_target("a las ocho y pico", true, 8, 0, false);
  assert_clock_target_name("a las ocho y pico", "");                // "must be a clean 8:00 with an empty name"
  assert_clock_target("hasta las ocho y pico", true, 8, 0, false);
  assert_clock_target("despertame a las seis y algo", true, 6, 0, false);
  assert_clock_target_name("despertame a las seis y algo", "despertame");

  // Midnight/noon flip -- the PM branch's missing hour==12 mirror.
  assert_clock_target("a las doce de la noche", true, 0, 0, true);   // was SW noon
  assert_clock_target("a las doce de la tarde", true, 12, 0, true);  // regression lock
  assert_clock_target("a las doce de la manana", true, 0, 0, true);  // AM branch, already correct (lock)
  assert_clock_target("a las ocho de la noche", true, 20, 0, true);  // must not break
}

// -- "for six thirty" disambiguation ------------------------------------------
// An alarm/wake cue promotes an otherwise-ordinary unit-less
// two-number-group duration pair to a clock target; without one, it stays
// the shipped spoken-pair duration.
void test_alarm_cue_for_disambiguation(void) {
  assert_clock_target("set an alarm for six thirty", true, 6, 30, false);
  assert_clock_target_name("set an alarm for six thirty", "");
  {
    NlParse r;
    parse_nl("set an alarm for six thirty", &r);
    TEST_ASSERT_EQUAL_INT(KIND_COUNTDOWN, r.kind);
    TEST_ASSERT_FALSE(r.has_duration);
  }

  // No alarm cue -- "for" itself is never promoted to a clock preposition;
  // stays the shipped 390s spoken-pair duration.
  assert_parse("timer for six thirty", true, KIND_COUNTDOWN, 390, "");
  assert_clock_target("timer for six thirty", false, 0, 0, false);

  // Other alarm/wake cues + the "esta noche" cue.
  assert_clock_target("wake me up six thirty", true, 6, 30, false);
  assert_clock_target("despiertame a las seis treinta", true, 6, 30, false);

  // Must-not-break: a unit-bearing "alarm ... N U" phrase is unaffected (the
  // any_unit gate) -- unchanged pre-existing behavior.
  assert_parse("ponme una alarma de cinco minutos", true, KIND_COUNTDOWN, 300, "");
  assert_parse("la alarma de cinco minutos", true, KIND_COUNTDOWN, 300, "la");
}

// -- Name-extraction + vocab + articles ---------------------------------------
// Corrections, and unit-less default-minutes, were restored in v0.1.13 --
// their own rows are asserted below. A bare "veinte", no cue, stays
// fail-soft as the pre-existing baseline behavior either way (default-
// minutes declines entirely without a create cue), so it is still asserted
// below as a plain regression lock.
void test_namedrop_vocab_and_true_names(void) {
  // English carriers, politeness words, and disfluencies drop from the name.
  assert_parse("give me 15 minutes", true, KIND_COUNTDOWN, 900, "");
  assert_parse("just 5 minutes", true, KIND_COUNTDOWN, 300, "");
  assert_parse("please set a timer for ten minutes", true, KIND_COUNTDOWN, 600, "");
  assert_parse("uh five minutes", true, KIND_COUNTDOWN, 300, "");
  assert_parse("five uh minutes", true, KIND_COUNTDOWN, 300, "");  // disfluency bridge

  // Spanish imperatives and particles drop from the name.
  assert_parse("dame quince minutos", true, KIND_COUNTDOWN, 900, "");
  assert_parse("venga, diez minutos", true, KIND_COUNTDOWN, 600, "");
  assert_parse("pone cinco minutos", true, KIND_COUNTDOWN, 300, "");  // accent-folded

  // English indefinite article a/an -> 1, directly before a unit.
  assert_parse("an hour", true, KIND_COUNTDOWN, 3600, "");
  assert_parse("a minute", true, KIND_COUNTDOWN, 60, "");

  // True-name lock: article coercion never touches a real scenario noun.
  assert_parse("set the parking meter for an hour", true, KIND_COUNTDOWN, 3600,
               "the parking meter");  // "set"/"for" are FILLER and "the" is a
                                       // real CLS_WORD, kept by the parser.

  // -illo/-ico diminutive units + cuartito (KEPT -- real headroom measured
  // after the corrections/default-minutes cut + 19-table packed-vocab
  // refactor).
  assert_parse("pon quince minutillos", true, KIND_COUNTDOWN, 900, "");
  assert_parse("cinco minuticos", true, KIND_COUNTDOWN, 300, "");
  assert_parse("un cuartito de hora", true, KIND_COUNTDOWN, 900, "");

  // Corrections / last-value-wins -- the later value always wins
  // outright, consuming the marker + new number(+unit) so no residue leaks.
  assert_parse("ten minutes actually fifteen", true, KIND_COUNTDOWN, 900, "");
  assert_parse("veinte minutos digo treinta", true, KIND_COUNTDOWN, 1800, "");

  // Guarded default-minutes -- a unit-less imperative numeral resolves
  // to N*60s only when an explicit create/imperative cue is present.
  assert_parse("ponme veinte", true, KIND_COUNTDOWN, 1200, "");

  // A bare unit-less number with no create cue stays fail-soft; default-
  // minutes declines entirely without a create cue.
  {
    NlParse r;
    parse_nl("veinte", &r);
    TEST_ASSERT_FALSE(r.ok);
  }

  // TRUE-NAME LOCKS -- NAMEDROP must never eat a scenario noun.
  assert_parse("pasta for eight minutes", true, KIND_COUNTDOWN, 480, "pasta");
  // es "en" is deliberately not FILLER, so it survives in the name --
  // asserting the real implementation output ("el mate en"), not the
  // idealized "el mate"; the point is the duration+kind and "mate" itself
  // being present, both true.
  assert_parse("el mate en diez minutos", true, KIND_COUNTDOWN, 600, "el mate en");
  assert_parse("porridge for 3 minutes", true, KIND_COUNTDOWN, 180, "porridge");
  assert_parse("siesta de veinte minutos", true, KIND_COUNTDOWN, 1200, "siesta");

  // Repeat/stopwatch regional verb forms.
  assert_parse("temporizador repetitivo de diez minutos", true, KIND_REPEAT, 600, "");
  assert_parse("cronometrame esto", true, KIND_STOPWATCH, 0, "esto");

  // The "de una" idiom's "una" (=1) is a pure-numeric
  // residue that the existing residue-drop already hides once the rest
  // resolves -- no new rule needed, locked here per the contract's own call.
  assert_parse("de una, veinte minutos", true, KIND_COUNTDOWN, 1200, "");
}

// Default-minutes residue guard (v0.1.13 restoration): thorough testing
// surfaced a real SILENT-WRONG regression class the original guard set
// didn't cover -- the rule, as first implemented, only checked "a cue is
// present anywhere" + "one bare number, no unit", never whether anything
// else survived unconsumed. Two classes of input exploited that: (a)
// ES "un"/"una" doubling as the indefinite article, and (b) an EN
// alarm/timer phrase whose clock-target reading failed the discriminator
// (unsupported vocab), leaving a bare hour/count number that the rule
// wrongly read as "N minutes". Locks the fix (parse_nl.c's
// `has_residue_outside_number` check) so it can't silently regress.
void test_default_minutes_residue_guard_declines_when_real_content_survives(void) {
  // "una" is the indefinite article ("set AN alarm"), not a
  // numeral; must stay fail-soft, not fabricate a spurious 60s duration.
  {
    NlParse r;
    parse_nl("pon una alarma", &r);
    TEST_ASSERT_FALSE(r.ok);
  }
  // "un toque"/"un cacho" are the already-documented
  // deliberately-excluded vague-duration idioms (NAMEDROP_WORDS_BLOB's own
  // comment) -- must stay fail-soft.
  {
    NlParse r;
    parse_nl("dale, un toque nomas", &r);
    TEST_ASSERT_FALSE(r.ok);
  }
  {
    NlParse r;
    parse_nl("pone un cacho de tiempo para la pasta", &r);
    TEST_ASSERT_FALSE(r.ok);
  }
  // "una vuelta con el cronometro" must keep resolving
  // KIND_STOPWATCH (has_duration=false) -- default-minutes must never clobber an
  // otherwise-correct kind resolution with a fabricated COUNTDOWN duration.
  assert_parse("dale, date una vuelta con el cronometro", true, KIND_STOPWATCH, 0,
               "date vuelta con el");
  // EN alarm/timer phrases whose clock-target reading failed the
  // discriminator (unsupported vocab: "quarter past"/"o'clock", or a
  // non-time quantity: cricket "overs") -- the bare hour/count number left
  // over must NOT be read as "N minutes".
  {
    NlParse r;
    parse_nl("set an alarm for quarter past seven", &r);
    TEST_ASSERT_FALSE(r.ok);
  }
  {
    NlParse r;
    parse_nl("ten overs, set a timer", &r);
    TEST_ASSERT_FALSE(r.ok);
  }
  {
    NlParse r;
    parse_nl("set an alarm for six o'clock sharp", &r);
    TEST_ASSERT_FALSE(r.ok);
  }
  // Must-not-break: every genuine default-minutes conversion (above, plus the
  // digit-cue forms) still fires -- a clean "cue + bare number, nothing
  // else" utterance is unaffected by this guard.
  assert_parse("dale 15", true, KIND_COUNTDOWN, 900, "");
  assert_parse("pone 20", true, KIND_COUNTDOWN, 1200, "");
  assert_parse("dale 10", true, KIND_COUNTDOWN, 600, "");
}

// Fix lock: "echa" is a listed create cue (CREATE_CUE_WORDS_BLOB) but,
// unlike the other 8 cues, was never added to
// FILLER_WORDS_BLOB/NAMEDROP_WORDS_BLOB -- it stayed unconsumed CLS_WORD,
// so it was non-functional as a solo cue ("echa 20" always declined) and
// poisoned the default-minutes residue guard even alongside a second,
// working cue ("echa pon veinte" also declined, despite "pon" being a
// perfectly good cue on its own). Fixed both ways (see the
// CREATE_CUE_WORDS_BLOB and guard comments in parse_nl.c for the full
// reasoning): "echa" added to NAMEDROP_WORDS_BLOB (uniform
// consumption/empty-name-residue behavior with the other 8 cues, not just a
// bare "converts" fix) AND the guard now also checks CREATE_CUE_WORDS_BLOB
// membership directly, matching its own doc comment literally
// (belt-and-suspenders against a future cue word added to the blob but not
// the vocab tables).
void test_echa_cue_converts_and_default_minutes_guard_stays_precise(void) {
  // "echa" alone, solo cue -- now converts like the other 8, with an empty
  // name residue.
  assert_parse("echa 20", true, KIND_COUNTDOWN, 1200, "");

  // A composite repro: a co-occurring cue ("pon") no longer gets
  // poisoned by an unconsumed "echa" sitting elsewhere in the utterance.
  assert_parse("echa pon veinte", true, KIND_COUNTDOWN, 1200, "");

  // Must-not-break: the guard still declines the pre-existing 21-row
  // regression class -- spot-check the representative row; the full class
  // stays locked by test_default_minutes_residue_guard_declines_when_real_content_survives
  // above, unmodified and still green.
  {
    NlParse r;
    parse_nl("pon una alarma", &r);
    TEST_ASSERT_FALSE(r.ok);
  }
}

// Fix lock: the corrections rule's differing-unit handling previously
// discarded the user's stated NEW unit and silently reused the ORIGINAL
// match's multiplier against it whenever a trailing unit token was present
// -- because a CLS_UNIT token is ALWAYS pre-consumed at classification time
// (parse_nl.c's own consumed[]-marking switch, whose documented convention
// is to gate a "was a real unit word seen" check on unit_word_exact()
// directly, never on `!consumed[after]`), gating this check on
// `!consumed[after]` meant it could never even SEE a real trailing unit
// word, so it always silently fell through to the bare-reuse branch -- "two
// hours actually ten minutes" produced 36000s (10 HOURS) with zero name
// residue, when the user said "ten minutes". Fixed: a present but
// DIFFERING unit now declines the entire rebind, leaving the ORIGINAL match
// standing with the correction phrase visible as name residue -- matches
// this block's own doc comment ("a differing unit declines, leaving the
// original value standing rather than guessing") and is also the
// historically shipped, safe behavior for this exact input.
void test_differing_unit_correction_declines_rebind(void) {
  // Differing units: original value stands, correction phrase surfaces as
  // name residue -- never silently wrong, never invisible.
  assert_parse("two hours actually ten minutes", true, KIND_COUNTDOWN, 7200, "actually ten");
  assert_parse("ten minutes actually two hours", true, KIND_COUNTDOWN, 600, "actually two");
  assert_parse("one hour actually five seconds", true, KIND_COUNTDOWN, 3600, "actually five");

  // Must-not-break: same-unit corrections still rebind cleanly -- both with
  // an explicit (matching) trailing unit on the correction, and the bare
  // "trailing unit re-binds" form with no unit at all.
  assert_parse("ten minutes actually fifteen minutes", true, KIND_COUNTDOWN, 900, "");
  assert_parse("ten minutes actually fifteen", true, KIND_COUNTDOWN, 900, "");
}

// -- Pair-rule tightening + ranges --------------------------------------------

// The pair-rule definite-article precondition ("the twenty twenty"->refuse
// instead of 1220s SW) was restored in v0.1.13 -- a "the NN NN"/"el/la/los/
// las NN NN" shape is a named entity (a cricket match, a train), never a
// spoken duration.
void test_pair_rule_definite_article_guard(void) {
  assert_parse("six forty five", true, KIND_COUNTDOWN, 405, "");  // no article, unaffected
  // The definite-article guard declines these entirely (ok=false) -- the definite
  // article "the" immediately precedes the spoken-pair's own first group.
  assert_parse("the twenty twenty", false, KIND_COUNTDOWN, 0, "the twenty twenty");
  assert_parse("set a timer for the twenty twenty", false, KIND_COUNTDOWN, 0,
               "the twenty twenty");  // "set"/"a"/"for" are FILLER, consumed regardless
}

void test_ranges_refuse(void) {
  // A duration range is never resolved to either endpoint -- refuse
  // (ok=false) instead of silently picking a bound.
  // Names below match the ORIGINAL SILENT-WRONG rows' own name fields
  // exactly (e.g. "5 to 10 minutes" -> name='5 to') -- the
  // consumed[] span from the main N-U search's own winning match ("10
  // minutes") is untouched by the veto (only has_duration/duration_s are
  // zeroed), so the residual name is unchanged from before this rule
  // existed; only `ok` flips true->false.
  assert_parse("5 to 10 minutes", false, KIND_COUNTDOWN, 0, "5 to");
  assert_parse("5 or 10 minutes", false, KIND_COUNTDOWN, 0, "5 or");
  assert_parse("between 5 and 10 minutes", false, KIND_COUNTDOWN, 0, "between 5");
  // With ok=false, the pure-numeric-residue-drop no longer applies (it's
  // gated on has_duration, now deliberately zeroed), so the leftover "5"
  // surfaces as name text instead of being dropped -- the same "leftover
  // digits visible on a fail-soft row" behavior every other fail-soft lock
  // in this file already shows (e.g. "ten to six" -> name="ten to six").
  assert_parse("de 5 a 10 minutos", false, KIND_COUNTDOWN, 0, "5");

  // Must NOT break: no trailing unit -- the bare subtractive clock shape
  // stays exactly as before (unaffected by the new range veto, since it
  // requires a trailing UNIT that a bare clock phrase never has).
  {
    NlParse r;
    parse_nl("ten to six", &r);
    TEST_ASSERT_FALSE(r.ok);  // unchanged fail-soft with no preposition
  }
  // A real clock read with "to" stays a clock target, not a refused range
  // (restated here to prove the range guard doesn't regress it).
  assert_clock_target("at ten to six", true, 5, 50, false);
}

// -- Fix: a bare subtractive-clock shape (no leading preposition/article at
// all) with a TRAILING meridiem -- "ten to six pm", "quarter to six pm" --
// was silently resolving to the WRONG (LATE) hour: match_bare_meridiem_
// clock's greedy scan found "six pm"/"nine pm" on its own, before the
// leading "ten to"/"quarter to" fragment was ever offered to the
// subtractive matcher (a REGRESSION vs the v0.1.11 baseline, where these
// strings were safe fail-soft). Fixed by widening match_subtractive_a_bare
// to also try the EN "to" glue form, strictly gated on the recursed
// HOUR-EXPR itself resolving a meridiem -- this is the exact compound cell
// (bare + no prep + trailing meridiem) none of the earlier matrices
// covered.
void test_bare_subtractive_clock_with_trailing_meridiem(void) {
  assert_clock_target("ten to six pm", true, 17, 50, true);
  assert_clock_target("twenty to nine pm", true, 20, 40, true);
  assert_clock_target("quarter to six pm", true, 17, 45, true);
  assert_clock_target("ten to six am", true, 5, 50, true);

  // es siblings: the ES "para las" glue form never had this bug (tried by
  // match_subtractive_a_bare before match_bare_meridiem_clock regardless of
  // a trailing meridiem) -- pinned here as a regression lock on the same
  // repro family, not a new fix.
  assert_clock_target("diez para las seis de la tarde", true, 17, 50, true);
  assert_clock_target("veinte para las nueve de la noche", true, 20, 40, true);
  assert_clock_target("un cuarto para las seis de la tarde", true, 17, 45, true);
  assert_clock_target("cuarto para las seis pm", true, 17, 45, true);

  // The safe-abstention lock (no meridiem, no preposition) must still hold --
  // the widened EN "to" bare attempt is gated strictly on had_meridiem, so
  // it never fires on these.
  assert_clock_target("ten to six", false, 0, 0, false);
  assert_clock_target("twenty to nine", false, 0, 0, false);
  // An outer "at" already establishes context, with no meridiem
  // needed) unaffected.
  assert_clock_target("at ten to six", true, 5, 50, false);
  assert_clock_target("at twenty to nine", true, 8, 40, false);
}

// -- Fix: match_subtractive_a_bare's widened EN "to" retry (above) matches
// starting at the NUMBER token and never looks left, so an outer EN
// preposition ("at"/"until"/"til"/"till") was left unconsumed in the residue
// even though the clock VALUE is correct -- subtractive_a_bare_prep_start
// closes this by checking the immediately preceding token against the same
// CLOCK_PREP_WORDS_EN_BLOB the ordinary prep loop uses. The analogous ES
// "hasta las"/"hasta la" glue (the same gap, initially left out for size
// headroom) was restored in v0.1.13, reusing CLOCK_ART_WORDS_ES_BLOB.
void test_subtractive_clock_prep_residue_consumed(void) {
  assert_clock_target("at ten to six pm", true, 17, 50, true);
  assert_clock_target_name("at ten to six pm", "");
  assert_clock_target("until ten to six pm", true, 17, 50, true);
  assert_clock_target_name("until ten to six pm", "");
  assert_clock_target("til ten to six pm", true, 17, 50, true);
  assert_clock_target_name("til ten to six pm", "");
  assert_clock_target("till ten to six pm", true, 17, 50, true);
  assert_clock_target_name("till ten to six pm", "");

  // ES glue (v0.1.13 restoration): "hasta las"/"hasta la", 2-token lookback.
  assert_clock_target("hasta las diez para las seis pm", true, 17, 50, true);
  assert_clock_target_name("hasta las diez para las seis pm", "");
}

// -- Fix: two independent, equally-plausible hour NUMBERs (joined by
// "and"/"y"/"o", or with no connective at all) sharing one trailing
// meridiem must REFUSE, never fabricate a specific HH:MM by binding the
// second hour as if it were a minute ("5 and 6 pm"->17:06 before this
// fix). two_hour_pair_ambiguous vetoes the entire clock-target search AND
// the no-unit spoken-pair duration fallback (both would otherwise
// independently produce their own, different, fabricated answer for this
// shape).
void test_two_hour_pair_ambiguous_refuses(void) {
  assert_parse("5 and 6 pm", false, KIND_COUNTDOWN, 0, "5 6 pm");
  assert_parse("at 5 and 6 pm", false, KIND_COUNTDOWN, 0, "at 5 6 pm");
  assert_parse("5 y 6 pm", false, KIND_COUNTDOWN, 0, "5 6 pm");
  assert_parse("5 6 pm", false, KIND_COUNTDOWN, 0, "5 6 pm");
  assert_parse("between 5 and 6 pm", false, KIND_COUNTDOWN, 0, "between 5 6 pm");
  // Spanish "o" ("or") signals the identical ambiguity, even though it's
  // never classified CLS_JOINER (only "y"/"and" are).
  assert_parse("a las 3 o 4 de la tarde", false, KIND_COUNTDOWN, 0, "las 3 o 4 la tarde");

  assert_clock_target("5 and 6 pm", false, 0, 0, false);
  assert_clock_target("5 6 pm", false, 0, 0, false);

  // Must NOT break: a genuine hour+minute reading where the minute happens
  // to land in 1-12 but NO meridiem follows is untouched -- the guard is
  // strictly gated on a trailing meridiem being present (already pinned in
  // test_arbitrary_minutes_and_meridiem_and_midnight; re-asserted here as
  // this specific guard's own "must not break" lock).
  assert_clock_target("a las diez y veinte", true, 10, 20, false);
  assert_clock_target("despiertame a las seis y veinte", true, 6, 20, false);
  // A genuine hour+minute pair where the "minute" is out of stand-alone-hour
  // range (13-59) is never ambiguous, meridiem or not.
  assert_clock_target("six forty five pm", true, 18, 45, true);

  // This guard wasn't fully closed at first -- English
  // 'or' was untreated: the connective check only ever tested CLS_JOINER
  // ("y"/"and") and the Spanish literal "o", never English "or" -- even
  // though this function's own doc comment illustrates the exact ambiguity
  // with "wake me at 5 or 6". Closed by adding "or" to the same
  // literal-text check "o" already uses.
  assert_clock_target("5 or 6 pm", false, 0, 0, false);
  assert_clock_target("0 or 1 pm", false, 0, 0, false);
  assert_clock_target("23 or 1 pm", false, 0, 0, false);
  assert_clock_target("1 or 12 am", false, 0, 0, false);
}

// -- Fix: the range-refusal rule only ever covered the UNIT-bearing
// duration search; the no-unit "between N and M"/"de N a M" shape still
// silently resolved via the spoken-M:SS-pair rule (its own gap-tolerant
// "adjacent" check treated the range's own connective -- between/and/
// de...a -- as harmless glue). Tightened the pair rule to require the two
// number groups be STRICTLY adjacent (nothing at all between them,
// matching every genuine spoken-pair's own shape, e.g. "6 45"/"seis
// cuarenta y cinco") -- any connective between two bare numbers now means
// they're being related (a range/list), not spoken together as one M:SS
// pair.
void test_unitless_range_refuses_via_pair_rule(void) {
  assert_parse("between 3 and 4", false, KIND_COUNTDOWN, 0, "between 3 4");
  assert_parse("3 and 4", false, KIND_COUNTDOWN, 0, "3 4");
  assert_parse("call me between 3 and 4", false, KIND_COUNTDOWN, 0, "call between 3 4");
  assert_parse("de 5 a 10", false, KIND_COUNTDOWN, 0, "5 10");
  assert_parse("somewhere between 20 and 30", false, KIND_COUNTDOWN, 0, "somewhere between 20 30");

  // Must NOT break: the genuine strictly-adjacent spoken-pair rows (no
  // connective at all between the two groups) are unaffected.
  assert_parse("seis cuarenta y cinco", true, KIND_COUNTDOWN, 405, "");
  assert_parse("six forty five", true, KIND_COUNTDOWN, 405, "");
  assert_parse("6 45", true, KIND_COUNTDOWN, 405, "");
  assert_parse("cero cinco", true, KIND_COUNTDOWN, 5, "");
  assert_parse("veinte veinte", true, KIND_COUNTDOWN, 1220, "");
}

// -- Fix: match_subtractive_shape_b discarded an already-resolved EXPLICIT
// minute (colon, "y veinte", "y media") the moment a "menos N" followed
// ANY resolved hour, even one that already carried its own unambiguous
// minute -- silently replacing the stated time with unrelated arithmetic
// ("a las seis y veinte menos cuarto" -> 5:45, discarding the stated :20
// entirely). Mirrors the existing had_mer1 guard: declines shape B whenever
// H already carried an explicit minute, letting the plain match_clock_expr
// reading stand (H:MM kept, "menos N..." left as residue/name) -- same
// posture as the had_mer1 case just above it.
void test_subtractive_b_preserves_already_explicit_minute(void) {
  assert_clock_target("a las seis y veinte menos cuarto", true, 6, 20, false);
  assert_clock_target_name("a las seis y veinte menos cuarto", "menos cuarto");
  assert_clock_target("a las seis y media menos cuarto", true, 6, 30, false);
  assert_clock_target_name("a las seis y media menos cuarto", "menos cuarto");
  assert_clock_target("a las 6:45 menos cuarto", true, 6, 45, false);
  assert_clock_target_name("a las 6:45 menos cuarto", "menos cuarto");

  // Must NOT break: a bare hour (no explicit minute of its own) before
  // "menos N" is exactly the attested shape B -- unaffected.
  assert_clock_target("las seis menos cuarto", true, 5, 45, false);
  assert_clock_target("a las nueve menos diez", true, 8, 50, false);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_en_basic_number_unit_pairs);
  RUN_TEST(test_es_basic_number_unit_pairs);
  RUN_TEST(test_en_repeat_kind_position_independent);
  RUN_TEST(test_es_repeat_kind_position_independent);
  RUN_TEST(test_vocab_recurrente_recurring_fold);
  RUN_TEST(test_vocab_regresiva_and_alarma_article_fold);
  RUN_TEST(test_stopwatch_keyword_alone_en_es);
  RUN_TEST(test_compound_duration_sum_en_es);
  RUN_TEST(test_name_residue_both_orders_en_es);
  RUN_TEST(test_en_number_word_coverage);
  RUN_TEST(test_es_number_word_coverage);
  RUN_TEST(test_homophones_position_aware);
  RUN_TEST(test_es_para_stays_filler_not_a_homophone);
  RUN_TEST(test_fuzzy_false_positives_fixed);
  RUN_TEST(test_number_accumulator_structural_validation);
  RUN_TEST(test_kind_resolution_is_clause_scoped);
  RUN_TEST(test_set_and_pon_are_fillers);
  RUN_TEST(test_token_cap_handles_long_preamble);
  RUN_TEST(test_magnitude_guard_complete_lexicon);
  RUN_TEST(test_benign_embedded_number_names_no_longer_fail_the_whole_parse);
  RUN_TEST(test_magnitude_protections_still_fail_soft);
  RUN_TEST(test_digit_token_repeat_regression);
  RUN_TEST(test_bare_article_plus_unit);
  RUN_TEST(test_unitless_fractional_durations_unsupported);
  RUN_TEST(test_once_en_es_collision);
  RUN_TEST(test_normalize_merges_across_unsupported_multibyte);
  RUN_TEST(test_dictation_emits_real_words_not_typos);
  RUN_TEST(test_code_switch_mixed_language);
  RUN_TEST(test_spanish_y_joiner_vs_filler);
  RUN_TEST(test_ambiguous_shapes_fall_back);
  RUN_TEST(test_range_guards_zero_and_over_99h);
  RUN_TEST(test_name_residue_preserves_raw_accents_and_case);
  RUN_TEST(test_garbage_and_edge_inputs_never_crash);
  RUN_TEST(test_normalize_folds_accents_lowercases_and_strips_punct);
  RUN_TEST(test_normalize_splits_hyphens_into_spaces);
  RUN_TEST(test_normalize_collapses_and_trims_whitespace);
  RUN_TEST(test_normalize_null_input_is_empty_string);
  RUN_TEST(test_normalize_folds_all_six_spanish_accents);
  RUN_TEST(test_normalize_drops_punctuation_without_inserting_a_boundary);
  RUN_TEST(test_normalize_truncates_safely_never_overflows);
  RUN_TEST(test_words_to_number_plain_digit);
  RUN_TEST(test_words_to_number_en_compound_tens_and_ones);
  RUN_TEST(test_words_to_number_es_veinti_single_token);
  RUN_TEST(test_words_to_number_es_tens_y_ones_compound);
  RUN_TEST(test_words_to_number_non_number_first_token_consumes_nothing);
  RUN_TEST(test_words_to_number_joiner_not_followed_by_number_is_not_consumed);
  RUN_TEST(test_spoken_pair_rule_acceptance_matrix);
  RUN_TEST(test_hours_trailing_minutes);
  RUN_TEST(test_minutes_trailing_seconds);
  RUN_TEST(test_pure_numeric_residue_dropped);
  RUN_TEST(test_diminutive_units);
  RUN_TEST(test_approximation_markers);
  RUN_TEST(test_ninety_minutes_regression);
  RUN_TEST(test_fraction_acceptance_matrix);
  RUN_TEST(test_number_and_half_or_quarter);
  RUN_TEST(test_un_cuarto_precedence);
  RUN_TEST(test_multiunit_chains);
  RUN_TEST(test_in_within_fillers);
  RUN_TEST(test_couple_equals_two);
  RUN_TEST(test_decimal_failsoft_guard);
  RUN_TEST(test_colon_time_duration_routing);
  RUN_TEST(test_slash_fraction_duration_routing);
  RUN_TEST(test_decimal_duration_routing);
  RUN_TEST(test_hundred_composition);
  RUN_TEST(test_hundred_cull_adversarial_lock);
  RUN_TEST(test_clock_target_discriminator);
  RUN_TEST(test_colon_clock_routing_flip);
  RUN_TEST(test_clock_target_pure_numeric_residue_drop);
  RUN_TEST(test_clock_target_to_duration_boundary_table);
  RUN_TEST(test_subtractive_clock_family);
  RUN_TEST(test_arbitrary_minutes_and_meridiem_and_midnight);
  RUN_TEST(test_alarm_cue_for_disambiguation);
  RUN_TEST(test_namedrop_vocab_and_true_names);
  RUN_TEST(test_default_minutes_residue_guard_declines_when_real_content_survives);
  RUN_TEST(test_echa_cue_converts_and_default_minutes_guard_stays_precise);
  RUN_TEST(test_differing_unit_correction_declines_rebind);
  RUN_TEST(test_pair_rule_definite_article_guard);
  RUN_TEST(test_ranges_refuse);
  RUN_TEST(test_bare_subtractive_clock_with_trailing_meridiem);
  RUN_TEST(test_subtractive_clock_prep_residue_consumed);
  RUN_TEST(test_two_hour_pair_ambiguous_refuses);
  RUN_TEST(test_unitless_range_refuses_via_pair_rule);
  RUN_TEST(test_subtractive_b_preserves_already_explicit_minute);
  return UNITY_END();
}
