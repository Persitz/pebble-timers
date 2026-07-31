// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Herman Persitz

#include "parse_nl.h"

#include <string.h>

#ifdef PBL_PLATFORM_EMERY
static const uint8_t *s_vocabulary;
#else
#include "../../../resources/generated/nl_tables_host.inc"
static const uint8_t *s_vocabulary = NL_TABLES_HOST_DATA;
#endif

#define NL_TABLES_BASE s_vocabulary
#include "../../../resources/generated/nl_tables.h"

static uint32_t vocabulary_crc32(const uint8_t *data, size_t size) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < size; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      uint32_t low_bit = crc & 1u;
      crc >>= 1;
      if (low_bit != 0u) crc ^= 0xedb88320u;
    }
  }
  return crc ^ 0xffffffffu;
}

bool parse_nl_vocabulary_set(const uint8_t *data, size_t size) {
  s_vocabulary = NULL;
  if (data == NULL || size != NL_TABLES_TOTAL) return false;
  if (vocabulary_crc32(data, size) != NL_TABLES_CRC32) return false;
  s_vocabulary = data;
  return true;
}

size_t parse_nl_vocabulary_size(void) {
  return NL_TABLES_TOTAL;
}

bool parse_nl_vocabulary_available(void) {
  return s_vocabulary != NULL;
}

// -- Tunables -----------------------------------------------------------
// 12 was tight enough that a realistic politeness preamble ("please could
// you kindly go ahead and just set a timer for exactly five minutes
// thanks") consumed the whole cap before the actual duration (at tokens
// 14-15) was ever seen, silently dropping it (ok=false with no hint).
// Raised to 24 -- stack cost is trivial (a few dozen extra bytes of
// NlToken/char-buffer locals in parse_nl's own frame) and 24 tokens
// comfortably covers that example with headroom.
#define NL_MAX_TOKENS 24  // cap ~12 tokens minimum expected; overflow -> truncate
#define NL_TOKEN_MAX 24   // longest realistic vocab word ("temporizador", "veinticuatro") + margin

#define ARRLEN(x) ((int)(sizeof(x) / sizeof((x)[0])))

// -- Token model ----------------------------------------------------------

typedef enum {
  CLS_NUMBER,
  CLS_UNIT,
  CLS_KIND_REPEAT,
  CLS_KIND_STOPWATCH,
  CLS_KIND_COUNTDOWN,  // explicit "timer"/"temporizador" etc -- redundant w/ the default, still consumed
  CLS_JOINER,          // "y" / "and" -- meaning (number-joiner vs nothing) resolved by position
  CLS_FILLER,
  // An approximation marker ("unos"/"como"/"about"/"around"/"like") --
  // consumed (dropped from name residue) like a filler, but deliberately
  // kept a SEPARATE class from CLS_FILLER: the clause-scoped kind-resolution
  // walk (below) treats CLS_FILLER as transparent glue, and the FILLER set's
  // minimality is a load-bearing invariant (see FILLER_WORDS_BLOB's own
  // comment). An approximation adverb is not kind-glue, so it must stay
  // OPAQUE to that walk -- this class is never added to the walk's
  // "continue" set, so it falls through to the same `break` a CLS_WORD hits
  // (a name/clause boundary), while still being dropped from the residue via
  // its own consumed[] entry below.
  CLS_APPROX,
  // A closed pollution vocabulary -- carriers/politeness ("please"/"just"/
  // "give"/"me"), disfluency ("uh"/"um"/"eh"/"este"), regional imperatives/
  // particles (voseo "dale"/"pone", Rioplatense "che"/"ta"/"bo"), and
  // single-token hedges ("tipo"/"pico"/"monedas"/"capaz") -- mirrors
  // CLS_APPROX EXACTLY: consumed (dropped from the name residue) below, but
  // deliberately kept OPAQUE to the clause-scoped kind-resolution walk (never
  // added to that walk's "continue" set -- falls to the same `else` ->
  // break an unconsumed CLS_WORD hits). This is the whole point: it removes
  // name pollution WITHOUT growing FILLER_WORDS, so the FILLER set's
  // load-bearing minimal invariant (which protects "cancel the repeat
  // timer actually just 5 minutes"->COUNTDOWN) is untouched -- NAMEDROP_WORDS
  // (below) never overlaps FILLER_WORDS's true grammatical connectives.
  // Never contains a scenario noun (pasta/mate/porridge/etc.) -- see that
  // table's own comment for the audited must-not-break list.
  CLS_NAMEDROP,
  // "6:45" / "18:30" / "1:20:30" -- a colon token is its OWN class, never
  // CLS_NUMBER, so it can never enter the tens+ones accumulator or the
  // two-number-group spoken-pair collector. Like CLS_NUMBER, it is NOT
  // auto-consumed by the classification loop below and is opaque to the
  // clause-scoped kind-resolution walk when left unmatched (falls to that
  // walk's `else` -> break, same as an unconsumed CLS_NUMBER) -- it is the
  // duration anchor, not connective glue.
  CLS_COLONTIME,
  // "1/2" / "3/4" -- same non-CLS_NUMBER, non-auto-consumed, walk-opaque
  // treatment as CLS_COLONTIME above.
  CLS_SLASHFRAC,
  // "1.5" / "5,5" / "1.25" -- the decimal-quantity shape (replaces an
  // earlier split-into-two-CLS_NUMBER-tokens behavior). Same non-CLS_NUMBER,
  // non-auto-consumed, walk-opaque treatment.
  CLS_DECIMAL,
  CLS_WORD,            // unclassified -- candidate name residue
} NlTokClass;

typedef struct {
  char raw[NL_TOKEN_MAX];   // original text (case/accents preserved) -- used for name residue
  char norm[NL_TOKEN_MAX];  // lowercase/accent-folded/typo-corrected -- used for vocab lookups
  NlTokClass cls;
} NlToken;

// -- Vocabulary tables (merged es/en) ----------------------------------
// The 26 byte definitions live in parse_nl_vocab.def. The generated header
// above maps every historical *_BLOB identifier to its checked offset in
// the one installed resource span, so the grammar below is unchanged.

// NUMBER_WORDS used to be a `{const char*, value}` pointer-struct array --
// `.data.rel.ro` (552 B: 69 pointers + 69 relocations) for a payload that's
// really just a small per-word int. Converted to the file's established
// NUL-separated blob convention (see exact_match_blob's own comment) plus a
// PARALLEL `int8_t` value array in the identical word order -- every
// NUMBER_WORDS value (0-90) fits int8 cleanly. `number_word_exact` (below)
// walks the blob with `blob_find_index` (shared with UNIT_WORDS) and
// indexes the parallel array; same O(total chars) linear-scan cost as the
// old pointer-array walk, no relocations, no pointer table.
//
// en 0-19 + tens; es 0-29 (veinti- forms are single lexicalized tokens) + tens.
// The accumulator below handles compounding ("twenty five" / "treinta y
// cinco") uniformly -- this table only needs the individual atoms.
// Parallel to NUMBER_WORDS_BLOB, same word order -- index `i` here is the
// value for the i-th blob entry (0-based ordinal, per blob_find_index).
static const int8_t NUMBER_VALUES[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 30, 40, 50, 60, 70, 80, 90,
    2,  // couple
    0, 1, 1, 1, 2, 3,
    4, 5, 6, 7, 8, 9,
    10, 11, 12, 13, 14,
    15, 16, 17, 18, 19,
    20, 21, 21, 22,
    23, 24, 25, 26,
    27, 28, 29,
    30, 40, 50,
    60, 70, 80, 90,
};

// The generator's own checks (blob count, ASCII, trailing NUL,
// whole-payload CRC) have no idea NUMBER_WORDS_BLOB's
// word order must stay in lockstep with this hand-maintained array -- a
// `.def` edit that reorders NUMBER_WORDS_BLOB at equal word count would
// pass every existing check and silently mis-index NUMBER_VALUES at
// runtime. This closes the COUNT half mechanically: an edit that adds/
// removes a word without updating this array now fails the BUILD instead of
// misbehaving at runtime. It cannot catch a reorder at equal count -- that
// half would require moving the values into the .def beside their words.
_Static_assert(ARRLEN(NUMBER_VALUES) == NL_WORD_COUNT_NUMBER_WORDS_BLOB,
               "NUMBER_VALUES must have exactly one entry per NUMBER_WORDS_BLOB word, in the same order");

// Same blob+parallel-array conversion as NUMBER_WORDS above, for UNIT_WORDS.
// Every mult value is one of exactly 3 constants (1/60/3600), so the
// parallel array stores a small `int8_t` INDEX into UNIT_MULTS rather than
// the seconds-per-unit value itself.
static const int32_t UNIT_MULTS[] = {1, 60, 3600};

// Parallel to UNIT_WORDS_BLOB, same word order -- index into UNIT_MULTS.
static const int8_t UNIT_MULT_IDX[] = {
    0, 0, 0, 0,
    0, 0, 0, 0,
    1, 1, 1, 1,
    1, 1,
    2, 2, 2, 2,
    2, 2,
    1, 1,
    2, 2,
    0, 0,
    1, 1,
    0, 0,
    2, 2,
    1, 1,
    0, 0,
    2, 2,
};

// Same rationale as NUMBER_VALUES's own _Static_assert above -- closes the
// count half for UNIT_WORDS_BLOB/UNIT_MULT_IDX mechanically.
_Static_assert(ARRLEN(UNIT_MULT_IDX) == NL_WORD_COUNT_UNIT_WORDS_BLOB,
               "UNIT_MULT_IDX must have exactly one entry per UNIT_WORDS_BLOB word, in the same order");

// "recurrente"/"recurrentes" (es) and "recurring" (en; "repeating" was already present) were
// added as single-token, position-independent kind keywords, same cost/risk tier as
// every other entry in this table. "ciclo"/"cycle" were deliberately NOT added: they collide
// with real timer names such as "wash cycle 45 minutes" (a laundry timer), which would
// silently resolve to KIND_REPEAT if "cycle" were a bare kind keyword, since the clause-scoped
// kind-resolution walk only requires the keyword to sit immediately adjacent to the duration
// match, not that the whole utterance be unambiguous. Unlike "recurring"/"repeat"/"interval",
// "cycle" is common enough as an ordinary standalone noun/timer-name word that this collision
// risk is real, not contrived.
// "repetitivo(s)"/"repite"/"repites" -- pure closed-vocab additions, no
// logic; governed by the clause-scoped kind-resolution walk like every other kind
// keyword.
// "cronometrame"/"cronometra" -- the voseo stopwatch-verb forms (the most
// natural voseo stopwatch verb); KIND_STOPWATCH_WORDS previously had only
// noun forms.
// Explicit/optional -- redundant with the countdown default; listed so these
// tokens are consumed (never leak into the name residue) rather than treated
// as unclassified WORD tokens. "regresiva"/"regresivo" added alongside
// "cuenta"/"atras" (same vocab addition as KIND_REPEAT_WORDS above) --
// "cuenta regresiva" is the other common es phrasing for "countdown"
// alongside the already-covered "cuenta atras".
// "set" (as in "set a timer for...", arguably THE canonical assistant
// phrasing) was missing -- it pollutes the name residue on the single most
// common real phrasing. "pon"/"ponme" are the casual es equivalents ("pon
// un temporizador de..."/"ponme un temporizador de..."). "an" (only "a"
// was listed) is added for the same reason -- "a minute"/"an hour" should
// leave the same (empty) residue either way.
//
// Load-bearing invariant: this MUST stay the minimal closed set of
// true grammatical connectives. The clause-scoped kind-resolution walk (below)
// treats every FILLER token as transparent -- do NOT add discourse markers,
// verbs, or self-correction words ("just", "actually", "cancel", "make",
// "please", "only", "want") here, even if a specific phrasing would
// benefit -- they must stay CLS_WORD so they act as the clause boundary
// that protects cases like "cancel the repeat timer actually just 5
// minutes" (COUNTDOWN, not REPEAT).
//
// "in"/"within" (en only) -- both are true grammatical prepositions/
// connectives, satisfying the FILLER-minimality invariant above, added
// specifically to stop "in 5 minutes"/"within ten minutes" (very common EN
// timer phrasing) from leaking "in"/"within" into the name residue. Spanish
// "en" deliberately NOT added: "en" is far more often real name content
// ("te en hebras", "pollo en salsa") than duration glue, and es already has
// "de"/"por"/"para" -- the same reasoning that already kept "para" a
// filler without coercing "en" the same way.
// Approximation markers -- dropped from the name residue (consumed, like a
// filler) but deliberately kept OUT of FILLER_WORDS (see the CLS_APPROX
// comment on the enum above and the FILLER-minimality invariant comment
// just above this table): these are not grammatical glue, so they must
// stay opaque to the clause-scoped kind-resolution walk. "como"/"unos"/"unas" (es);
// "about"/"around"/"like"/"roughly" (en). The multi-word Spanish "más o
// menos" is a deliberate, still-open scope gap (needs different,
// multi-token matching, not a same-shape table entry) rather than a
// silent omission -- not added.
// The CLS_NAMEDROP pollution vocabulary -- a curated word list built from
// observed name-pollution phrasings, not freelanced or extended beyond what
// those phrasings required. Semantics identical to APPROX_DROP_WORDS above
// (see the CLS_NAMEDROP enum comment): dropped from the name residue, opaque
// to the clause-scoped kind-resolution walk.
//
// Checked against known true-name CORRECT phrasings: contains ONLY
// zero-content function words, carriers, disfluency, and regional discourse
// particles -- NEVER a scenario noun. "pasta"/"plank"/"pomodoro"/"porridge"/
// "mate"/"lavadora"/"siesta"/"asado"/"turno"/"sprint" and AU slang
// ("arvo"/"smoko"/"brekkie") do not appear here and must never be added --
// that is what keeps a true-name phrasing ("pasta for eight minutes"->
// name="pasta") from being silently eaten by an over-eager entry.
//
// Deliberately OUT (an ambiguous-negative-case call, not an omission):
// "un toque"/"un cacho" (vague short-duration NOUNS, "un" is a real NUMBER
// word -- adding "toque"/"cacho" alone would still leave "un" as residue,
// and the whole phrase is treated as an ambiguous negative case). Multi-token
// hedges ("mas o menos", "un rato", "algo asi", "nada mas") are also
// deliberately NOT special-cased here -- none of them is common enough to
// justify it, and per-token membership is preferred FIRST, multi-token only
// if a specific phrase needs to be suppressed exactly (it doesn't, for
// these). "menos" itself is deliberately NOT added despite "mas o menos"
// naming it -- "menos" is load-bearing string content read directly (by norm
// text, not class) by the subtractive-clock grammar
// (parse_subtractive_n_expr/match_subtractive_shape_b); reclassifying it
// risks that family's own pinned regression locks for a small
// hedge-cleanliness gain. A residual "menos" may still surface in a
// non-clock "mas o menos" duration utterance's name.
// The COMPLETE en+es magnitude-morpheme inventory (>99, outside
// this app's 0-99 word-form grammar -- see NUMBER_WORDS). Unlike an
// open-ended fuzzy-match target set, this class is closed and finite --
// natural language does not invent new magnitude morphemes -- so listing it
// to completion once is correct and stays complete. Adjacent to a NUMBER
// token (see the magnitude_adjacent guard in parse_nl below), it disables
// the whole duration search rather than letting an unrelated number+unit
// pair elsewhere in the utterance silently "rescue" a wrong-magnitude match.
//
// "hundred"/"cien"/"ciento"/"cientos" (the HUNDRED_WORDS class, below) are
// DELIBERATELY REMOVED from this list -- they have their own dedicated,
// broader guard (`hundred_present` in parse_nl, below: presence ANYWHERE in
// the utterance, not just adjacency to a NUMBER token like this blob's own
// `magnitude_adjacent` check). The positive hundred-class COMPOSER that used
// to sit behind that guard (match_hundred_composite(),
// "ciento veinte segundos"->120) was removed for size -- `hundred_present`
// alone now REFUSES every hundred-word utterance (fail-soft), which is
// exactly the outcome this exclusion protects: were these four folded back
// into MAGNITUDE_WORDS_BLOB instead of keeping their own broader guard, a
// non-adjacent hundred word (e.g. bare "cien horas") would silently skip
// the guard and risk an unrelated pair elsewhere "rescuing" the match --
// the guard must stay dedicated and presence-based, not merged into this
// adjacency-based one. "hundreds" (en plural -- doesn't compose the same
// way, "two hundreds seconds" isn't a phrasing this grammar needs) stays
// here unchanged. thousand/million/billion (+ es equivalents) are
// UNCHANGED -- there is no composer for them either, following this table's
// own "enumerate a closed class to completion; never chase an open one"
// principle.
// The hundred-CLASS word set -- narrower than MAGNITUDE_WORDS above (see
// that table's own comment for why these four are kept separate from it).
// The positive composer that used to read this list
// (match_hundred_composite()) was removed for size; the ONLY remaining
// reader is the `hundred_present` GUARD in parse_nl (below) -- kept
// deliberately, so a hundred-word utterance still REFUSES (fail-soft)
// instead of an unrelated number+unit pair elsewhere silently "rescuing" it
// into a wrong value (e.g. misreading "ciento veinte segundos" as "veinte
// segundos"=20s).
// The clock-target discriminator's own closed word sets -- kept separate
// from every other vocab table above since they are never number/unit/
// kind/filler/joiner content, only clock-specific markers
// match_clock_target() (below) reads directly by norm string. EN
// prepositions (deliberately NOT "by" -- "by 6" is a deadline nuance, out
// of scope); ES "las"/"la" (the CLOCK_ART class -- "a las seis"/"a la
// una"), consumed only when it actually precedes a valid clock expression
// (see that function's own doc comment for why a bare "la"/"a" elsewhere is
// never affected).
// am/pm meridiem markers -- en single tokens; es "tarde"/"noche"/"manana"
// used after an optional "de"/"la"/"el" (match_clock_expr skips those,
// itself, not via a FILLER_WORDS addition -- keeps this discipline scoped
// to the clock sub-grammar only, never affecting the clause-scoped kind-resolution walk or the
// ordinary duration grammar's residue).
// Standalone clock-noun words -- each resolves to a full clock target on
// its own, no hour/minute grammar needed (match_clock_expr's own preamble,
// below).
// A trailing "y pico"/"y algo"/"y monedas" hedge after a resolved clock
// hour -- consumed and dropped (the resolved target is kept exactly as-is;
// there is no unambiguous "a bit past" value to add), so it neither leaks
// into the name nor derails the meridiem check that follows it inside
// match_clock_expr.
// "set an alarm for six thirty" promotes an otherwise-ordinary unit-less
// two-number-group DURATION pair to a clock target only when one of these
// alarm/wake cues is present somewhere in the utterance (see
// match_alarm_cue_clock's own doc comment for the full reasoning -- "for"
// itself is deliberately never promoted to a generic clock preposition).
// The fractions engine's two fraction-word families. Denominator-4 words
// ("cuarto(s)"/"quarter(s)") combine with an optional preceding NUMBER
// (default 1 if absent, via match_fraction_at below) to form N/4 -- one
// rule covers "cuarto"/"quarter" alone (1/4), "un cuarto" (1/4 -- the
// "un"=1-vs-cuarto-fraction collision, resolved uniformly here rather than
// as a bolted-on special case), and "tres cuartos"/"three quarters" (3/4).
// Standalone 1/2 words need no numerator prefix in this catalog's examples
// ("media hora", "hora y media", "half an hour", ...).
// "cuartito"/"cuartitos" (the diminutive quarter-hour form) added alongside
// the base QUARTER_WORDS -- same pure-vocab addition as the UNIT_WORDS
// diminutives above.
// The pair-rule's own definite-article precondition -- a "the NN NN"/
// "el/la/los/las NN NN" shape is a named entity (a cricket match, a train),
// never a spoken duration (no one says "the six forty five" to mean 405s).
// Read directly by norm text against the token immediately before the
// spoken-M:SS pair's own first group, below -- never added to FILLER_WORDS
// (these are ordinary CLS_WORD name content everywhere else, e.g. "set the
// parking meter for an hour").
// The range-connective word set read directly by norm text against the
// token before a winning duration match's own first bound (see the usage
// site, further down, for why "and" needs its own extra "between"
// precondition rather than living in this table too).
// Closed create/imperative-cue set, read by norm text regardless of
// class/consumed -- "pon"/"ponme"/"set" are FILLER, "dale"/"hazme"/
// "hagame"/"pone"/"echa" are NAMEDROP, all consumed either way; matched
// here by string, not class. "pone" closes voseo "poné veinte" (folds to
// "pone veinte" post-normalization) and the observed real-world phrasing
// "pone 20" -- already NAMEDROP today so adding it here changes zero
// classification, only whether the cue-presence check sees it. Deliberately
// NOT "pona" (not a real voseo form).
//
// "echa" was already listed here as a cue but was never added to
// FILLER_WORDS_BLOB/NAMEDROP_WORDS_BLOB like the other 8 cues, so it stayed
// unconsumed CLS_WORD -- non-functional as a solo cue ("echa 20" always
// declined) and actively poisoned the residue guard below even alongside a
// second, working cue ("echa pon veinte" also declined). Now added to
// NAMEDROP_WORDS_BLOB (alongside "echad", the vosotros form already there --
// same verb, same imperative-particle category) so it is consumed and
// dropped from the name residue exactly like the other 8 cues, and the
// residue guard below also checks CREATE_CUE_WORDS_BLOB membership directly
// (matching this rule's own doc comment) as a second, independent guarantee
// -- belt-and-suspenders: a future cue word added to this blob but not the
// vocab tables still can't re-trigger this bug.
// Closed correction-marker set -- structural marker ("marker between two
// number-groups"), read by norm text only, NEVER added to FILLER (protects
// the clause-scoped kind-resolution walk's clause-boundary detection).
// Verified none of these words appear in
// NAMEDROP_WORDS_BLOB/FILLER_WORDS_BLOB (they must stay CLS_WORD so the
// !consumed[mk] check further down can see them).
// A cheap "at most one edit away" check (substitution, OR a single
// insertion/deletion) -- NOT general-vocabulary fuzzy matching (this file
// has no `nl_levenshtein`/classify-time fuzzy correction at all); this is a
// different, much narrower thing: it never changes any token's
// CLASS/meaning, it only decides whether the structural guard below should
// ALSO fire for a near-miss spelling of a magnitude word. No DP table, no
// allocation -- O(n), matching this file's own no-allocation discipline.
static bool within_one_edit(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  if (la == lb) {
    int diff = 0;
    for (size_t i = 0; i < la; i++) {
      if (a[i] != b[i] && ++diff > 1) return false;
    }
    return true;
  }
  if (la + 1 != lb && lb + 1 != la) return false;  // lengths differ by more than 1
  const char *longer = (la > lb) ? a : b;
  const char *shorter = (la > lb) ? b : a;
  size_t ls = (la > lb) ? lb : la;
  size_t i = 0, j = 0;
  bool skipped = false;
  while (i < ls) {
    if (longer[j] == shorter[i]) {
      i++;
      j++;
      continue;
    }
    if (skipped) return false;
    skipped = true;
    j++;  // skip the longer string's one extra (inserted/deleted) char
  }
  return true;
}

// Is `word` close enough to SOME entry of the SAME 17-word MAGNITUDE_WORDS
// lexicon the exact guard above already uses to plausibly BE a magnitude
// morpheme "we somehow failed to list" -- narrowly scoped to this one
// closed, semantically-coherent list, never a general "is this word close
// to ANY vocab word" test.
static bool word_near_magnitude_lexicon(const char *word) {
  size_t m = 0;
  while (m < sizeof(MAGNITUDE_WORDS_BLOB)) {
    if (within_one_edit(word, MAGNITUDE_WORDS_BLOB + m)) return true;
    m += strlen(MAGNITUDE_WORDS_BLOB + m) + 1;
  }
  return false;
}

// Curated homophone table -- the poor-man's substitute for the n-best/
// confidence rescoring the Dictation API denies us. Deliberately EN-only
// and small: "para"<->"cuatro" was considered and dropped -- "para" is
// also the common Spanish filler "for" (as in "temporizador para cinco
// minutos"), and the position rule below can't tell that legitimate filler
// use apart from a mis-heard "cuatro" the way it can for the EN entries.
//
// Used to be a `{const char*, const char*}` pointer-pair array -- the last
// `.data.rel.ro` survivor after NUMBER_WORDS/UNIT_WORDS were converted the
// same way. Converted to a single NUL-separated blob storing alternating
// heard/canonical pairs, walked two entries at a time by homophone_lookup
// (below, near apply_homophone_coercion, its only caller) -- retires this
// file's pointer-table category entirely.
// -- Small pure helpers ---------------------------------------------------

static bool is_ascii_alnum(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static unsigned char ascii_lower(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

// Folds one Spanish accented Latin-1-supplement letter (UTF-8 lead byte
// 0xC3) to its plain ASCII base letter. Only the letters this app's
// vocabulary actually needs. `*matched` reports whether `b2` was recognized.
static char fold_latin1_supplement(unsigned char b2, bool *matched) {
  *matched = true;
  switch (b2) {
    case 0x81: case 0xA1: return 'a';  // A' a'
    case 0x89: case 0xA9: return 'e';  // E' e'
    case 0x8D: case 0xAD: return 'i';  // I' i'
    case 0x93: case 0xB3: return 'o';  // O' o'
    case 0x9A: case 0xBA: return 'u';  // U' u'
    case 0x9C: case 0xBC: return 'u';  // U-diaeresis (rare) -- folds to plain u too
    case 0x91: case 0xB1: return 'n';  // N-tilde
    default:
      *matched = false;
      return 0;
  }
}

static bool is_boundary_byte(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '-';
}

static bool is_edge_punct(unsigned char c) {
  switch (c) {
    case ',': case '.': case ':': case ';': case '!': case '?':
    case '\'': case '"': case '(': case ')': case '[': case ']':
      return true;
    default:
      return false;
  }
}

// Supersedes an earlier guard that used to split on these characters: a
// ':' '/' '.' ',' sitting BETWEEN two ASCII digits is a STRUCTURAL separator
// inside one token -- colon time ("6:45"), slash fraction ("1/2"), decimal
// quantity ("5.5") -- never junk punctuation to silently drop. The earlier
// behavior (this same digit-flanked test, but used by nl_tokenize_raw below
// to SPLIT '.'/','  into a boundary, same as a hyphen) is gone: the
// separator must be PRESERVED INSIDE the token instead (never split), so a
// colon/slash pair's two halves are never mistaken for an unrelated
// two-number-group utterance, and decimal handling now does a real
// decimal-quantity read (parse_decimal, below) that no longer depends on a
// split at all. This check is therefore read at the nl_normalize
// per-character level (below), NOT nl_tokenize_raw -- none of ':' '/' '.'
// ',' are token-boundary bytes, so "6:45"/"1/2"/"5.5" already survive
// nl_tokenize_raw's raw-substring split whole (is_boundary_byte doesn't
// include any of them, and is_edge_punct only trims a LEADING/TRAILING
// occurrence, never a digit-flanked interior one) -- normalize is the only
// place that used to drop the interior character, so it is the only place
// that needs to stop. `i == 0` / `i + 1 >= len` return false defensively
// (no neighbor to check).
static bool is_digit_flanked_symbol(const char *s, size_t i, size_t len) {
  unsigned char c = (unsigned char)s[i];
  if (c != ':' && c != '/' && c != '.' && c != ',') return false;
  if (i == 0 || i + 1 >= len) return false;
  unsigned char prev = (unsigned char)s[i - 1];
  unsigned char next = (unsigned char)s[i + 1];
  return prev >= '0' && prev <= '9' && next >= '0' && next <= '9';
}

static bool is_all_digits(const char *s) {
  if (s == NULL || s[0] == '\0') return false;
  for (const char *p = s; *p != '\0'; p++) {
    if (*p < '0' || *p > '9') return false;
  }
  return true;
}

// Clamped digit-string -> value; never overflows (input is never more than
// NL_TOKEN_MAX-1 digits, but this guards against a huge run regardless).
// Hand-rolled rather than strtol(): keeps this module's only libc surface to
// <string.h>'s pure byte-buffer helpers (memcpy/memset/strlen/strcmp/
// strncpy), never touching libc's locale/errno-bearing numeric-conversion
// path, which has nothing to offer a fixed '0'-'9' digit run anyway.
static int32_t parse_digits(const char *s) {
  int32_t v = 0;
  for (const char *p = s; *p != '\0'; p++) {
    if (v > 999999) { v = 999999; break; }
    v = v * 10 + (*p - '0');
  }
  if (v > 999999) v = 999999;
  return v;
}

// -- nl_normalize (public) -------------------------------------------------

void nl_normalize(const char *in, char *out, size_t out_size) {
  if (out == NULL || out_size == 0) return;
  if (in == NULL) {
    out[0] = '\0';
    return;
  }

  size_t in_len = strlen(in);
  size_t oi = 0;
  bool pending_space = false;
  bool have_content = false;
  size_t i = 0;

  while (in[i] != '\0') {
    unsigned char c = (unsigned char)in[i];
    if (c < 0x80) {
      if (is_ascii_alnum(c)) {
        if (pending_space && have_content && oi + 1 < out_size) out[oi++] = ' ';
        pending_space = false;
        if (oi + 1 < out_size) out[oi++] = (char)ascii_lower(c);
        have_content = true;
      } else if (is_boundary_byte(c)) {
        pending_space = true;
      } else if (is_digit_flanked_symbol(in, i, in_len)) {
        // Preserve ':' '/' '.' ',' between two digits as part of the SAME
        // token -- CLS_COLONTIME/CLS_SLASHFRAC/CLS_DECIMAL (below) need the
        // separator intact to
        // classify and parse it. `pending_space` is always already false
        // here (reaching this branch requires in[i-1] to be a digit, which
        // only the is_ascii_alnum branch above can have just emitted, and
        // that branch always clears pending_space itself).
        if (oi + 1 < out_size) out[oi++] = (char)c;
        have_content = true;
      }
      // else: other ASCII punctuation -- dropped silently, no boundary
      i++;
    } else if (c == 0xC3 && i + 1 < in_len) {
      bool matched;
      char folded = fold_latin1_supplement((unsigned char)in[i + 1], &matched);
      if (matched) {
        if (pending_space && have_content && oi + 1 < out_size) out[oi++] = ' ';
        pending_space = false;
        if (oi + 1 < out_size) out[oi++] = folded;
        have_content = true;
      }
      i += 2;
    } else {
      // Unexpected/unsupported UTF-8 -- skip defensively, never crash.
      i++;
    }
  }
  out[oi] = '\0';
}

// -- Tokenizer --------------------------------------------------------------

// Splits `s` on whitespace/hyphen boundaries into raw substrings (original
// bytes, edge punctuation trimmed), capped at `max_tokens`. Never crashes;
// silently truncates the remainder of the transcript past the cap. This no
// longer also splits on a digit-flanked '.'/',' (an earlier behavior) --
// ':' '/' '.' ',' between two digits now survive whole into ONE raw token
// (none of them are boundary bytes), and nl_normalize (above) is what
// decides whether to preserve or drop the separator character;
// is_edge_punct's existing leading/trailing trim is unaffected either way.
static int nl_tokenize_raw(const char *s, char tokens[][NL_TOKEN_MAX], int max_tokens) {
  int count = 0;
  size_t i = 0;

  while (s[i] != '\0' && count < max_tokens) {
    while (s[i] != '\0' && is_boundary_byte((unsigned char)s[i])) i++;
    if (s[i] == '\0') break;

    size_t start = i;
    while (s[i] != '\0' && !is_boundary_byte((unsigned char)s[i])) i++;
    size_t end = i;

    while (end > start && is_edge_punct((unsigned char)s[end - 1])) end--;
    while (start < end && is_edge_punct((unsigned char)s[start])) start++;

    if (end > start) {
      size_t tok_len = end - start;
      if (tok_len >= (size_t)NL_TOKEN_MAX) tok_len = NL_TOKEN_MAX - 1;
      memcpy(tokens[count], s + start, tok_len);
      tokens[count][tok_len] = '\0';
      count++;
    }
  }
  return count;
}

// -- Vocab lookups ------------------------------------------------------

// A NUL-separated byte blob replaces an earlier `const char *const[]`
// pointer-array representation for every closed vocab list in this file --
// same O(total chars) linear scan cost as the old exact_match()'s
// pointer-array walk, but drops that array's own N*4-byte pointer table
// (measured ~51% realization across a 3-table/94-entry sample); applied here
// to all remaining vocab tables to help clear the hard 64KB process-image
// size wall. `blob_size` is `sizeof(...)` on the blob array (includes the
// compiler-appended final NUL, exactly like ARRLEN's own sizeof-based count
// did for the old pointer arrays).
static bool exact_match_blob(const char *tok, const char *blob, size_t blob_size) {
  size_t i = 0;
  while (i < blob_size) {
    if (strcmp(tok, blob + i) == 0) return true;
    i += strlen(blob + i) + 1;
  }
  return false;
}

// Shared walker backing NUMBER_WORDS_BLOB/UNIT_WORDS_BLOB/HOMOPHONE_BLOB's
// parallel small-int arrays -- same linear scan as exact_match_blob, but
// also returns the 0-based ordinal position of the match (the index into
// the parallel value array), or -1 if not found.
static int blob_find_index(const char *tok, const char *blob, size_t blob_size) {
  size_t i = 0;
  int idx = 0;
  while (i < blob_size) {
    if (strcmp(tok, blob + i) == 0) return idx;
    i += strlen(blob + i) + 1;
    idx++;
  }
  return -1;
}

static bool number_word_exact(const char *tok, int *val) {
  int idx = blob_find_index(tok, NUMBER_WORDS_BLOB, sizeof(NUMBER_WORDS_BLOB));
  if (idx < 0 || idx >= ARRLEN(NUMBER_VALUES)) return false;
  *val = NUMBER_VALUES[idx];
  return true;
}

static bool unit_word_exact(const char *tok, int32_t *mult) {
  int idx = blob_find_index(tok, UNIT_WORDS_BLOB, sizeof(UNIT_WORDS_BLOB));
  if (idx < 0 || idx >= ARRLEN(UNIT_MULT_IDX)) return false;
  *mult = UNIT_MULTS[UNIT_MULT_IDX[idx]];
  return true;
}

// A single-token NUMBER immediately followed by a bare fraction word
// (HALF_WORDS/QUARTER_WORDS) combines two distinct, equally legitimate ways
// depending on what sits between them:
//   - Multiplicative, NO connective at all ("tres cuartos"=3/4, "veinte
//     cuartos"=20/4, and by the same shape "veinte medios"=20/2): the
//     fraction word is a plural-count unit of its own denominator.
//   - Additive, separated by a CLS_JOINER (optionally with CLS_FILLER glue
//     on either side -- "veinte y cuarto"=20+1/4, "cinco y medio"=5+1/2,
//     "five and a half"=5+1/2, "twenty and a half"=20+1/2): this is the
//     order that was originally completely unhandled -- the old code had no
//     NUMBER+HALF combination at all, so a bare HALF word matched alone
//     regardless of what preceded it, silently dropping the leading number.
// The JOINER's presence/absence is what distinguishes the two readings, not
// an arbitrary choice, so both HALF and QUARTER get exactly the same
// handling here (one shared path) rather than QUARTER having a special case
// HALF lacks. Multi-token numerators (run != 1) are declined (fail-soft) for
// both readings, matching the pre-existing "un cuarto"/"tres cuartos"
// convention this generalizes from -- a compound number run directly
// against a fraction word is rare enough, and ambiguous enough, that
// guessing isn't worth it.
static bool match_number_fraction_at(const NlToken tokens[], const char *const normptrs[],
                                      int count, int idx, const char *frac_blob,
                                      size_t frac_blob_size, int32_t den, int32_t *num,
                                      int *n_tokens) {
  if (tokens[idx].cls != CLS_NUMBER) return false;
  int run = 0;
  int32_t base = nl_words_to_number((const char *const *)&normptrs[idx], count - idx, &run);
  if (run != 1) return false;

  int after = idx + 1;

  // Multiplicative: the fraction word sits immediately after the number,
  // no connective of any kind.
  if (after < count && exact_match_blob(tokens[after].norm, frac_blob, frac_blob_size)) {
    *num = base;
    *n_tokens = 2;
    return true;
  }

  // Additive: a CLS_JOINER (with optional CLS_FILLER glue around it) sits
  // between the number and the fraction word -- "N and a HALF/QUARTER".
  int j = after;
  bool saw_joiner = false;
  while (j < count && (tokens[j].cls == CLS_FILLER || tokens[j].cls == CLS_JOINER)) {
    if (tokens[j].cls == CLS_JOINER) saw_joiner = true;
    j++;
  }
  if (saw_joiner && j < count && exact_match_blob(tokens[j].norm, frac_blob, frac_blob_size)) {
    *num = base * den + 1;
    *n_tokens = (j - idx) + 1;
    return true;
  }

  return false;
}

// Detects a fraction QUANTITY starting at token `idx` -- a standalone HALF
// word (-> 1/2, 1 token: "medio"/"media"/"half"), a standalone QUARTER word
// (-> 1/4, 1 token: "cuarto"/"cuartos"/"quarter"/"quarters"), or a
// single-token NUMBER combined with a HALF or QUARTER word per
// match_number_fraction_at above (multiplicative when adjacent, additive
// when joined by "y"/"and" -- "un cuarto"=1/4, "tres cuartos"=3/4, "veinte y
// cuarto"=20+1/4, "cinco y medio"=5+1/2, "five and a half"=5+1/2). This is
// the "un cuarto vs un=1" precedence handled structurally: a QUARTER word
// never appears in this app's grammar except directly after its own
// optional numerator, so there is no other legitimate reading for a NUMBER
// immediately followed by one to lose. Does NOT consume/mutate any token --
// the caller marks tokens consumed only once the whole pattern (fraction +
// unit) is confirmed, so a fraction word with no following unit (the
// deferred, unit-less forms -- shipped here as fail-soft) simply falls
// through unclaimed, same as any other unrecognized CLS_WORD.
static bool match_fraction_at(const NlToken tokens[], const char *const normptrs[], int count,
                               int idx, int32_t *num, int32_t *den, int *n_tokens) {
  if (idx >= count) return false;
  if (exact_match_blob(tokens[idx].norm, HALF_WORDS_BLOB, sizeof(HALF_WORDS_BLOB))) {
    *num = 1;
    *den = 2;
    *n_tokens = 1;
    return true;
  }
  if (exact_match_blob(tokens[idx].norm, QUARTER_WORDS_BLOB, sizeof(QUARTER_WORDS_BLOB))) {
    *num = 1;
    *den = 4;
    *n_tokens = 1;
    return true;
  }
  if (match_number_fraction_at(tokens, normptrs, count, idx, QUARTER_WORDS_BLOB,
                                sizeof(QUARTER_WORDS_BLOB), 4, num, n_tokens)) {
    *den = 4;
    return true;
  }
  if (match_number_fraction_at(tokens, normptrs, count, idx, HALF_WORDS_BLOB,
                                sizeof(HALF_WORDS_BLOB), 2, num, n_tokens)) {
    *den = 2;
    return true;
  }
  return false;
}

// The clock HH[:MM] sub-grammar (subtractive shapes via the shape_a/shape_b
// wrappers below, arbitrary post-hour minutes, meridiem completion +
// clock-hedge tolerance, midnight/noon flip), starting at token `start`
// (immediately after the discriminator's preposition/article, a bare
// noon/midnight noun, or a CLS_COLONTIME token -- match_clock_target and
// the shape_a/shape_b wrappers below are the only callers). Core set: bare
// "noon"/"mediodia"->12:00, "midnight"/"medianoche"->00:00; a CLS_COLONTIME
// token ("6:45"/"18:30") read directly as HH:MM (a triple/seconds-bearing
// colon declines, no seconds field on a clock target); `H` (-> H:00),
// `H MM` (a second adjacent number group, 0-59), `H y MM` (a
// joiner-then-plain-NUMBER minute, generalized beyond cuarto/media),
// `H y media`/`H y cuarto` (a HALF/QUARTER word, optionally through ONE
// joiner), a trailing "y pico"/"y algo"/"y monedas" hedge (consumed,
// dropped, the already-resolved target kept exactly as-is), then an
// optional am/pm marker (en "am"/"pm"; es "de la manana/tarde/noche", the
// leading "de"/"la"/"el" skipped inline here -- deliberately NOT a
// FILLER_WORDS/CLOCK_ART addition, so this skip can never affect the
// clause-scoped kind-resolution walk or the ordinary duration grammar's
// residue) -- the PM branch
// has its own midnight fix: "doce" + a "noche" marker specifically is
// midnight (hour=0), not the general (hour%12)+12 formula's noon (the AM
// branch's own hour==12->0 special case is unchanged/already correct).
// `*had_meridiem` (out) is true ONLY when an actual meridiem WORD (or a
// noon/midnight noun) was consumed here -- never merely because hour ended
// up 24h-known via a direct 13-23/0 range or a bare colon -- this is the
// one signal match_bare_meridiem_clock (below) can safely gate on without
// falsely promoting an ordinary duration number ("20 minutes") into a
// clock target just because 20 happens to be in the 13-23 range. Deferred
// still: "en punto"/"sharp" as a standalone marker (the "o'clock" cue used
// by match_alarm_cue_clock is a different, narrower thing).
// Returns false if `start` isn't a NUMBER/COLONTIME token in the valid
// ranges and isn't a recognized noon/midnight noun.
// Forward declaration: parse_colon_time itself lives further down (with its
// CLS_SLASHFRAC/CLS_DECIMAL siblings, in the duration-shapes section) but the
// clock sub-grammar needs it here too, to route a CLS_COLONTIME token
// directly as HH:MM.
static bool parse_colon_time(const char *norm, int32_t *g1, int32_t *g2, int32_t *g3, int *arity);

// A FUSED digit+meridiem token -- "8pm" (a real observed phrasing), "12am" --
// with no space between the digit(s) and the suffix. nl_tokenize_raw never
// splits these (no
// whitespace/hyphen boundary between the digit and the letters), and
// nl_normalize doesn't either (only a digit-flanked ':'/'/'/'.'/'.' is
// preserved as a joiner -- 'p'/'m' aren't digits), so the whole thing
// survives as ONE token, classified CLS_WORD (it isn't all-digits, isn't
// any listed vocab word). Recognized directly here on the token's own norm
// text, rather than by teaching the tokenizer a new split rule -- exactly
// one shape (1-2 digits + "am"/"pm", nothing else), so a real CLS_WORD like
// "cream"/"exam" (which also end in "am") never matches (their prefix isn't
// all-digit).
static bool parse_fused_digit_meridiem(const char *norm, int32_t *hour, bool *is_pm) {
  size_t len = strlen(norm);
  if (len < 3 || len > 4) return false;  // "Npm"/"NNam" -- 1-2 digit hour + 2-letter suffix
  const char *suffix = norm + len - 2;
  bool pm;
  if (strcmp(suffix, "pm") == 0) {
    pm = true;
  } else if (strcmp(suffix, "am") == 0) {
    pm = false;
  } else {
    return false;
  }
  size_t ndigits = len - 2;
  int32_t h = 0;
  for (size_t i = 0; i < ndigits; i++) {
    if (norm[i] < '0' || norm[i] > '9') return false;
    h = h * 10 + (norm[i] - '0');
  }
  if (h < 1 || h > 12) return false;  // the fused form is always a 12h spoken hour
  *hour = h;
  *is_pm = pm;
  return true;
}

static bool match_clock_expr(const NlToken tokens[], const char *const normptrs[], int count,
                              int start, NlClockTarget *out, int *end_idx, bool *had_meridiem) {
  *had_meridiem = false;
  if (start >= count) return false;

  // Bare noon/midnight nouns resolve on their own, no
  // hour/minute grammar needed at all.
  if (exact_match_blob(tokens[start].norm, CLOCK_NOON_WORDS_BLOB, sizeof(CLOCK_NOON_WORDS_BLOB))) {
    out->has_clock_target = true;
    out->hour = 12;
    out->minute = 0;
    out->hour24_known = true;
    *had_meridiem = true;
    *end_idx = start;
    return true;
  }
  if (exact_match_blob(tokens[start].norm, CLOCK_MIDNIGHT_WORDS_BLOB, sizeof(CLOCK_MIDNIGHT_WORDS_BLOB))) {
    out->has_clock_target = true;
    out->hour = 0;
    out->minute = 0;
    out->hour24_known = true;
    *had_meridiem = true;
    *end_idx = start;
    return true;
  }

  int32_t hour;
  int32_t minute = 0;
  bool explicit_minute = false;
  int after;

  if (tokens[start].cls == CLS_COLONTIME) {
    // Colon-in-clock routing: "6:45"/"18:30" read directly as
    // HH:MM -- a wall clock has no seconds field, so a triple (arity==3)
    // declines this shape entirely (matches the duration-routing block's
    // own documented "not this block's concern" note).
    int32_t g1, g2, g3;
    int arity;
    if (!parse_colon_time(tokens[start].norm, &g1, &g2, &g3, &arity)) return false;
    if (arity != 2) return false;
    if (g1 < 0 || g1 > 23 || g2 < 0 || g2 > 59) return false;
    hour = g1;
    minute = g2;
    explicit_minute = true;
    after = start + 1;
  } else if (tokens[start].cls == CLS_NUMBER) {
    int run = 0;
    hour = nl_words_to_number((const char *const *)&normptrs[start], count - start, &run);
    if (run <= 0 || hour < 0 || hour > 23) return false;
    after = start + run;

    if (after < count && tokens[after].cls == CLS_NUMBER) {
      int run2 = 0;
      int32_t m = nl_words_to_number((const char *const *)&normptrs[after], count - after, &run2);
      if (run2 > 0 && m >= 0 && m <= 59) {
        minute = m;
        explicit_minute = true;
        after += run2;
      }
    }

    if (!explicit_minute) {
      // Skip a full run of CLS_FILLER|CLS_JOINER tokens here, not just a
      // single optional JOINER -- the natural English idiom is "six AND A
      // half" (JOINER "and" then FILLER "a"), and a single-JOINER-only skip
      // left the FILLER "a" unconsumed, so exact_match against HALF_WORDS/
      // QUARTER_WORDS never hit and the whole clock expr fell back to
      // minute=0 with "half"/"quarter" leaking into residue -- a
      // silently-wrong 30-minute-early target with only a stray word as
      // the sole hint. Mirrors match_number_fraction_at's own
      // already-correct loop for the exact same shape on the duration
      // grammar's fraction engine.
      int j = after;
      while (j < count && (tokens[j].cls == CLS_FILLER || tokens[j].cls == CLS_JOINER)) j++;
      if (j < count && exact_match_blob(tokens[j].norm, HALF_WORDS_BLOB, sizeof(HALF_WORDS_BLOB))) {
        minute = 30;
        after = j + 1;
      } else if (j < count && exact_match_blob(tokens[j].norm, QUARTER_WORDS_BLOB, sizeof(QUARTER_WORDS_BLOB))) {
        minute = 15;
        after = j + 1;
      } else if (j < count && tokens[j].cls == CLS_NUMBER) {
        // A plain NUMBER (not just cuarto/media) after the joiner run is
        // the minute too -- "a las
        // diez y veinte"->10:20. Guard 0..59; out of range declines the
        // minute (stays 0) and leaves the number visible/unconsumed, never
        // guesses (matches the "diez sesenta" group>59 lock).
        int run3 = 0;
        int32_t m3 = nl_words_to_number((const char *const *)&normptrs[j], count - j, &run3);
        if (run3 > 0 && m3 >= 0 && m3 <= 59) {
          minute = m3;
          explicit_minute = true;
          after = j + run3;
        }
      }
    }
  } else if (tokens[start].cls == CLS_WORD) {
    // A fused digit+meridiem token ("8pm", "12am") -- resolves and
    // consumes on its own, no further minute/meridiem grammar to check
    // (the meridiem is already baked into the one token).
    int32_t fh;
    bool fpm;
    if (!parse_fused_digit_meridiem(tokens[start].norm, &fh, &fpm)) return false;
    hour = fpm ? ((fh % 12) + 12) : (fh == 12 ? 0 : fh);
    minute = 0;
    explicit_minute = true;
    after = start + 1;
    out->has_clock_target = true;
    out->hour = (int8_t)hour;
    out->minute = 0;
    out->hour24_known = true;
    *had_meridiem = true;
    *end_idx = start;
    return true;
  } else {
    return false;
  }

  // Clock-hedge tolerance: a trailing "y pico"/"y algo"/
  // "y monedas" is consumed and dropped -- the already-resolved hour/minute
  // is kept exactly as-is (no unambiguous nuance to add), so the hedge
  // neither leaks into the name nor derails the meridiem check just below.
  {
    int j = after;
    bool saw_joiner = j < count && tokens[j].cls == CLS_JOINER;
    if (saw_joiner) j++;
    if (saw_joiner && j < count &&
        exact_match_blob(tokens[j].norm, CLOCK_HEDGE_TAIL_WORDS_BLOB, sizeof(CLOCK_HEDGE_TAIL_WORDS_BLOB))) {
      after = j + 1;
    }
  }

  // A direct 24h-range word ("las dieciocho"=18, a digit "18"/"0") or a
  // colon token is unambiguous on its own; an am/pm marker (checked next)
  // can still override/confirm it for the 1-12 range.
  bool hour24_known = hour == 0 || hour >= 13;

  int m_idx = after;
  if (m_idx < count && strcmp(tokens[m_idx].norm, "de") == 0) m_idx++;
  if (m_idx < count && (strcmp(tokens[m_idx].norm, "la") == 0 || strcmp(tokens[m_idx].norm, "el") == 0)) {
    m_idx++;
  }
  if (m_idx < count && exact_match_blob(tokens[m_idx].norm, CLOCK_PM_WORDS_BLOB, sizeof(CLOCK_PM_WORDS_BLOB))) {
    // "doce de la noche" is midnight (hour=0), not the general
    // (hour%12)+12 formula's noon -- narrowly
    // scoped to the "noche" marker word specifically ("doce de la tarde"
    // stays noon, unaffected -- (12%12)+12=12).
    bool is_noche = strcmp(tokens[m_idx].norm, "noche") == 0;
    if (hour == 12 && is_noche) {
      hour = 0;
    } else if (hour >= 1 && hour <= 12) {
      hour = (hour % 12) + 12;
    }
    hour24_known = true;
    *had_meridiem = true;
    after = m_idx + 1;
  } else if (m_idx < count && exact_match_blob(tokens[m_idx].norm, CLOCK_AM_WORDS_BLOB, sizeof(CLOCK_AM_WORDS_BLOB))) {
    if (hour == 12) hour = 0;
    hour24_known = true;
    *had_meridiem = true;
    after = m_idx + 1;
  }

  if (minute < 0 || minute > 59) return false;  // defensive; structurally unreachable given the guards above

  out->has_clock_target = true;
  out->hour = (int8_t)hour;
  out->minute = (int8_t)minute;
  out->hour24_known = hour24_known;
  *end_idx = after - 1;
  return true;
}

// The subtractive operand -- an optional leading bare "menos" (the "menos
// cuarto para las diez" reversed-phrasing
// idiom, where "menos cuarto" together just means "quarter to", the leading
// "menos" carrying no separate arithmetic meaning of its own), then either
// a QUARTER word alone (->15), "un"/"una"/"uno" + QUARTER (->15, the same
// "un cuarto" the fraction engine already knows, disambiguated here by the
// TO-glue the caller requires immediately after), or a plain NUMBER (1..59
// -- HALF_WORDS/"media" is never CLS_NUMBER, so it can never reach this
// branch; a "media=30" reading is illegal here and is declined simply by
// falling through unclaimed).
static bool parse_subtractive_n_expr(const NlToken tokens[], const char *const normptrs[],
                                      int count, int idx, int32_t *n_val, int *n_tokens) {
  int i = idx;
  if (i < count && strcmp(tokens[i].norm, "menos") == 0) i++;

  if (i < count && exact_match_blob(tokens[i].norm, QUARTER_WORDS_BLOB, sizeof(QUARTER_WORDS_BLOB))) {
    *n_val = 15;
    *n_tokens = (i - idx) + 1;
    return true;
  }

  if (i < count && tokens[i].cls == CLS_NUMBER) {
    int run = 0;
    int32_t v = nl_words_to_number((const char *const *)&normptrs[i], count - i, &run);
    if (run == 1 && v == 1 && i + 1 < count &&
        exact_match_blob(tokens[i + 1].norm, QUARTER_WORDS_BLOB, sizeof(QUARTER_WORDS_BLOB))) {
      *n_val = 15;
      *n_tokens = (i - idx) + 2;
      return true;
    }
    if (run > 0 && v >= 1 && v <= 59) {
      *n_val = v;
      *n_tokens = (i - idx) + run;
      return true;
    }
  }

  return false;
}

// Subtractive shape A: "[menos]? (cuarto|quarter | N |
// un/una/uno cuarto) <TO-glue> <HOUR-EXPR>[meridiem]" -- en TO-glue is "to"
// (only valid when `allow_en_to`, i.e. an outer EN preposition already
// established clock context -- match_clock_expr_ext's own caller, below);
// es TO-glue is "para" immediately followed by "las"/"la" (self-
// establishing, no outer context needed -- match_subtractive_a_bare, below,
// is the independent scan that relies on this). The HOUR-EXPR (+ its own
// optional meridiem) is parsed by recursing into match_clock_expr itself --
// exactly the shape it already handles -- so a meridiem on the STATED hour
// ("un cuarto para las tres de la tarde"->14:45, tres=15 via PM, THEN
// quarter-to) is resolved before the subtraction, for free.
//
// `had_meridiem_out` (optional, NULL-able): reports whether the recursed
// HOUR-EXPR itself resolved a trailing meridiem word (ct2's own
// had_meridiem). match_subtractive_a_bare (below) uses this to gate its
// own widened EN-"to" retry -- a trailing meridiem is at least as strong a
// context signal as an explicit "at", so it's the discriminator that lets
// "ten to six pm"/"quarter to six pm" resolve while "ten to six" alone (no
// meridiem, no prep) still correctly refuses.
static bool match_subtractive_shape_a(const NlToken tokens[], const char *const normptrs[],
                                       int count, int idx, bool allow_en_to, NlClockTarget *out,
                                       int *end_idx, bool *had_meridiem_out) {
  int32_t n_val;
  int n_tokens;
  if (!parse_subtractive_n_expr(tokens, normptrs, count, idx, &n_val, &n_tokens)) return false;

  int glue_start = idx + n_tokens;
  int hour_start;
  if (allow_en_to && glue_start < count && strcmp(tokens[glue_start].norm, "to") == 0) {
    hour_start = glue_start + 1;
  } else if (glue_start + 1 < count && strcmp(tokens[glue_start].norm, "para") == 0 &&
             (strcmp(tokens[glue_start + 1].norm, "las") == 0 ||
              strcmp(tokens[glue_start + 1].norm, "la") == 0)) {
    hour_start = glue_start + 2;
  } else {
    return false;
  }

  NlClockTarget ct2 = {0};
  int end2 = -1;
  bool had_mer2 = false;
  if (!match_clock_expr(tokens, normptrs, count, hour_start, &ct2, &end2, &had_mer2)) return false;
  if (had_meridiem_out != NULL) *had_meridiem_out = had_mer2;

  int32_t new_hour =
      ct2.hour24_known ? (ct2.hour - 1 + 24) % 24 : (ct2.hour == 1 ? 12 : ct2.hour - 1);
  int32_t new_minute = 60 - n_val;
  if (new_minute <= 0 || new_minute > 59) return false;  // defensive; n_val 1..59 guarantees this

  out->has_clock_target = true;
  out->hour = (int8_t)new_hour;
  out->minute = (int8_t)new_minute;
  out->hour24_known = ct2.hour24_known;
  *end_idx = end2;
  return true;
}

// Subtractive shape B: "<HOUR-EXPR> menos <N-expr>
// [meridiem]" -- always reached via an outer "las"/"la" (+ optional "a"/
// "hasta" glue), since H itself is introduced by the ordinary clock-article
// grammar. Unlike shape A, a trailing meridiem here describes the FINAL
// (post-subtraction) instant, not H itself ("...menos veinte de la
// manana"->5:40 AM, not "6 AM minus 20") -- so it is applied to `new_hour`
// AFTER the subtraction, not folded into the recursive match_clock_expr
// call the way shape A folds it into H.
static bool match_subtractive_shape_b(const NlToken tokens[], const char *const normptrs[],
                                       int count, int idx, NlClockTarget *out, int *end_idx) {
  NlClockTarget ct1 = {0};
  int end1 = -1;
  bool had_mer1 = false;
  if (!match_clock_expr(tokens, normptrs, count, idx, &ct1, &end1, &had_mer1)) return false;
  if (had_mer1) return false;  // a meridiem before "menos" isn't an attested shape -- let the plain reading stand
  // Mirrors the had_mer1 guard just above --
  // H already carrying its OWN explicit minute (colon "6:45", "y veinte",
  // "y media"/"y cuarto") before "menos N" is a self-contradictory/self-
  // correcting utterance ("at six twenty... no, quarter to six"), not the
  // attested "<bare HOUR> menos <N>" shape (that shape's H is always a bare
  // hour, minute defaulting to 0). Applying `60 - n_val` on top of an
  // already-stated minute silently discards it (ct1.minute was thrown away
  // entirely, replaced by unrelated arithmetic) -- decline shape B here too
  // and let the plain match_clock_expr reading stand (H:ct1.minute, "menos
  // N..." left as residue/name), same posture as the had_mer1 case.
  if (ct1.minute != 0) return false;

  int menos_idx = end1 + 1;
  if (menos_idx >= count || strcmp(tokens[menos_idx].norm, "menos") != 0) return false;

  int32_t n_val;
  int n_tokens;
  if (!parse_subtractive_n_expr(tokens, normptrs, count, menos_idx + 1, &n_val, &n_tokens)) {
    return false;
  }

  int after = menos_idx + 1 + n_tokens;
  int m_idx = after;
  bool matched_pm = false, matched_am = false, is_noche = false;
  if (m_idx < count && strcmp(tokens[m_idx].norm, "de") == 0) m_idx++;
  if (m_idx < count && (strcmp(tokens[m_idx].norm, "la") == 0 || strcmp(tokens[m_idx].norm, "el") == 0)) {
    m_idx++;
  }
  if (m_idx < count && exact_match_blob(tokens[m_idx].norm, CLOCK_PM_WORDS_BLOB, sizeof(CLOCK_PM_WORDS_BLOB))) {
    matched_pm = true;
    is_noche = strcmp(tokens[m_idx].norm, "noche") == 0;
    after = m_idx + 1;
  } else if (m_idx < count && exact_match_blob(tokens[m_idx].norm, CLOCK_AM_WORDS_BLOB, sizeof(CLOCK_AM_WORDS_BLOB))) {
    matched_am = true;
    after = m_idx + 1;
  }

  int32_t new_hour;
  bool hour24_known;
  if (ct1.hour24_known) {
    new_hour = (ct1.hour - 1 + 24) % 24;
    hour24_known = true;
  } else {
    new_hour = (ct1.hour == 1) ? 12 : ct1.hour - 1;
    hour24_known = false;
  }
  int32_t new_minute = 60 - n_val;
  if (new_minute <= 0 || new_minute > 59) return false;

  if (matched_pm) {
    if (new_hour == 12 && is_noche) {
      new_hour = 0;
    } else if (new_hour >= 1 && new_hour <= 12) {
      new_hour = (new_hour % 12) + 12;
    }
    hour24_known = true;
  } else if (matched_am) {
    if (new_hour == 12) new_hour = 0;
    hour24_known = true;
  }

  out->has_clock_target = true;
  out->hour = (int8_t)new_hour;
  out->minute = (int8_t)new_minute;
  out->hour24_known = hour24_known;
  *end_idx = after - 1;
  return true;
}

// Tries the two subtractive shapes above before falling back to the plain
// match_clock_expr grammar -- shape_b first (it only needs a plain hour,
// which match_clock_expr itself
// resolves internally, so trying it costs nothing extra), then shape_a (en
// "to" only valid here, since an outer prep/article already established
// context -- match_subtractive_a_bare, below, is the independent scan that
// handles the ES form needing NO outer context). First-match-wins, same
// convention as everywhere else in this file.
static bool match_clock_expr_ext(const NlToken tokens[], const char *const normptrs[], int count,
                                  int start, NlClockTarget *out, int *end_idx) {
  if (match_subtractive_shape_b(tokens, normptrs, count, start, out, end_idx)) return true;
  if (match_subtractive_shape_a(tokens, normptrs, count, start, /*allow_en_to=*/true, out,
                                 end_idx, /*had_meridiem_out=*/NULL)) {
    return true;
  }
  bool had_meridiem;
  return match_clock_expr(tokens, normptrs, count, start, out, end_idx, &had_meridiem);
}

// The independent, no-outer-context scan for shape A's ES form (see
// match_subtractive_shape_a's own doc comment for why this can't just be
// nested inside the ordinary prep/article loop in match_clock_target,
// below): "un cuarto para las seis"/"veinte para las cinco" have no leading
// preposition/article before their own N-expr at all -- without this
// independent scan, match_clock_target's own "las"/"la" trigger would find
// the embedded "las seis" FIRST and wrongly resolve just the bare hour (the
// very SW bug this family fixes) before ever considering the "un cuarto
// para" prefix that precedes it. Tries every position -- cheap (count <=
// NL_MAX_TOKENS) -- first success wins.
// A match at `p` originally never looked left, so an outer EN prep
// ("at"/"until"/"til"/"till") directly before the bare subtractive shape
// was left unconsumed -- "at ten to six pm" resolves the correct clock but
// leaked name="at". A first fix only covered the EN case; the analogous ES
// "hasta las"/"hasta la" glue (+ the bare "las"/"la" article) is restored
// below once the float->fixed-point size reclaim reopened enough headroom.
static int subtractive_a_bare_prep_start(const NlToken tokens[], int p) {
  if (p > 0 &&
      exact_match_blob(tokens[p - 1].norm, CLOCK_PREP_WORDS_EN_BLOB, sizeof(CLOCK_PREP_WORDS_EN_BLOB))) {
    return p - 1;
  }
  // ES glue: "las"/"la", optionally preceded by "a"/"hasta" (mirrors
  // match_clock_target's own prep/article loop). Cosmetic residue consume
  // only -- clock value already correct.
  if (p > 0 && exact_match_blob(tokens[p - 1].norm, CLOCK_ART_WORDS_ES_BLOB,
                                sizeof(CLOCK_ART_WORDS_ES_BLOB))) {
    if (p > 1 && (strcmp(tokens[p - 2].norm, "a") == 0 || strcmp(tokens[p - 2].norm, "hasta") == 0)) {
      return p - 2;
    }
    return p - 1;
  }
  return p;
}

static bool match_subtractive_a_bare(const NlToken tokens[], const char *const normptrs[],
                                      int count, bool consumed[], NlClockTarget *out) {
  for (int p = 0; p < count; p++) {
    if (consumed[p]) continue;
    NlClockTarget ct = {0};
    int end = -1;
    if (match_subtractive_shape_a(tokens, normptrs, count, p, /*allow_en_to=*/false, &ct, &end,
                                   /*had_meridiem_out=*/NULL)) {
      for (int k = subtractive_a_bare_prep_start(tokens, p); k <= end; k++) consumed[k] = true;
      *out = ct;
      return true;
    }
    // The EN "to" glue form ("ten to six pm", "quarter to six pm") has no
    // leading preposition/article either -- without this,
    // match_bare_meridiem_clock (tried later, in match_clock_target)
    // greedily resolves just the trailing "six pm" on its own, leaving "ten
    // to"/"quarter to" as stray unconsumed name residue and silently
    // landing 10-20 minutes LATE (the exact "rings late" failure class this
    // subtractive family was built to close). Gated strictly on the
    // recursed HOUR-EXPR itself resolving a trailing meridiem
    // (had_meridiem_out) -- a trailing meridiem is at least as strong a
    // context signal as an explicit "at" -- so bare "ten to six" (no
    // meridiem, no prep) still correctly refuses; only the
    // meridiem-qualified compound shape is newly accepted here.
    bool had_meridiem = false;
    if (match_subtractive_shape_a(tokens, normptrs, count, p, /*allow_en_to=*/true, &ct, &end,
                                   &had_meridiem) &&
        had_meridiem) {
      for (int k = subtractive_a_bare_prep_start(tokens, p); k <= end; k++) consumed[k] = true;
      *out = ct;
      return true;
    }
  }
  return false;
}

// A valid clock-expr immediately followed by a meridiem marker is clock
// context EVEN WITH NO PREPOSITION
// ("6:45 pm"->18:45, "5 pm"->17:00, "seis cuarenta y cinco de la
// tarde"->18:45) -- also covers bare "noon"/"midnight"/"mediodia"/
// "medianoche" (match_clock_expr's own preamble sets *had_meridiem for
// those too). Gated STRICTLY on `had_meridiem` (an actual meridiem WORD
// consumed), never on the resulting hour24_known alone -- a bare 13-23
// range number ("eighteen minutes") must never itself become a clock
// target just because 18 happens to be unambiguous; only a literal
// trailing am/pm/noon/midnight word may promote a bare number here.
static bool match_bare_meridiem_clock(const NlToken tokens[], const char *const normptrs[],
                                       int count, bool consumed[], NlClockTarget *out) {
  for (int p = 0; p < count; p++) {
    if (consumed[p]) continue;
    if (tokens[p].cls != CLS_NUMBER && tokens[p].cls != CLS_COLONTIME &&
        tokens[p].cls != CLS_WORD) {
      continue;
    }
    NlClockTarget ct = {0};
    int end = -1;
    bool had_meridiem = false;
    if (match_clock_expr(tokens, normptrs, count, p, &ct, &end, &had_meridiem) && had_meridiem) {
      for (int k = p; k <= end; k++) consumed[k] = true;
      *out = ct;
      return true;
    }
  }
  return false;
}

// The "first two CLS_NUMBER groups" collector -- shared by
// match_alarm_cue_clock (below), the spoken-M:SS pair rule, and the range
// detector (parse_nl's own scratch helpers, further down), all three of
// which need the exact same shape and none of
// which needs a 3rd group's position (a 3rd group is enough on its own to
// reject the two-group shape, so only its COUNT matters past that point).
// `consumed`/`g_val` are both optional (NULL skips that check/output) since
// the three callers don't all need them: the pair rule and range detector
// never check consumed[] here (both run before anything could have
// consumed a NUMBER token), and the range detector doesn't need the actual
// values, only the groups' token spans.
static int collect_first_two_number_groups(const NlToken tokens[], const char *const normptrs[],
                                            int count, const bool consumed[], int g_start[2],
                                            int g_end[2], int32_t g_val[2]) {
  int ngroups = 0;
  int i = 0;
  while (i < count) {
    if ((consumed != NULL && consumed[i]) || tokens[i].cls != CLS_NUMBER) {
      i++;
      continue;
    }
    int run = 0;
    int32_t val = nl_words_to_number((const char *const *)&normptrs[i], count - i, &run);
    if (run <= 0) {
      i++;
      continue;
    }
    if (ngroups < 2) {
      g_start[ngroups] = i;
      g_end[ngroups] = i + run - 1;
      if (g_val != NULL) g_val[ngroups] = val;
    }
    ngroups++;
    i += run;
  }
  return ngroups;
}

// "set an alarm for six thirty" promotes an otherwise-ordinary unit-less
// two-number-group DURATION pair (the exact shape the spoken-M:SS-pair
// rule reads as a duration) to a clock
// target -- but ONLY when an explicit alarm/wake cue (CLOCK_ALARM_CUE_WORDS)
// or a bare "o'clock" token is present somewhere in the utterance. Without
// one, "timer for six thirty" stays the shipped 390s duration -- "for"
// itself is deliberately never promoted to a generic clock preposition
// (that would wrongly turn "for five minutes" into a bogus 5:00 clock
// target, since match_clock_expr accepts ANY 0-23 number with no further
// context). Reuses the exact "no UNIT token anywhere, first two adjacent
// number groups" preconditions the duration pair rule itself uses, so this
// can never touch a case that rule doesn't already consider.
static bool match_alarm_cue_clock(const NlToken tokens[], const char *const normptrs[], int count,
                                   bool consumed[], NlClockTarget *out) {
  bool cue = false;
  for (int i = 0; i < count && !cue; i++) {
    if (exact_match_blob(tokens[i].norm, CLOCK_ALARM_CUE_WORDS_BLOB, sizeof(CLOCK_ALARM_CUE_WORDS_BLOB))) {
      cue = true;
    }
    if (!cue && strcmp(tokens[i].norm, "oclock") == 0) cue = true;
    if (!cue && i + 1 < count && strcmp(tokens[i].norm, "esta") == 0 &&
        strcmp(tokens[i + 1].norm, "noche") == 0) {
      cue = true;
    }
  }
  if (!cue) return false;

  for (int i = 0; i < count; i++) {
    if (tokens[i].cls == CLS_UNIT) return false;
  }

  int g_start[2] = {-1, -1}, g_end[2] = {-1, -1};
  int32_t g_val[2] = {0, 0};
  int ngroups =
      collect_first_two_number_groups(tokens, normptrs, count, consumed, g_start, g_end, g_val);
  if (ngroups != 2) return false;

  bool adjacent = true;
  for (int k = g_end[0] + 1; k < g_start[1]; k++) {
    if (tokens[k].cls != CLS_JOINER && tokens[k].cls != CLS_FILLER) {
      adjacent = false;
      break;
    }
  }
  if (!adjacent) return false;
  if (g_val[0] < 0 || g_val[0] > 23 || g_val[1] < 0 || g_val[1] > 59) return false;

  out->has_clock_target = true;
  out->hour = (int8_t)g_val[0];
  out->minute = (int8_t)g_val[1];
  out->hour24_known = (g_val[0] == 0 || g_val[0] >= 13);
  for (int k = g_start[0]; k <= g_end[1]; k++) consumed[k] = true;
  return true;
}

// "5 and 6 pm" / "5 y 6 pm" / "5 6 pm" -- two independent,
// equally-plausible hour NUMBERs (optionally joined by a JOINER, or with no
// connective at all) immediately followed by a single trailing meridiem
// word collapse, unguarded, into a FABRICATED specific HH:MM: the "a plain
// NUMBER after the hour is the minute too" read (added for "diez y
// veinte"->10:20, a genuine hour+minute) can't tell that
// shape apart from two INDEPENDENT hour mentions sharing one meridiem --
// "5 and 6 pm" bound the second "6" as if it were ":06" and produced
// 17:06. A real user saying this means genuine uncertainty ("wake me at 5
// or 6") and must REFUSE, never guess an endpoint.
//
// Checked ONCE, up front, and vetoes match_clock_target's ENTIRE search
// (every sub-strategy below -- bare subtractive, prep/article loop,
// bare-meridiem scan, alarm-cue) the same way magnitude_adjacent/
// hundred_present veto the whole duration family (parse_nl, further down):
// declining the minute-bind at just ONE sub-matcher isn't enough, since a
// LATER sub-matcher (e.g. match_bare_meridiem_clock scanning forward to the
// second bare number + meridiem on its own) would otherwise still resolve
// its own, different, equally-fabricated answer.
//
// Shape: an unconsumed NUMBER in [0,23] (a plausible hour), optionally
// followed by one JOINER, then a SECOND unconsumed NUMBER in [1,12]
// (plausible as a stand-alone hour -- 13-59 stays unambiguously "a minute",
// e.g. "ten twenty pm" is untouched, matches only trip in the narrow band
// where the second number really could be either), then optionally
// "de"/"la"/"el", then a literal PM/AM word.
static bool two_hour_pair_ambiguous(const NlToken tokens[], const char *const normptrs[],
                                     int count) {
  for (int i = 0; i < count; i++) {
    if (tokens[i].cls != CLS_NUMBER) continue;
    int run1 = 0;
    int32_t h1 = nl_words_to_number((const char *const *)&normptrs[i], count - i, &run1);
    if (run1 <= 0 || h1 < 0 || h1 > 23) continue;

    int j = i + run1;
    // "o"/"or" (Spanish/English "or", e.g. "a las 3 o 4 de la tarde", "5 or
    // 6 pm") are never classified JOINER (only "y"/"and" are) but signal the
    // identical hour-or-hour ambiguity -- checked by literal norm text, same
    // as this function's own "de"/"la"/"el" meridiem-hedge skip just below.
    if (j < count && (tokens[j].cls == CLS_JOINER || strcmp(tokens[j].norm, "o") == 0 ||
                       strcmp(tokens[j].norm, "or") == 0)) {
      j++;
    }
    if (j >= count || tokens[j].cls != CLS_NUMBER) continue;

    int run2 = 0;
    int32_t h2 = nl_words_to_number((const char *const *)&normptrs[j], count - j, &run2);
    if (run2 <= 0 || h2 < 1 || h2 > 12) continue;

    int m_idx = j + run2;
    if (m_idx < count && strcmp(tokens[m_idx].norm, "de") == 0) m_idx++;
    if (m_idx < count &&
        (strcmp(tokens[m_idx].norm, "la") == 0 || strcmp(tokens[m_idx].norm, "el") == 0)) {
      m_idx++;
    }
    if (m_idx < count &&
        (exact_match_blob(tokens[m_idx].norm, CLOCK_PM_WORDS_BLOB, sizeof(CLOCK_PM_WORDS_BLOB)) ||
         exact_match_blob(tokens[m_idx].norm, CLOCK_AM_WORDS_BLOB, sizeof(CLOCK_AM_WORDS_BLOB)))) {
      return true;
    }
  }
  return false;
}

// The clock-target DISCRIMINATOR -- a clock target fires iff a clock
// preposition/article immediately precedes
// a valid HH[:MM] expression (match_clock_expr, above). EN: a single prep
// token (at/until/til/till) directly before H. ES: "las"/"la" directly
// before H, optionally preceded by "a"/"hasta" (both consumed as connective
// glue when present -- "a"/"hasta" ALONE, with no las/la, never trigger
// this: bare "hasta seis" stays undetected -- and a bare "la"/"a" elsewhere in an utterance, e.g.
// "la alarma de cinco minutos", never matches either, since match_clock_expr
// requires the VERY NEXT token to be a 0-23 NUMBER, which "alarma" isn't).
// Checked BEFORE any duration rule; when it matches, marks its whole token
// span (preposition through the last clock-expr token) consumed and returns
// true -- the caller (parse_nl) then SKIPS the entire duration pipeline
// (magnitude guard, main N-U search, cheap-tier/fraction fallbacks, hundred-present guard)
// for this utterance: letting them ALSO run risks exactly the silent
// misread this discriminator exists to prevent ("a las seis cuarenta y
// cinco" must never fall through to the spoken-M:SS-pair rule and silently
// resolve as a 405s
// DURATION instead of a clock target). First-match-wins, same convention as
// every other rule in this file.
static bool match_clock_target(const NlToken tokens[], const char *const normptrs[], int count,
                                bool consumed[], NlClockTarget *out) {
  // Ambiguous-two-hour veto (two_hour_pair_ambiguous, above) -- must run
  // before EVERY sub-strategy below, not just the joiner-based minute-read
  // that first surfaced it (see that function's own comment for why a single
  // sub-matcher decline isn't sufficient).
  if (two_hour_pair_ambiguous(tokens, normptrs, count)) return false;

  // The ES bare subtractive shape A has no
  // leading preposition/article at all -- must run FIRST, before the
  // ordinary prep/article loop below would otherwise find the embedded
  // "las <HOUR>" on its own and wrongly resolve just the bare hour (see
  // match_subtractive_a_bare's own doc comment).
  if (match_subtractive_a_bare(tokens, normptrs, count, consumed, out)) return true;

  for (int p = 0; p < count; p++) {
    int prep_start;
    int expr_start;

    if (exact_match_blob(tokens[p].norm, CLOCK_PREP_WORDS_EN_BLOB, sizeof(CLOCK_PREP_WORDS_EN_BLOB))) {
      prep_start = p;
      expr_start = p + 1;
    } else if (exact_match_blob(tokens[p].norm, CLOCK_ART_WORDS_ES_BLOB, sizeof(CLOCK_ART_WORDS_ES_BLOB))) {
      prep_start = p;
      if (p > 0 && (strcmp(tokens[p - 1].norm, "a") == 0 || strcmp(tokens[p - 1].norm, "hasta") == 0)) {
        prep_start = p - 1;
      }
      expr_start = p + 1;
    } else if (strcmp(tokens[p].norm, "hasta") == 0 && p + 1 < count &&
               tokens[p + 1].cls == CLS_COLONTIME) {
      // One of the guarded cases ("hasta 18:30"): bare "hasta" (no
      // "las"/"la") never triggers for a PLAIN number --
      // test_clock_target_discriminator's own
      // assert_clock_target("hasta seis", false, ...) is a load-bearing
      // regression lock this must not break -- but a COLON-shaped
      // continuation is unambiguous enough (nobody says "hasta 18:30" to
      // mean an 18-minute-30-second duration) to trigger on its own,
      // narrowly scoped to CLS_COLONTIME only.
      prep_start = p;
      expr_start = p + 1;
    } else {
      continue;
    }

    NlClockTarget ct = {0};
    int end = -1;
    if (!match_clock_expr_ext(tokens, normptrs, count, expr_start, &ct, &end)) continue;

    for (int k = prep_start; k <= end; k++) consumed[k] = true;
    *out = ct;
    return true;
  }

  // A valid clock-expr immediately followed by a meridiem, with
  // no preposition at all ("6:45 pm", bare "noon"/"medianoche").
  if (match_bare_meridiem_clock(tokens, normptrs, count, consumed, out)) return true;

  // An alarm/wake cue promoting a unit-less two-number pair to clock.
  if (match_alarm_cue_clock(tokens, normptrs, count, consumed, out)) return true;

  return false;
}

// -- Colon-time / slash-fraction / decimal-quantity shapes ---------------
// Hand-rolled byte scanners, matching this file's own no-strtol/no-allocation
// discipline (parse_digits, above). Each returns false (no match) rather
// than guessing on anything outside its exact shape -- the caller
// (classify_token_context_free, below) only calls these on a token that
// nl_normalize has already confirmed contains a preserved digit-flanked
// ':' '/' '.' ',' , but they re-validate the whole token defensively anyway
// (never trust a caller's classification to hold forever).

// Scans a run of 1-2 ASCII digits starting at `s[*i]`, advancing `*i` past
// them. False (no group consumed, `*i` unchanged) on zero digits or a run
// longer than 2 -- the colon-time/slash-fraction shapes are both `\d{1,2}`.
// The loop itself is capped at 2 iterations
// (never accumulates a 3rd digit into `v`) -- a fuzz-found UBSan signed-
// overflow (this function's first draft kept multiplying `v` for the WHOLE
// digit run before checking the length, so a long adversarial digit run
// overflowed int32_t) is why the length check happens by construction, not
// after the fact.
static bool scan_digit_group_2(const char *s, size_t *i, int32_t *val) {
  size_t start = *i;
  int32_t v = 0;
  int n = 0;
  while (n < 2 && s[*i] >= '0' && s[*i] <= '9') {
    v = v * 10 + (s[*i] - '0');
    (*i)++;
    n++;
  }
  if (n == 0) {
    *i = start;
    return false;
  }
  if (s[*i] >= '0' && s[*i] <= '9') {
    // A 3rd digit immediately follows -- not a 1-2 digit group at all.
    *i = start;
    return false;
  }
  *val = v;
  return true;
}

// CLS_COLONTIME: `^\d{1,2}(:\d{1,2}){1,2}$` -- a pair (`arity=2`, g3=0) or a
// triple (`arity=3`). Range validity (g2/g3 <= 59, etc.) is NOT checked here
// -- same "classifier matches the SHAPE, the semantic rule checks the
// RANGE" separation every other rule in this file already uses (e.g.
// match_clock_expr's own hour/minute bounds) -- `6:75` classifies as
// CLS_COLONTIME just fine; the duration rule below declines it.
static bool parse_colon_time(const char *norm, int32_t *g1, int32_t *g2, int32_t *g3, int *arity) {
  size_t i = 0;
  int32_t a, b, c;
  if (!scan_digit_group_2(norm, &i, &a)) return false;
  if (norm[i] != ':') return false;
  i++;
  if (!scan_digit_group_2(norm, &i, &b)) return false;
  if (norm[i] == '\0') {
    *g1 = a;
    *g2 = b;
    *g3 = 0;
    *arity = 2;
    return true;
  }
  if (norm[i] != ':') return false;
  i++;
  if (!scan_digit_group_2(norm, &i, &c)) return false;
  if (norm[i] != '\0') return false;
  *g1 = a;
  *g2 = b;
  *g3 = c;
  *arity = 3;
  return true;
}

// CLS_SLASHFRAC: `^\d{1,2}/\d{1,2}$`. Denominator support (`den in {2,4}`)
// is a semantic-rule concern (below), not a classifier concern.
static bool parse_slash_frac(const char *norm, int32_t *num, int32_t *den) {
  size_t i = 0;
  int32_t n, d;
  if (!scan_digit_group_2(norm, &i, &n)) return false;
  if (norm[i] != '/') return false;
  i++;
  if (!scan_digit_group_2(norm, &i, &d)) return false;
  if (norm[i] != '\0') return false;
  *num = n;
  *den = d;
  return true;
}

// CLS_DECIMAL: `^\d+[.,]\d+$` -- unlike the colon/slash groups above,
// neither digit run is capped at 2 ("12.5", "1.25" are both real observed
// shapes), but each is capped at 6 digits (decline beyond that, matching
// this file's other "pathological input declines rather than overflows"
// guards, e.g. parse_digits' own clamp) -- comfortably more than any real
// timer-duration decimal needs. `*frac_digits` is the fractional run's
// digit COUNT (the "k" in a `10^k` denominator generalization, e.g. "1.25"
// -> frac_digits=2, so the caller divides by 100 not 10).
static bool parse_decimal(const char *norm, int32_t *int_part, int32_t *frac_part,
                           int *frac_digits) {
  size_t i = 0;
  int32_t ip = 0;
  int ni = 0;
  while (norm[i] >= '0' && norm[i] <= '9') {
    ip = ip * 10 + (norm[i] - '0');
    i++;
    ni++;
    if (ni > 6) return false;
  }
  if (ni == 0) return false;
  if (norm[i] != '.' && norm[i] != ',') return false;
  i++;
  int32_t fp = 0;
  int nf = 0;
  while (norm[i] >= '0' && norm[i] <= '9') {
    fp = fp * 10 + (norm[i] - '0');
    i++;
    nf++;
    if (nf > 6) return false;
  }
  if (nf == 0) return false;
  if (norm[i] != '\0') return false;
  *int_part = ip;
  *frac_part = fp;
  *frac_digits = nf;
  return true;
}

// A colon token immediately adjacent to a CLOCK-CONTEXT signal must NOT be
// read as a duration by the colon-routing block (below, in parse_nl) --
// shipping the colon SHAPE before the clock-context ROUTING existed would
// otherwise introduce a NEW transitional silent-wrong: "a las 18:30"/
// "at 6:45"/"6:45 pm" were safely fail-soft before that pass (this file's
// own never-silent-wrong rule) and must stay that way, not silently become
// a wrong-semantics duration with the preposition/meridiem leaking into the
// name as pollution. Reuses match_clock_target's/match_clock_expr's OWN
// word sets as the one source of truth -- no second, drifting word list:
//   - PRECEDING signal: CLOCK_PREP_WORDS_EN ("at"/"until"/"til"/"till") or
//     CLOCK_ART_WORDS_ES ("las"/"la") immediately before the colon token --
//     exactly match_clock_target's own trigger set. Also "hasta" (bare,
//     directly before the colon token, no "las"/"la"): match_clock_target
//     itself already treats "hasta" as clock-context GLUE (extending
//     prep_start backward when "hasta las"/"hasta la" appears), even though
//     "hasta" ALONE never independently triggers a recognized clock TARGET
//     (that non-recognition is unchanged, intentional, existing behavior);
//     but it unambiguously signals clock-context language, so the colon
//     token must still decline to a duration rather than risk the same
//     silent-wrong class ("hasta 18:30" was ok=false originally; it must
//     stay that way, not become a confident-wrong 1110s).
//   - FOLLOWING signal: a meridiem marker (CLOCK_PM_WORDS/CLOCK_AM_WORDS)
//     right after the colon token, optionally through the same "de"/"la"/
//     "el" skip match_clock_expr's own meridiem check already uses ("6:45
//     pm", "a las 6:45 de la tarde") -- a bare pm/am is as strong a clock
//     signal as "at", the same reasoning applied to the real clock-target
//     routing.
static bool colon_needs_clock_context_guard(const NlToken tokens[], int count, int idx) {
  if (idx > 0) {
    const char *prev = tokens[idx - 1].norm;
    if (exact_match_blob(prev, CLOCK_PREP_WORDS_EN_BLOB, sizeof(CLOCK_PREP_WORDS_EN_BLOB))) return true;
    if (exact_match_blob(prev, CLOCK_ART_WORDS_ES_BLOB, sizeof(CLOCK_ART_WORDS_ES_BLOB))) return true;
    if (strcmp(prev, "hasta") == 0) return true;
  }

  int m_idx = idx + 1;
  if (m_idx < count && strcmp(tokens[m_idx].norm, "de") == 0) m_idx++;
  if (m_idx < count &&
      (strcmp(tokens[m_idx].norm, "la") == 0 || strcmp(tokens[m_idx].norm, "el") == 0)) {
    m_idx++;
  }
  if (m_idx < count && exact_match_blob(tokens[m_idx].norm, CLOCK_PM_WORDS_BLOB, sizeof(CLOCK_PM_WORDS_BLOB))) {
    return true;
  }
  if (m_idx < count && exact_match_blob(tokens[m_idx].norm, CLOCK_AM_WORDS_BLOB, sizeof(CLOCK_AM_WORDS_BLOB))) {
    return true;
  }
  return false;
}

// -- Classification ---------------------------------------------------------

// Context-free pass: exact vocab match only -- number/unit/kind
// tokens resolve by exact match against the merged closed vocab; there is
// no edit-distance correction into any vocabulary class. Dictation emits
// whole real words, never character-level typos, so a fuzzy/edit-distance
// layer here has no real typo to rescue and instead silently miscorrects an
// open-ended set of ordinary real words that happen to sit one edit from a
// short vocab word. The position-gated homophone table
// (apply_homophone_coercion, below) remains -- it is closed, position-safe,
// and the correct tool for this channel's actual error mode (a mis-heard
// whole word, not a mis-typed character).
static NlTokClass classify_token_context_free(const char *norm) {
  if (norm[0] == '\0') return CLS_WORD;

  // Checked BEFORE is_all_digits -- none of the three can actually match an
  // all-digit token anyway (each requires a preserved ':' '/' '.' ','
  // separator nl_normalize only keeps between two digits), so this is a
  // documentation-order choice, not a behavioral one.
  int32_t g1, g2, g3, num, den, ip, fp;
  int arity, fd;
  if (parse_colon_time(norm, &g1, &g2, &g3, &arity)) return CLS_COLONTIME;
  if (parse_slash_frac(norm, &num, &den)) return CLS_SLASHFRAC;
  if (parse_decimal(norm, &ip, &fp, &fd)) return CLS_DECIMAL;

  if (is_all_digits(norm)) return CLS_NUMBER;

  if (exact_match_blob(norm, KIND_STOPWATCH_WORDS_BLOB, sizeof(KIND_STOPWATCH_WORDS_BLOB))) return CLS_KIND_STOPWATCH;
  if (exact_match_blob(norm, KIND_REPEAT_WORDS_BLOB, sizeof(KIND_REPEAT_WORDS_BLOB))) return CLS_KIND_REPEAT;
  if (exact_match_blob(norm, KIND_COUNTDOWN_WORDS_BLOB, sizeof(KIND_COUNTDOWN_WORDS_BLOB))) return CLS_KIND_COUNTDOWN;

  int32_t mult;
  if (unit_word_exact(norm, &mult)) return CLS_UNIT;
  int val;
  if (number_word_exact(norm, &val)) return CLS_NUMBER;

  if (exact_match_blob(norm, JOINER_WORDS_BLOB, sizeof(JOINER_WORDS_BLOB))) return CLS_JOINER;
  if (exact_match_blob(norm, APPROX_DROP_WORDS_BLOB, sizeof(APPROX_DROP_WORDS_BLOB))) return CLS_APPROX;
  // Checked here -- after KIND/UNIT/NUMBER/JOINER/APPROX, before FILLER.
  if (exact_match_blob(norm, NAMEDROP_WORDS_BLOB, sizeof(NAMEDROP_WORDS_BLOB))) return CLS_NAMEDROP;
  if (exact_match_blob(norm, FILLER_WORDS_BLOB, sizeof(FILLER_WORDS_BLOB))) return CLS_FILLER;

  return CLS_WORD;
}

// Walks HOMOPHONE_BLOB's alternating heard/canonical pairs; on a match,
// copies the canonical spelling into `out` (truncated to out_size, always
// NUL-terminated) and returns true.
static bool homophone_lookup(const char *tok, char *out, size_t out_size) {
  size_t i = 0;
  while (i < sizeof(HOMOPHONE_BLOB)) {
    const char *heard = HOMOPHONE_BLOB + i;
    i += strlen(heard) + 1;
    if (i >= sizeof(HOMOPHONE_BLOB)) break;
    const char *canonical = HOMOPHONE_BLOB + i;
    i += strlen(canonical) + 1;
    if (strcmp(tok, heard) == 0) {
      strncpy(out, canonical, out_size - 1);
      out[out_size - 1] = '\0';
      return true;
    }
  }
  return false;
}

// Position-aware homophone coercion: a
// FILLER/WORD token that is a known homophone of a digit is coerced to that
// digit ONLY when the immediately-following token is a UNIT ("for minutes"
// -> "four minutes", "ate seconds" -> "eight seconds") -- this is the
// specific, narrow rule that also correctly leaves "pasta for eight
// minutes" alone: "for" there is followed by "eight" (a NUMBER, not a
// UNIT), so it stays the filler it actually is and "eight minutes" resolves
// normally.
static void apply_homophone_coercion(NlToken tokens[], int count) {
  for (int i = 0; i < count; i++) {
    if (tokens[i].cls != CLS_FILLER && tokens[i].cls != CLS_WORD) continue;
    bool next_is_unit = (i + 1 < count) && tokens[i + 1].cls == CLS_UNIT;
    if (!next_is_unit) continue;

    char canonical[NL_TOKEN_MAX];
    if (homophone_lookup(tokens[i].norm, canonical, sizeof canonical)) {
      strncpy(tokens[i].norm, canonical, NL_TOKEN_MAX - 1);
      tokens[i].norm[NL_TOKEN_MAX - 1] = '\0';
      tokens[i].cls = CLS_NUMBER;
    }
  }
}

// True for a WORD-form number value that is a round multiple of ten in
// 20..90 -- i.e. an actual "tens" word (en twenty/thirty/.../ninety, es
// veinte/treinta/.../noventa), NOT a digit token that merely happens to be a
// multiple of ten (a spoken/typed "20" never compounds further) and not a
// teen (diez/ten=10 fails %10==0's >=20 half, correctly excluded).
static bool is_tens_word_value(int32_t val) {
  return val >= 20 && val <= 90 && (val % 10 == 0);
}

// True for a value in the 1..9 "ones" range -- the only continuation a tens
// word may compound with ("tens + ones").
static bool is_ones_value(int32_t val) {
  return val >= 1 && val <= 9;
}

// -- nl_words_to_number (public) ---------------------------------------------

int32_t nl_words_to_number(const char *const *tokens, int n, int *consumed) {
  if (n <= 0 || tokens[0] == NULL) {
    if (consumed != NULL) *consumed = 0;
    return 0;
  }

  const char *tok = tokens[0];
  int32_t value;
  bool is_tens_word;

  if (is_all_digits(tok)) {
    value = parse_digits(tok);
    is_tens_word = false;  // a digit token is already a complete value -- never compounds further
  } else {
    int val;
    if (!number_word_exact(tok, &val)) {
      if (consumed != NULL) *consumed = 0;
      return 0;
    }
    value = val;
    is_tens_word = is_tens_word_value(val);
  }

  // Structural validation, not "any adjacent number tokens
  // sum": only a genuine WORD-form tens value may extend with exactly one
  // "ones" (1-9) continuation, optionally through a "y"/"and" joiner (en
  // "twenty five" has no joiner; es "treinta y cinco" does; both consume
  // the same shape). This is the ONLY compounding this function performs --
  // digit tokens, ones/teens words, and already-lexicalized es 21-29 tokens
  // (veintitres etc., not a round multiple of ten) all stop at one token.
  // Rejects "twenty twenty five" (tens followed by tens -- the second
  // "twenty" isn't 1-9), "sixty ten" (ten=10 isn't 1-9 either), and "5 5"
  // (digit tokens never compound at all).
  int i = 1;
  if (is_tens_word && i < n) {
    int j = i;
    if (exact_match_blob(tokens[j], JOINER_WORDS_BLOB, sizeof(JOINER_WORDS_BLOB)) && j + 1 < n) j++;
    if (j < n && !is_all_digits(tokens[j])) {
      int ones_val;
      if (number_word_exact(tokens[j], &ones_val) && is_ones_value(ones_val)) {
        value += ones_val;
        i = j + 1;
      }
    }
  }

  if (consumed != NULL) *consumed = i;
  return value;
}

// Shared by the trailing-subcount and half-word-extension blocks inside
// parse_nl() below. Both scan for the first bare CLS_UNIT token (the
// "implicit count of 1" case -- "an hour twenty", "hora y media", no
// preceding number). `require_mult60` matches each call site's own guard
// exactly: the trailing sub-count rule only accepts a minute-or-larger unit
// (`mult >= 60 && mult % 60 == 0`); the half-word extension accepts any
// unit here (its own, different exactness guard applies only in the OTHER
// case, the already-resolved-match one, which is small enough per call site
// that it stays inline rather than folding into this helper too).
static int find_bare_qualifying_unit(const NlToken tokens[], int count, bool require_mult60,
                                      int32_t *out_mult) {
  for (int i = 0; i < count; i++) {
    int32_t mult;
    if (tokens[i].cls == CLS_UNIT && unit_word_exact(tokens[i].norm, &mult) &&
        (!require_mult60 || (mult >= 60 && mult % 60 == 0))) {
      *out_mult = mult;
      return i;
    }
  }
  return -1;
}

// -- parse_nl (public) --------------------------------------------------------

bool parse_nl(const char *transcript, NlParse *out) {
  if (out == NULL) return false;
  memset(out, 0, sizeof(*out));
  out->kind = KIND_COUNTDOWN;

  if (!parse_nl_vocabulary_available()) return false;

  if (transcript == NULL || transcript[0] == '\0') {
    out->ok = false;
    return false;
  }

  char raw_tokens[NL_MAX_TOKENS][NL_TOKEN_MAX];
  int count = nl_tokenize_raw(transcript, raw_tokens, NL_MAX_TOKENS);

  NlToken tokens[NL_MAX_TOKENS];
  for (int i = 0; i < count; i++) {
    strncpy(tokens[i].raw, raw_tokens[i], NL_TOKEN_MAX - 1);
    tokens[i].raw[NL_TOKEN_MAX - 1] = '\0';
    nl_normalize(raw_tokens[i], tokens[i].norm, NL_TOKEN_MAX);
    tokens[i].cls = classify_token_context_free(tokens[i].norm);
  }

  apply_homophone_coercion(tokens, count);

  bool consumed[NL_MAX_TOKENS] = {0};
  for (int i = 0; i < count; i++) {
    switch (tokens[i].cls) {
      case CLS_KIND_STOPWATCH: consumed[i] = true; break;
      case CLS_KIND_REPEAT:    consumed[i] = true; break;
      case CLS_KIND_COUNTDOWN: consumed[i] = true; break;
      // A CLS_UNIT token is always
      // pre-marked consumed here regardless of whether any rule's match
      // actually uses it -- this is the ONE convention in this whole switch
      // that pattern code must not rely on `consumed[]` to check "is this
      // unit slot free": call unit_word_exact() directly on the token
      // instead. (Every other consumed-here class -- FILLER/JOINER/APPROX/
      // kind keywords -- is fine to treat as simply "gone": nothing ever
      // needs to re-examine what a filler/joiner/kind word WAS, only that
      // it isn't a duration/name token. A UNIT token is different: multiple
      // rules need to know its multiplier after this point, e.g. the
      // fraction patterns below via unit_word_exact(tokens[after].norm,
      // &mult), never via consumed[after].) A first Pattern-1 fraction
      // draft gated its trailing-unit slot on `!consumed[after]`, which is
      // always true for a CLS_UNIT token by construction -- every Pattern-1
      // transcript failed until that check was dropped (see the usage-site
      // note near the fraction pattern below). Documented here, at the
      // marking site, so the next pattern author sees this before writing a
      // fifth rule block, not after re-discovering it the same way.
      case CLS_UNIT:           consumed[i] = true; break;
      case CLS_FILLER:         consumed[i] = true; break;
      case CLS_JOINER:         consumed[i] = true; break;
      case CLS_APPROX:         consumed[i] = true; break;
      // Same treatment as CLS_APPROX -- dropped from residue
      // unconditionally, opaque to the clause-scoped kind-resolution walk
      // (see the enum's own comment).
      case CLS_NAMEDROP:       consumed[i] = true; break;
      case CLS_NUMBER:         break;  // consumed below only if it lands in the winning match
      // Same treatment as CLS_NUMBER -- these are
      // duration anchors, consumed only by the routing blocks below if
      // they actually win a match, never pre-consumed like FILLER/JOINER/
      // UNIT/APPROX/kind keywords.
      case CLS_COLONTIME:      break;
      case CLS_SLASHFRAC:      break;
      case CLS_DECIMAL:        break;
      case CLS_WORD:           break;
    }
  }

  // The magnitude guard: a magnitude word
  // (complete en+es lexicon, MAGNITUDE_WORDS above) sitting next to a
  // recognized number token -- directly, or across a single optional joiner
  // ("a hundred AND twenty") -- means the speaker meant a value we cannot
  // compute. Never let some unrelated, independent number+unit pair
  // ELSEWHERE in the utterance silently "rescue" a match and produce a
  // confidently wrong duration ("one hundred twenty minutes" must not
  // silently become "20 minutes"). Checked structurally (any magnitude word
  // adjacent to any NUMBER token anywhere) so the whole duration search is
  // skipped rather than just the one adjacent attempt.
  bool magnitude_adjacent = false;
  for (int i = 0; i < count && !magnitude_adjacent; i++) {
    if (!exact_match_blob(tokens[i].norm, MAGNITUDE_WORDS_BLOB, sizeof(MAGNITUDE_WORDS_BLOB))) continue;
    if (i > 0 && tokens[i - 1].cls == CLS_NUMBER) magnitude_adjacent = true;
    if (!magnitude_adjacent && i > 1 && tokens[i - 1].cls == CLS_JOINER &&
        tokens[i - 2].cls == CLS_NUMBER) {
      magnitude_adjacent = true;
    }
    if (!magnitude_adjacent && i + 1 < count && tokens[i + 1].cls == CLS_NUMBER) {
      magnitude_adjacent = true;
    }
    if (!magnitude_adjacent && i + 2 < count && tokens[i + 1].cls == CLS_JOINER &&
        tokens[i + 2].cls == CLS_NUMBER) {
      magnitude_adjacent = true;
    }
  }

  // Any UNCONSUMED
  // HUNDRED_WORDS token anywhere in the utterance means the ordinary
  // duration rules below must not silently match some OTHER, independent
  // number+unit pair while ignoring it -- e.g. "ciento veinte minutos" must
  // never resolve as a plain "veinte minutos"=1200s pair with "ciento" left
  // as an unrelated residue word, the exact same "don't let an unrelated
  // match rescue a wrong-magnitude phrase" principle the magnitude guard's own
  // magnitude_adjacent already applies to thousand+ -- deliberately a
  // broader "present anywhere", not just "adjacent to a number", since a
  // hundred-composite's own leading H is optional (bare "cien horas" has no
  // adjacent number at all).
  //
  // The positive composer
  // this guard used to route matches into (match_hundred_composite()) was
  // removed for size -- 1,063 B for a small number of long-tail hundred-word
  // phrasings, a deliberate capability trade against the app's tight binary
  // size budget. This guard is DELIBERATELY KEPT unchanged: every
  // duration rule below is gated `!hundred_present`, so with no composer
  // left, a hundred-word utterance now falls through every rule and REFUSES
  // (fail-soft) instead of composing -- never silently misparsed by an
  // unrelated pair elsewhere. Removing this guard alongside the composer
  // would flip that outcome from refuse to silent-wrong, which is forbidden.
  bool hundred_present = false;
  for (int i = 0; i < count; i++) {
    if (!consumed[i] && exact_match_blob(tokens[i].norm, HUNDRED_WORDS_BLOB, sizeof(HUNDRED_WORDS_BLOB))) {
      hundred_present = true;
      break;
    }
  }

  int32_t duration_s = 0;
  bool has_duration = false;
  int match_start = -1;
  int match_end = -1;  // last token index the winning match consumed
  // Tracks whether the compound-sum second pair below fired -- the
  // hours+trailing-minutes rule (further down) must not also try to extend
  // a match whose unit slot is already a genuine two-unit compound.
  bool used_compound = false;
  // True only when `duration_s`/`match_start`/
  // `match_end` came from the PLAIN main N-U pair search directly below
  // (never the colon/slash/decimal/cheap-tier/fraction/hundred-composite
  // fallbacks, and never a chained compound -- see `used_compound`) -- the
  // correction rule only re-binds a later numeral against a simple, single
  // N-U pair's own unit, per every phrasing it closes.
  bool simple_pair_matched = false;
  int32_t last_match_mult = 0;

  // Hoisted so the hours+trailing-minutes and pair-rule blocks (below,
  // after this search) can reuse it too, not just this search.
  const char *normptrs[NL_MAX_TOKENS];
  for (int i = 0; i < count; i++) normptrs[i] = tokens[i].norm;

  // The clock-target discriminator is
  // checked FIRST, before magnitude_adjacent or any duration rule -- see
  // match_clock_target's own doc comment for why it must take precedence
  // (never let the ordinary duration grammar silently "rescue" a clock-
  // prefixed utterance as a plain duration). `ct_matched` gates every
  // duration rule below; a CT match consumes its own token span directly
  // (mutating `consumed[]`) and leaves has_duration/duration_s at their
  // zero-initialized defaults -- lib/parse_nl stays pure, so resolving
  // NlParse.clock into an actual duration needs the caller's own clock
  // read (dictation_am.c's deliver(), or the gallery's fixed-`now` fixture
  // -- both call nl_clock_target_to_duration() themselves after parse_nl()
  // returns).
  NlClockTarget clock_out = {0};
  bool ct_matched = match_clock_target(tokens, normptrs, count, consumed, &clock_out);

  // Also gated on !hundred_present --
  // see that flag's own doc comment for why an unrelated pair elsewhere
  // must not silently rescue a hundred-composite phrase.
  if (!ct_matched && !magnitude_adjacent && !hundred_present) {
    for (int i = 0; i < count; i++) {
      if (consumed[i] || tokens[i].cls != CLS_NUMBER) continue;

      int run_consumed = 0;
      int32_t val = nl_words_to_number((const char *const *)&normptrs[i], count - i, &run_consumed);
      if (run_consumed <= 0) continue;

      int unit_idx = i + run_consumed;
      // Bridge a run of CLS_APPROX/CLS_NAMEDROP connective tokens between
      // the number and its unit -- "five uh minutes"/"cinco este minutos"
      // (some STT engines keep the disfluency verbatim; others strip it,
      // making this a no-op there). Deliberately does NOT also bridge
      // CLS_FILLER here (unlike the fraction/clock engines' own
      // FILLER|JOINER skips) -- doing so UNCONDITIONALLY would reopen the
      // load-bearing "once a minute" collision guard just below (a general
      // filler skip flips that pinned regression from false to true=660s).
      // The narrower "couple"-only FILLER skip immediately below is
      // unaffected/still its own case.
      while (unit_idx < count &&
             (tokens[unit_idx].cls == CLS_APPROX || tokens[unit_idx].cls == CLS_NAMEDROP)) {
        unit_idx++;
      }
      // "a couple OF minutes" needs
      // the FILLER "of" skipped between the number and its unit -- NARROWLY
      // scoped to the literal word "couple" (never a general "skip any
      // filler between a number and its unit" rule): a general skip would
      // reopen the "once" collision guard
      // (test_once_en_es_collision) -- "once a minute" must keep FAILING
      // precisely because the filler "a" breaks adjacency there, which is
      // the only thing that tells the Spanish number "once"=11 apart from
      // the English frequency idiom "once a ___" (verified empirically:
      // generalizing this skip to all numbers flips that pinned case from
      // false to true=660s). "couple" is never "once", so gating on the
      // literal token cannot touch that guard.
      if (run_consumed == 1 && strcmp(tokens[i].norm, "couple") == 0) {
        while (unit_idx < count && tokens[unit_idx].cls == CLS_FILLER) unit_idx++;
      }
      int32_t mult;
      if (unit_idx >= count || !unit_word_exact(tokens[unit_idx].norm, &mult)) continue;

      // The original literal claim behind this guard -- "a name never sits
      // between two numbers of one duration phrase" -- is falsified by ordinary
      // phrasings like "5 people 5 minutes" / "5 rounds 5 minutes" (a
      // headcount/name token, not a magnitude morpheme, sitting between two
      // numbers). An earlier unconditional "any CLS_WORD between two
      // numbers" catch failed the ENTIRE parse on these, not just the name.
      // Narrowed to the catch's OWN STATED PURPOSE -- "a magnitude word we
      // somehow failed to list" -- via word_near_magnitude_lexicon() above:
      // only disable the search when the intervening word is itself close
      // to a KNOWN magnitude morpheme (catches typos/near-misses of
      // "hundred"/"mil"/etc., the belt-and-suspenders case this guard
      // exists for); an unrelated residue word no longer breaks adjacency,
      // so "5 people 5 minutes" now correctly yields duration=5min +
      // name="5 people" instead of ok=false. Does NOT reopen the exact-word
      // magnitude case: every exact "thousand"/"mil" etc. repro is already
      // caught upstream by the exact-lexicon guard above (magnitude_
      // adjacent), which runs before this whole duration-search loop even
      // starts -- this narrowing only affects words that exact match
      // already let through. Whole-search disable preserved when it DOES
      // fire, same conservative posture as before.
      if (i >= 2 && tokens[i - 1].cls == CLS_WORD && tokens[i - 2].cls == CLS_NUMBER &&
          word_near_magnitude_lexicon(tokens[i - 1].norm)) {
        break;
      }

      // First N U pair found ("first match
      // wins"). Sum in int64_t -- val/mult are each individually bounded,
      // but their product can exceed int32_t range for pathological input
      // ("999999 hours"); clamp to just past the real duration ceiling so
      // the range guard below rejects it, never UB on the narrowing cast.
      int64_t total64 = (int64_t)val * (int64_t)mult;

      for (int k = i; k < i + run_consumed; k++) consumed[k] = true;
      consumed[unit_idx] = true;
      int end_idx = unit_idx;

      // Optional compound sum: [N U] ([y|and]? N U)* -- GENERALIZED to a
      // descending-unit CHAIN of arbitrary length, closing the on-device-
      // class "1 hour 20 minutes 30 seconds" bug: the old code matched
      // exactly ONE additional pair, silently dropping a 3rd unit -- "una
      // hora veinte minutos y treinta segundos" was landing at 4800 with
      // "treinta" (30s) lost, not 4830. The original guard ("the second
      // pair must use a DIFFERENT unit than the first") is TIGHTENED, not
      // loosened: each
      // subsequent pair's unit must be STRICTLY SMALLER than the
      // immediately preceding pair's own unit (hour(3600) > minute(60) >
      // second(1)) -- canonical descending spoken order. Every pre-existing
      // pinned compound row is already descending (verified against
      // test_compound_duration_sum_en_es: minute->second, hour->minute), so
      // this is a strict tightening + generalization in one: it still
      // rejects a same-unit restatement ("5 minutes 5 minutes" -- 60 is
      // not < 60) and ALSO rejects an ascending reorder ("5 minutes 3
      // hours" -- 3600 is not < 60; left unguarded, this would silently sum
      // to 11100s), while additionally accepting a 3rd/4th/... strictly-smaller pair
      // instead of leaving it as name residue. Stops at the first
      // non-descending unit, non-pair, or unconsumed name token -- disjoint
      // from the trailing-subcount/pair rules below by the unit-presence
      // test (a chain pair always carries its own unit token; those rules
      // need a BARE trailing number or no unit anywhere, respectively).
      int32_t last_mult = mult;
      int j = unit_idx + 1;
      for (;;) {
        int j2 = j;
        if (j2 < count && tokens[j2].cls == CLS_JOINER) j2++;
        if (j2 >= count || consumed[j2] || tokens[j2].cls != CLS_NUMBER) break;

        int run2 = 0;
        int32_t val2 = nl_words_to_number((const char *const *)&normptrs[j2], count - j2, &run2);
        int unit_idx2 = j2 + run2;
        int32_t mult2;
        if (run2 <= 0 || unit_idx2 >= count || !unit_word_exact(tokens[unit_idx2].norm, &mult2) ||
            mult2 >= last_mult) {
          break;
        }

        total64 += (int64_t)val2 * (int64_t)mult2;
        for (int k = j2; k < j2 + run2; k++) consumed[k] = true;
        consumed[unit_idx2] = true;
        end_idx = unit_idx2;
        used_compound = true;
        last_mult = mult2;
        j = unit_idx2 + 1;
      }

      duration_s = (total64 > (int64_t)TIMER_MAX_DURATION_S) ? (int32_t)TIMER_MAX_DURATION_S + 1
                                                              : (int32_t)total64;
      has_duration = true;
      match_start = i;
      match_end = end_idx;
      simple_pair_matched = !used_compound;  // used by the correction rule below
      last_match_mult = mult;                 // used by the correction rule below
      break;
    }
  }

  // -- CLS_COLONTIME duration routing -------------------------------------
  // Clock-context routing of colon tokens is handled separately by the
  // clock-target discriminator above; this block only delivers the
  // token + duration reads. A colon token is its own
  // class (CLS_COLONTIME, never CLS_NUMBER), so it cannot double-fire with
  // the main N-U search above, or be seen by the tens+ones accumulator --
  // gated the same as every other duration rule here (!ct_matched/
  // !magnitude_adjacent/!hundred_present), fallback-only (!has_duration)
  // like every cheap-tier/fraction rule below.
  if (!ct_matched && !has_duration && !magnitude_adjacent && !hundred_present) {
    for (int i = 0; i < count; i++) {
      if (consumed[i] || tokens[i].cls != CLS_COLONTIME) continue;

      // A clock-context signal
      // (preposition/article precedes, or a meridiem marker follows) means
      // this pass must decline entirely and stay fail-soft -- see
      // colon_needs_clock_context_guard's own doc comment for the full
      // reasoning. This is what keeps "a las 18:30"/"at 6:45"/"6:45 pm"
      // from being silently misread as a duration by this colon-routing pass.
      if (colon_needs_clock_context_guard(tokens, count, i)) continue;

      int32_t g1, g2, g3;
      int arity;
      if (!parse_colon_time(tokens[i].norm, &g1, &g2, &g3, &arity)) continue;  // defensive; already classified

      if (arity == 3) {
        // Triple g1:g2:g3, no clock context -> DURATION H:M:S. A wall
        // clock has no seconds field in this app, so a triple in clock
        // context declines g3 instead -- not this block's concern; this is
        // the duration-only reading.
        if (g2 < 0 || g2 > 59 || g3 < 0 || g3 > 59) continue;  // fail-soft, e.g. bad field
        int64_t total = (int64_t)g1 * 3600 + (int64_t)g2 * 60 + g3;
        if (total < 1 || total > (int64_t)TIMER_MAX_DURATION_S) continue;
        duration_s = (int32_t)total;
        has_duration = true;
        consumed[i] = true;
        match_start = i;
        match_end = i;
        break;
      }

      // Pair g1:g2 -- unit-scaled if a trailing duration unit follows
      // ("6:45 minutes"->405, "6:45 hours"->24300, "6:45 seconds"->
      // fail-soft), else bare M:SS ("6:45"->405, a real observed phrasing;
      // "90:00"->5400). g2>59 ("6:75") fail-softs either way -- same lock as the
      // "diez sesenta" group-2>59 guard elsewhere in this file.
      if (g2 < 0 || g2 > 59) continue;

      // NOTE: no `consumed[unit_idx]` check -- a CLS_UNIT token is ALWAYS
      // pre-marked consumed by the classification loop's own switch
      // (above), regardless of use; call unit_word_exact() directly, per
      // the convention documented at that switch's CLS_UNIT case.
      int unit_idx = i + 1;
      int32_t mult;
      bool has_unit = unit_idx < count && unit_word_exact(tokens[unit_idx].norm, &mult);

      int64_t total;
      int end_idx = i;
      if (has_unit) {
        if (mult == 3600) {
          total = (int64_t)g1 * 3600 + (int64_t)g2 * 60;  // H:MM
        } else if (mult == 60) {
          total = (int64_t)g1 * 60 + g2;  // M:SS (redundant unit, tolerated)
        } else {
          continue;  // "seconds" (or any other unit) on a colon pair -- no sane reading (fail-soft)
        }
        end_idx = unit_idx;
      } else {
        total = (int64_t)g1 * 60 + g2;  // bare pair -- M:SS default
      }

      if (total < 1 || total > (int64_t)TIMER_MAX_DURATION_S) continue;
      duration_s = (int32_t)total;
      has_duration = true;
      consumed[i] = true;
      if (has_unit) consumed[unit_idx] = true;
      match_start = i;
      match_end = end_idx;
      break;
    }
  }

  // -- CLS_SLASHFRAC duration routing --------------------------------------
  // Reuses the exact unit-anchoring + exactness-guard arithmetic the
  // fraction engine (below) already uses,
  // sourced from a slash token instead of a HALF/QUARTER word run. Only the
  // denominators this app's fraction vocab already supports (2, 4) resolve;
  // an unsupported denominator (`1/3`) or a missing/unresolvable unit
  // fail-softs -- declines, never guesses (this closes a silent-wrong
  // class: `1/2 hour` used to silently read as `12 hours`).
  if (!ct_matched && !has_duration && !magnitude_adjacent && !hundred_present) {
    for (int i = 0; i < count; i++) {
      if (consumed[i] || tokens[i].cls != CLS_SLASHFRAC) continue;

      int32_t fnum, fden;
      if (!parse_slash_frac(tokens[i].norm, &fnum, &fden)) continue;  // defensive; already classified
      if (fden != 2 && fden != 4) continue;  // unsupported denominator -- fail-soft (was SW)

      int after = i + 1;
      while (after < count && (tokens[after].cls == CLS_FILLER || tokens[after].cls == CLS_JOINER)) {
        after++;
      }
      if (after >= count) continue;  // no unit -- fail-soft ("1/2" alone)
      // NOTE: no `consumed[after]` check -- see the CLS_COLONTIME block's
      // own comment above (same CLS_UNIT pre-consumed convention).
      int32_t mult;
      if (!unit_word_exact(tokens[after].norm, &mult)) continue;

      int64_t numer = (int64_t)fnum * mult;
      if (numer % fden != 0) continue;  // non-integer result -- decline, stay fail-soft

      int64_t total = numer / fden;
      if (total < 1 || total > (int64_t)TIMER_MAX_DURATION_S) continue;

      duration_s = (int32_t)total;
      has_duration = true;
      for (int k = i; k < after; k++) consumed[k] = true;
      consumed[after] = true;
      match_start = i;
      match_end = after;
      break;
    }
  }

  // -- CLS_DECIMAL duration routing -----------------------------------
  // The re-fix of an old decimal-split silent-wrong: "N.M <unit>" through
  // the same unit-anchored, exactness-guarded fraction arithmetic as every
  // other fraction rule in this file, generalized to an arbitrary number of
  // fractional digits (`10^frac_digits` denominator, e.g. "1.25"->den 100).
  // A bare decimal with no unit fail-softs -- "6.45" (no unit) no longer
  // coincidentally resolves via the old split-into-pair path (a deliberate
  // behavior change; the word form "six forty five"->405 and
  // the colon form "6:45"->405 both still give the same value another way).
  if (!ct_matched && !has_duration && !magnitude_adjacent && !hundred_present) {
    for (int i = 0; i < count; i++) {
      if (consumed[i] || tokens[i].cls != CLS_DECIMAL) continue;

      int32_t ip, fp;
      int fd;
      if (!parse_decimal(tokens[i].norm, &ip, &fp, &fd)) continue;  // defensive; already classified

      int after = i + 1;
      while (after < count && (tokens[after].cls == CLS_FILLER || tokens[after].cls == CLS_JOINER)) {
        after++;
      }
      if (after >= count) continue;  // bare decimal, no unit -- fail-soft
      // NOTE: no `consumed[after]` check -- see the CLS_COLONTIME block's
      // own comment above (same CLS_UNIT pre-consumed convention).
      int32_t mult;
      if (!unit_word_exact(tokens[after].norm, &mult)) continue;

      int64_t pow10 = 1;
      for (int k = 0; k < fd; k++) pow10 *= 10;

      int64_t scaled = (int64_t)ip * pow10 + fp;
      int64_t numer = scaled * mult;
      if (numer % pow10 != 0) continue;  // non-integer result -- decline (fail-soft), never round

      int64_t total = numer / pow10;
      if (total < 1 || total > (int64_t)TIMER_MAX_DURATION_S) continue;

      duration_s = (int32_t)total;
      has_duration = true;
      for (int k = i; k < after; k++) consumed[k] = true;
      consumed[after] = true;
      match_start = i;
      match_end = after;
      break;
    }
  }

  // -- Cheap tier: unit + trailing sub-count -------------------------------
  // Originally specced this for hours only ("<N> <hour-unit> <M>" ->
  // N*3600 + M*60, "una hora veinte" / "an hour twenty" / "hora y veinte" ->
  // 4800s). GENERALIZED, closing the case of "6 minutos 45" -> 6:00 named
  // "45": the exact same shape recurs one level down the unit hierarchy
  // -- "<N> <minute-unit> <M>" with M a bare 0-59 number is N*60 + M*1
  // seconds ("6 minutos 45" / "seis minutos cuarenta y cinco" / "six minutes
  // forty five" -> 405s) -- so this is one structural rule parameterized on
  // the matched unit's own multiplier, not two parallel copies. A unit
  // qualifies (has a "trailing sub-count" reading) iff its multiplier is
  // itself a whole multiple of 60 (`mult % 60 == 0`, and `mult >= 60` so
  // second's mult==1 is structurally excluded, not by a special-cased unit
  // list) -- the sub-count's own multiplier is then `mult / 60` (60/60=1s
  // for a minute unit, 3600/60=60s for an hour unit). Disjoint from the pair
  // rule below by the unit-presence test (this rule NEEDS a qualifying unit;
  // the pair rule fires only when NO unit exists anywhere) -- so it can
  // never touch that same-unit-restatement case above. Two shapes:
  //  (a) the search above already matched a SINGLE (non-compound) pair whose
  //      unit qualifies ("una hora veinte" / "6 minutos 45": the base pair
  //      already resolves to N*mult seconds, with the trailing bare number
  //      left over unconsumed) -- extend that match.
  //  (b) no match exists at all, but a bare qualifying-unit token is present
  //      with no preceding number word ("an hour twenty" / "a minute
  //      twenty": "an"/"a" is a filler, not a number, so the search above
  //      never fires on the unit at all; "hora y veinte" has no preceding
  //      number word either) -- the implicit count is 1, the everyday
  //      spoken idiom at either level. Gated on !magnitude_adjacent like
  //      every other duration rule in this file (the magnitude guard's
  //      conservative posture), on !ct_matched -- a clock target already
  //      claimed its own tokens and must not also be re-read as a duration
  //      -- and on !hundred_present, same reasoning as the main
  //      search's own gate above.
  if (!ct_matched && !magnitude_adjacent && !hundred_present) {
    int unit_idx_trail = -1;
    int32_t unit_n = 0;
    int32_t unit_mult = 0;

    if (has_duration && !used_compound && tokens[match_end].cls == CLS_UNIT &&
        unit_word_exact(tokens[match_end].norm, &unit_mult) && unit_mult >= 60 &&
        unit_mult % 60 == 0) {
      unit_idx_trail = match_end;
      unit_n = duration_s / unit_mult;
    } else if (!has_duration) {
      // find_bare_qualifying_unit() -- shared
      // scan with the half-word extension below.
      unit_idx_trail = find_bare_qualifying_unit(tokens, count, /*require_mult60=*/true, &unit_mult);
      if (unit_idx_trail >= 0) {
        unit_n = 1;  // implicit count -- bare "hour"/"hora"/"minute"/"minuto", no explicit number
      }
    }

    if (unit_idx_trail >= 0) {
      int next_idx = unit_idx_trail + 1;
      if (next_idx < count && tokens[next_idx].cls == CLS_JOINER) next_idx++;

      if (next_idx < count && !consumed[next_idx] && tokens[next_idx].cls == CLS_NUMBER) {
        int run = 0;
        int32_t mval =
            nl_words_to_number((const char *const *)&normptrs[next_idx], count - next_idx, &run);
        int after_idx = next_idx + run;
        int32_t junk_mult;
        bool trailing_has_unit =
            after_idx < count && unit_word_exact(tokens[after_idx].norm, &junk_mult);

        // A refinement flagged as a
        // known limitation initially: "one hour 5
        // people" must NOT read as 1h05m dropping "people" -- the trailing
        // number binds ONLY if it is utterance-final or everything after it
        // is pure connective glue (filler/joiner/approx). A real, unclaimed
        // CLS_WORD (or another number) after it means the trailing digits
        // belong to the NAME, not this rule -- "one hour 5 people" then
        // resolves to the base "one hour" match (3600s) with "5 people" left
        // as residue. "una hora veinte" (nothing follows) and "una hora
        // veinte para el te" ("para" is a filler, but "el"/"te" are not, so
        // the guard fails there too, same outcome: 1h + residue) are the
        // pinned regression cases for this guard (test_hours_trailing_minutes
        // residue-guard cases) -- the same guard now also applies at the
        // minute level (structurally, no extra code -- it's the same `for`
        // loop over the same `after_idx`).
        bool trailing_ok = true;
        for (int k = after_idx; k < count; k++) {
          if (tokens[k].cls != CLS_FILLER && tokens[k].cls != CLS_JOINER &&
              tokens[k].cls != CLS_APPROX) {
            trailing_ok = false;
            break;
          }
        }

        // The trailing number must be a bare 0-59 sub-count with no unit of
        // its own -- a unit-bearing trailing number ("... twenty minutes")
        // is a normal compound sum (see the compound-sum rule above), not
        // this rule's shape.
        if (run > 0 && mval >= 0 && mval <= 59 && !trailing_has_unit && trailing_ok) {
          duration_s = unit_n * unit_mult + mval * (unit_mult / 60);
          has_duration = true;
          for (int k = next_idx; k < next_idx + run; k++) consumed[k] = true;
          if (match_start < 0 || unit_idx_trail < match_start) match_start = unit_idx_trail;
          match_end = next_idx + run - 1;
        }
      }
    }
  }

  // -- Cheap tier: spoken M:SS pair -----------------------------------------
  // Fixes the "seis cuarenta y cinco" bug: a unit-LESS utterance of exactly
  // two adjacent number groups is read as a spoken M:SS clock time
  // (group1*60 + group2). Gated so it can NEVER touch a case the existing
  // grammar already handles: only when the search above found nothing, no
  // UNIT token exists anywhere (the exact boundary vs every unit-bearing
  // phrasing), and the magnitude guard hasn't already fired. Also gated on
  // !ct_matched: this is the exact rule the clock-target discriminator must
  // run ahead of -- "a las seis cuarenta y cinco" must never fall through
  // here and silently resolve as a 405s DURATION instead of the clock
  // target it actually is. Also gated on !hundred_present for the same
  // defense-in-depth reasoning as every other rule -- kept uniform with
  // every other duration rule's own gate. (At one point this gate was
  // provably unreachable here: an earlier hundred-class composer required a
  // UNIT token, and this rule requires zero, so the two conditions
  // couldn't overlap. Once that composer was removed for size -- see
  // HUNDRED_WORDS_BLOB's own doc comment -- that specific reachability
  // argument no longer applies; the gate itself is unchanged and still
  // correctly refuses any hundred-word utterance regardless.)
  if (!ct_matched && !has_duration && !magnitude_adjacent && !hundred_present) {
    bool any_unit = false;
    for (int i = 0; i < count; i++) {
      if (tokens[i].cls == CLS_UNIT) { any_unit = true; break; }
    }

    if (!any_unit) {
      // Collect number GROUPS left-to-right (each a maximal run resolved by
      // nl_words_to_number) -- only the first two are kept; a third makes
      // ngroups > 2 below, which is enough to reject "too many groups"
      // without tracking their positions.
      int g_start[2] = {-1, -1};
      int g_end[2] = {-1, -1};
      int32_t g_val[2] = {0, 0};
      int ngroups =
          collect_first_two_number_groups(tokens, normptrs, count, NULL, g_start, g_end, g_val);

      if (ngroups == 2) {
        // This used to be "only CLS_JOINER/CLS_FILLER
        // tokens lie between group-1's end and group-2's start" -- tolerant
        // enough that "between 3 and 4"/"3 and 4"/"de 5 a 10"/"somewhere
        // between 20 and 30" (a genuine unit-less RANGE, "between"/"and"/
        // "de...a" all classify JOINER or FILLER) silently resolved as a
        // spoken M:SS pair (e.g. "3 and 4"->184s) -- the range veto (below)
        // never caught this because its own veto is keyed off the UNIT-bearing
        // duration search's own match_start semantics, a structurally
        // different shape (the range veto's winning match is the SECOND
        // bound alone, with the true first bound sitting OUTSIDE it; here
        // the winning match spans BOTH numbers, so that veto's own
        // "connective precedes match_start" check doesn't apply the same
        // way -- this needs its own, narrower rule). Tightened to a real spoken-pair's own actual
        // shape: the two number groups must be STRICTLY ADJACENT, nothing
        // at all between them (matches every real spoken-pair phrasing, e.g.
        // "6 45", "seis cuarenta y cinco" -- neither has any token between
        // its two groups) -- any connective at all between two bare numbers
        // means they're being related (a range/list), not spoken together
        // as one M:SS pair.
        bool adjacent = (g_start[1] == g_end[0] + 1);

        // Even a strictly-adjacent pair ("5 6 pm", no joiner at
        // all) must still decline if it's the two-plausible-hours-plus-
        // trailing-meridiem shape two_hour_pair_ambiguous already guards on
        // the clock side -- otherwise the clock discriminator correctly
        // refuses (see match_clock_target) but this duration fallback would
        // still silently fabricate "5 6 pm"->306s.
        if (adjacent && two_hour_pair_ambiguous(tokens, normptrs, count)) adjacent = false;

        // Decline when group-1 is
        // immediately preceded by a definite article -- "the twenty
        // twenty"/"the six forty five" is a named entity (T20 cricket, a
        // train), never a duration.
        bool preceded_by_article =
            g_start[0] > 0 && exact_match_blob(tokens[g_start[0] - 1].norm,
                                               DEFINITE_ARTICLE_WORDS_BLOB,
                                               sizeof(DEFINITE_ARTICLE_WORDS_BLOB));

        if (!preceded_by_article && adjacent && g_val[0] >= 0 && g_val[0] <= 99 &&
            g_val[1] >= 0 && g_val[1] <= 59) {
          duration_s = g_val[0] * 60 + g_val[1];
          has_duration = true;
          for (int k = g_start[0]; k <= g_end[1]; k++) consumed[k] = true;
          match_start = g_start[0];
          match_end = g_end[1];
        }
      }
    }
  }

  // -- Fractions engine pattern 1: <fraction> [connectives] <unit> --------
  // "media hora"->1800, "un cuarto de hora"->900, "tres cuartos de
  // hora"->2700, "half an hour"->1800, "quarter of an hour"->900, "three
  // quarters of an hour"->2700. Fallback only (has_duration==false and
  // !magnitude_adjacent, the same posture as every other rule in this file);
  // the exactness guard ((fnum*mult) % fden == 0) is what makes "no rounding
  // policy needed" a structural fact rather than an assumption -- every
  // matrix product happens to land on an integer number of seconds, and this
  // guard would decline (fail-soft) rather than silently truncate if some
  // future unit/fraction combination didn't. Also gated on !ct_matched and
  // !hundred_present like every duration rule below the discriminator.
  if (!ct_matched && !has_duration && !magnitude_adjacent && !hundred_present) {
    for (int i = 0; i < count; i++) {
      if (consumed[i]) continue;
      int32_t fnum, fden;
      int ftoks;
      if (!match_fraction_at(tokens, normptrs, count, i, &fnum, &fden, &ftoks)) continue;

      int after = i + ftoks;
      while (after < count &&
             (tokens[after].cls == CLS_FILLER || tokens[after].cls == CLS_JOINER)) {
        after++;
      }
      // NOTE: no `consumed[after]` check here -- see the CLS_UNIT case arm's
      // comment near the top of parse_nl() for the convention this relies
      // on (a CLS_UNIT token is always pre-marked consumed regardless of
      // use; check unit_word_exact() directly, never consumed[]).
      if (after >= count) continue;
      int32_t mult;
      if (!unit_word_exact(tokens[after].norm, &mult)) continue;

      int64_t numer = (int64_t)fnum * mult;
      if (numer % fden != 0) continue;  // non-integer result -- decline, stay fail-soft

      duration_s = (int32_t)(numer / fden);
      has_duration = true;
      for (int k = i; k < after; k++) consumed[k] = true;
      consumed[after] = true;
      match_start = i;
      match_end = after;
      break;
    }
  }

  // -- Fractions engine patterns 2/3: <unit> y
  // media / <N> <unit> y medio (+ en equivalents) -------------------------
  // A trailing HALF-word right after an already-resolved unit adds half a
  // unit to the duration. Two shapes share this one code path: pattern 3,
  // explicit N -- the main N-U search already matched a single (non-
  // compound) pair ("cinco minutos y medio"->330, "two minutes and a
  // half"->150); pattern 2, implicit N=1 -- no match exists yet, but a bare
  // unit token is present with no preceding number ("hora y media"->5400,
  // "an hour and a half"->5400, "a minute and a half"->90). The
  // `duration_s % mult == 0` guard defensively keeps this from mis-firing
  // onto a duration pattern 1 (above) already produced from a FRACTION
  // (e.g. "media hora"'s 1800s is not a whole multiple of its own 3600s
  // unit) -- not reachable by any of this app's known supported phrasings
  // (pattern 1's own match never leaves a trailing HALF-word anywhere in
  // those), kept as a structural belt-and-suspenders rather than relying
  // on that being true forever: it IS reachable, and load-bearing, for
  // adversarial input outside those known phrasings -- "media hora y media"
  // stays correctly at 1800s (not silently doubled to 2700s) precisely
  // because this guard declines when pattern 1 already produced a
  // non-whole-unit duration. `mult % 2 == 0` avoids a non-integer
  // half-second (also not reachable by any known supported phrasing -- every
  // fraction example here is minute/hour, both even). Also gated on
  // !ct_matched and !hundred_present, like every duration rule
  // below the discriminator.
  if (!ct_matched && !magnitude_adjacent && !hundred_present) {
    int frac_unit_idx = -1;
    int32_t base_n = 0;
    int32_t frac_mult = 0;

    if (has_duration && !used_compound && tokens[match_end].cls == CLS_UNIT &&
        unit_word_exact(tokens[match_end].norm, &frac_mult) &&
        duration_s % frac_mult == 0) {
      frac_unit_idx = match_end;
      base_n = duration_s / frac_mult;
    } else if (!has_duration) {
      // find_bare_qualifying_unit() -- shared
      // scan with the trailing sub-count block above.
      frac_unit_idx = find_bare_qualifying_unit(tokens, count, /*require_mult60=*/false, &frac_mult);
      if (frac_unit_idx >= 0) {
        base_n = 1;  // implicit count -- bare unit, no preceding number
      }
    }

    if (frac_unit_idx >= 0 && frac_mult % 2 == 0) {
      int after = frac_unit_idx + 1;
      while (after < count &&
             (tokens[after].cls == CLS_FILLER || tokens[after].cls == CLS_JOINER)) {
        after++;
      }
      if (after < count && !consumed[after] &&
          exact_match_blob(tokens[after].norm, HALF_WORDS_BLOB, sizeof(HALF_WORDS_BLOB))) {
        duration_s = base_n * frac_mult + frac_mult / 2;
        has_duration = true;
        for (int k = frac_unit_idx + 1; k <= after; k++) consumed[k] = true;
        consumed[frac_unit_idx] = true;
        if (match_start < 0 || frac_unit_idx < match_start) match_start = frac_unit_idx;
        match_end = after;
      }
    }
  }

  // The hundred-class
  // COMPOSER ("ciento veinte segundos"->120, "two hundred seconds"->200,
  // "one hundred twenty minutes"->7200, incl. its trailing-subcount/half
  // extension parity and the "couple of hundred X" filler skip) is REMOVED
  // -- 1,063 B ARM cost for a small number of long-tail hundred-word
  // phrasings, worst bytes-per-phrasing of anything removed to reclaim size,
  // a deliberate capability trade for permanent size headroom. The
  // `hundred_present` GUARD (computed above, before the main
  // N-U search) is DELIBERATELY KEPT: every duration rule in this file,
  // including the main search, is already gated `!hundred_present`, so with
  // no composer left to satisfy it, any utterance containing an unconsumed
  // HUNDRED_WORDS token now falls through every rule and REFUSES (fail-soft)
  // instead of the guard's whole reason for existing -- silently matching
  // some OTHER, unrelated number+unit pair and misreading "ciento veinte
  // segundos" as "veinte segundos"=20s. No new code needed here: the guard
  // alone is sufficient to keep the fail-soft contract once the composer is
  // gone.

  // A bare en indefinite article "a"/"an"
  // immediately before an otherwise-unclaimed CLS_UNIT token is the
  // implicit-count-1 idiom ("an hour"->3600, "a minute"->60, "for an
  // hour"->3600) -- closes the en/es symmetry hole (es "un"/"una" are
  // already NUMBER words and already work: "un minuto"->60). Built as its
  // OWN last-resort fallback, gated on !has_duration same as every other
  // fallback below the main search, rather than a blanket token
  // reclassification -- an earlier draft coerced "a"/"an" to CLS_NUMBER=1
  // unconditionally whenever it preceded a UNIT, which silently broke "half
  // an hour" and "quarter of an hour": the fraction engine's own "an" is
  // FILLER, deliberately skipped by its own FILLER|JOINER loop to reach the
  // unit past it; reclassifying it to a NUMBER blocked that skip (a literal
  // "1" token isn't a unit word either), so those patterns declined and fell
  // through to a wrong plain "an hour"=3600 match instead of the correct
  // fraction. Running this ONLY as a last-resort, has_duration==false
  // fallback means the fraction/cheap-tier/hundred rules above always see "a"/
  // "an" as ordinary, unmodified FILLER first, exactly as before this pass --
  // this block only ever engages once every one of them has already
  // declined. No `consumed[]` check needed on either token: CLS_FILLER and
  // CLS_UNIT are BOTH unconditionally pre-marked consumed by the
  // classification switch above regardless of use (the same convention
  // documented at that switch's own CLS_UNIT case) -- call unit_word_exact()
  // directly, matching every other fraction/unit rule in this file.
  if (!ct_matched && !has_duration && !magnitude_adjacent && !hundred_present) {
    for (int i = 0; i < count; i++) {
      if (tokens[i].cls != CLS_FILLER) continue;
      if (strcmp(tokens[i].norm, "a") != 0 && strcmp(tokens[i].norm, "an") != 0) continue;
      // Collision protection (test_once_en_es_collision): "once a minute" must
      // stay fail-soft -- an "a"/"an" directly preceded by a NUMBER token
      // ("once"=11) is the English frequency idiom "once a ___", not this
      // implicit-count-1 idiom; excluding that adjacency is what keeps the
      // two apart (mirrors the main N-U search's own couple-vs-once
      // reasoning just above it).
      if (i > 0 && tokens[i - 1].cls == CLS_NUMBER) continue;
      int unit_idx = i + 1;
      int32_t mult;
      if (unit_idx >= count || !unit_word_exact(tokens[unit_idx].norm, &mult)) continue;
      duration_s = mult;
      has_duration = true;
      match_start = i;
      match_end = unit_idx;
      break;
    }
  }

  // Kind is resolved by
  // CLAUSE-SCOPED ADJACENCY, not an anywhere-in-utterance scan (an earlier
  // approach) nor an exactly-one-token adjacency check (a later approach
  // that regressed "repeat for five minutes"/"repeat timer for five
  // minutes", since a single filler/kind token between the keyword and the
  // match broke a one-slot window). The correct window is the contiguous run of
  // already-consumed CONNECTIVE tokens on either side of the winning match:
  // walk outward from the match through FILLER/JOINER/UNIT/KIND tokens
  // (transparent -- grammatical glue), stopping at the first CLS_WORD or
  // unconsumed CLS_NUMBER (opaque -- a name/clause boundary). A kind keyword
  // found anywhere in that walk governs the kind; precedence
  // STOPWATCH > REPEAT > COUNTDOWN (default) when more than one is found.
  // This is structural (token classes/positions only, never specific
  // words) -- it cannot "miss a word" the way a blocklist can; the only
  // maintenance surface is the FILLER_WORDS minimality invariant (see the
  // comment on that table). With no duration match at all, there is
  // nothing to walk from, so the keyword governs if present anywhere (a
  // bare "stopwatch"/"cronometro" utterance has no other anchor).
  //
  // A clock target is
  // COUNTDOWN-ONLY, unconditionally -- a stopwatch has no duration concept
  // at all (nothing to count down TO), and KIND_REPEAT in this app means
  // "repeat every duration_s", which a wall-clock instant is not (the
  // leftover-today-until-6am seconds are not a sane recurring interval).
  // Rather than fail the whole parse when a REPEAT/STOPWATCH keyword
  // happens to co-occur with a clock target ("repeat at six"), this forces
  // COUNTDOWN and lets the kind keyword fall through as ordinary consumed-
  // but-ignored content (it was already marked consumed by the
  // classification loop's own switch, above, regardless of this branch) --
  // the confirm screen still shows the correctly-computed target, visible
  // and one Edit-on-dial repeat-toggle away from what the user actually
  // wanted, which is strictly safer than either fail-soft (losing the
  // parsed target entirely) or silently inventing a REPEAT interval from a
  // clock target (a genuinely wrong number). The clause-scoped kind-resolution
  // walk itself is skipped entirely for a CT match -- it would otherwise fall
  // into the has_duration==false "anywhere-in-utterance" branch below
  // (has_duration stays false for a CT match; lib/parse_nl never resolves
  // it), which is exactly the un-scoped scan this walk replaced for the
  // duration case.

  // A duration RANGE ("5 to 10 minutes"/
  // "5 or 10 minutes"/"between 5 and 10 minutes"/"de 5 a 10 minutos") must
  // never resolve to either endpoint -- reuses the winning plain N-U pair
  // match's own `match_start` directly (no separate group-collector scan
  // needed): the token right before it is a range connective (en "to"/
  // "or", es "a", or en "and" preceded by "between"), and the token before
  // THAT is itself a NUMBER -- i.e. the winning pair's own first number is
  // really a range's second bound, and the real first bound sits just
  // outside the match. Vetoes has_duration exactly like magnitude_
  // adjacent/hundred_present veto their own families, skipped when the
  // clock discriminator already won. Narrower than a full 2-group collector
  // (assumes the range's first bound is a single token, true of every
  // supported case this closes: "5"/"10", never a multi-word number) --
  // a deliberate byte-budget tradeoff given this file's tight size wall.
  if (!ct_matched && has_duration && match_start >= 2) {
    const char *rc = tokens[match_start - 1].norm;
    bool is_range = exact_match_blob(rc, RANGE_CONN_WORDS_BLOB, sizeof(RANGE_CONN_WORDS_BLOB));
    if (!is_range && strcmp(rc, "and") == 0 && match_start >= 3 &&
        strcmp(tokens[match_start - 3].norm, "between") == 0) {
      is_range = true;
    }
    if (is_range && tokens[match_start - 2].cls == CLS_NUMBER) {
      has_duration = false;
      duration_s = 0;
    }
  }

  // Guarded default-minutes for a unit-less
  // imperative numeral ("ponme veinte"->1200, "dale 15"->900). Last-resort
  // fallback -- every earlier rule (main search, colon/slash/decimal,
  // trailing-subcount/half-word/spoken-pair, hundred-composite) has already
  // tried and failed (!has_duration), and the
  // range veto (immediately above) has already run -- a vetoed range
  // cannot be rescued here (it needs zero UNIT tokens AND exactly one number
  // group; a range has a unit + two groups). This rule declines entirely
  // (stays fail-soft) unless BOTH: (a) a CREATE_CUE_WORDS_BLOB token is
  // present ANYWHERE (checked by norm text, regardless of class/consumed --
  // an explicit create/imperative cue is what makes turning a bare number
  // into a duration defensible, since the confirm screen still shows the
  // user the resulting value before it's used), and (b) EXACTLY ONE
  // unconsumed CLS_NUMBER group exists in the whole utterance, value 1..99,
  // with NO CLS_UNIT token anywhere (a unit-bearing number belongs to an
  // earlier rule that would already have matched; a SECOND number group
  // means this is the "the twenty twenty" pair shape, a separate rule's own
  // concern, not this rule's). Absent a cue, a bare
  // number is unchanged (still fail-soft) -- "veinte" alone stays declined.
  if (!ct_matched && !has_duration && !magnitude_adjacent && !hundred_present) {
    bool has_cue = false;
    for (int i = 0; i < count && !has_cue; i++) {
      if (exact_match_blob(tokens[i].norm, CREATE_CUE_WORDS_BLOB, sizeof(CREATE_CUE_WORDS_BLOB))) {
        has_cue = true;
      }
    }

    bool any_unit = false;
    for (int i = 0; i < count && !any_unit; i++) {
      if (tokens[i].cls == CLS_UNIT) any_unit = true;
    }

    if (has_cue && !any_unit) {
      int g_start = -1, g_end = -1;
      int32_t g_val = 0;
      int ngroups = 0;
      int i = 0;
      while (i < count) {
        if (consumed[i] || tokens[i].cls != CLS_NUMBER) {
          i++;
          continue;
        }
        int run = 0;
        int32_t val = nl_words_to_number((const char *const *)&normptrs[i], count - i, &run);
        if (run <= 0) {
          i++;
          continue;
        }
        if (ngroups == 0) {
          g_start = i;
          g_end = i + run - 1;
          g_val = val;
        }
        ngroups++;
        i += run;
      }

      if (ngroups == 1 && g_val >= 1 && g_val <= 99) {
        // This rule's original guard set had a real SILENT-WRONG gap:
        // the rule's own framing is "a unit-less IMPERATIVE numeral" (i.e.
        // the whole utterance reduces to just the cue + the bare number),
        // but as originally written, this rule only checked that a cue is
        // present ANYWHERE and that exactly one bare number group exists --
        // it never checked whether anything ELSE is left over. Two real
        // phrasing classes exploit that gap: (a) Spanish "un"/"una" is BOTH
        // the NUMBER-ONE word AND the indefinite article ("a"/"an") -- "pon
        // una alarma" = "set AN alarm", "un toque"/"un cacho" = vague-
        // duration idioms already documented at NAMEDROP_WORDS_BLOB's own
        // comment as a deliberately-excluded ambiguous-negative case, "una
        // vuelta con el cronometro" = "a lap with the stopwatch", which also
        // silently clobbered an otherwise-correct KIND_STOPWATCH resolution;
        // (b) an EN alarm/timer phrase whose clock-target reading failed the
        // discriminator (unsupported vocab: "quarter past"/"half seven"/
        // "o'clock"/"elevenses", or a non-time quantity: cricket "overs")
        // leaves exactly one bare hour/count NUMBER behind, which this rule
        // then wrongly reads as "N minutes" -- a class of case that was
        // previously safely FAIL-GAP/FAIL-ACCEPT, now SILENT-WRONG. Both
        // classes share one signal: real, unclaimed content survives
        // OUTSIDE the winning number group besides the cue itself -- a
        // genuine "set N" / "dale N" imperative never has any. Guard:
        // decline unless every token outside the number group is either
        // already consumed as pure glue (FILLER/JOINER/APPROX/NAMEDROP/UNIT
        // -- moot here since !any_unit already holds) or is itself the
        // closed CREATE_CUE_WORDS_BLOB cue -- a KIND keyword (COUNTDOWN/
        // STOPWATCH/REPEAT, e.g. "alarma"/"cronometro") is pre-consumed like
        // FILLER but is real semantic content, not glue, so it does NOT
        // satisfy this and still counts as residue. Costs zero true-positive
        // coverage ("ponme veinte"/"dale 15"/"pone 20"/"dale 10" all reduce
        // to exactly cue+number, nothing else) and fixes every one of the
        // cases described above.
        // The "or is itself the closed CREATE_CUE_WORDS_BLOB
        // cue" half of the comment above was documented but never actually
        // checked here -- 8 of the 9 cue words happen to already be
        // pre-consumed via FILLER_WORDS_BLOB/NAMEDROP_WORDS_BLOB membership
        // (so `!consumed[k]` alone was enough for them), but "echa" is not in
        // either table, so it stayed unconsumed CLS_WORD and registered as
        // residue -- both as the sole cue ("echa 20" always declined) and as
        // a co-occurring cue (an unconsumed "echa" poisoned the guard even
        // when another, working cue was also present). Checking
        // CREATE_CUE_WORDS_BLOB membership directly (matching the comment
        // literally) fixes "echa" uniformly with the other 8 cues without
        // adding it to FILLER/NAMEDROP, which would also make it transparent
        // to the clause-scoped kind-resolution walk and every other rule's
        // name residue -- a broader side effect this guard-local fix
        // doesn't need. No-op for
        // the other 8 cues (already `!consumed[k] == false` on their own).
        bool has_residue_outside_number = false;
        for (int k = 0; k < count; k++) {
          if (k >= g_start && k <= g_end) continue;  // the winning number group itself
          bool is_kind = tokens[k].cls == CLS_KIND_COUNTDOWN || tokens[k].cls == CLS_KIND_STOPWATCH ||
                          tokens[k].cls == CLS_KIND_REPEAT;
          bool is_cue = exact_match_blob(tokens[k].norm, CREATE_CUE_WORDS_BLOB,
                                          sizeof(CREATE_CUE_WORDS_BLOB));
          if ((!consumed[k] && !is_cue) || is_kind) {
            has_residue_outside_number = true;
            break;
          }
        }
        if (!has_residue_outside_number) {
          duration_s = g_val * 60;
          has_duration = true;
          for (int k = g_start; k <= g_end; k++) consumed[k] = true;
          match_start = g_start;
          match_end = g_end;
        }
      }
    }
  }

  // Correction / last-value-wins, built last as the riskiest sub-item here.
  // Only applies to a SIMPLE (non-chained) main N-U pair match
  // (`simple_pair_matched`) -- every case this closes is a plain
  // "N U" pair followed by a correction. Scans forward from the winning
  // match's own end for a RUN of one-or-more CORRECTION_MARKER_WORDS_BLOB
  // tokens, then requires an immediately following (no gap) CLS_NUMBER
  // group -- either with its OWN trailing unit (must be the SAME multiplier
  // as the original match -- "of the same unit context"; a differing
  // unit declines, leaving the original value standing rather than
  // guessing), or bare (a "trailing unit re-binds" case --
  // reuses `last_match_mult`). The later value always wins outright (never
  // summed/averaged); the earlier number-group is already consumed by the
  // original match (nothing to additionally drop), and the marker run + new
  // number(+unit) are consumed here so the correction phrase leaves no
  // residue.
  //
  // Deliberately does NOT touch the already-working adjacent form ("cinco,
  // no, diez minutos"->600) -- that case has NO marker after ITS OWN winning
  // match's end (the marker sits before the winning "diez minutos" pair,
  // which the ordinary first-match-wins search already found on its own),
  // so this block's forward-only scan never engages it.
  //
  // Deliberately scoped to a single forward scan, one correction only -- a
  // SECOND correction (e.g. "treinta segundos no treinta segundos no espera
  // un minuto") rebinds once and stops, not chaining further; a consumed
  // NAMEDROP token sitting between the match and the marker breaks the
  // marker run (the run requires !consumed[mk]). Both are out of scope by
  // design -- there are known real-world phrasings this leaves unconverted.
  if (!ct_matched && has_duration && simple_pair_matched && !used_compound) {
    int mk = match_end + 1;
    bool saw_marker = false;
    while (mk < count && !consumed[mk] &&
           exact_match_blob(tokens[mk].norm, CORRECTION_MARKER_WORDS_BLOB,
                             sizeof(CORRECTION_MARKER_WORDS_BLOB))) {
      saw_marker = true;
      mk++;
    }

    if (saw_marker && mk < count && !consumed[mk] && tokens[mk].cls == CLS_NUMBER) {
      int run = 0;
      int32_t newval = nl_words_to_number((const char *const *)&normptrs[mk], count - mk, &run);
      if (run > 0) {
        int after = mk + run;
        int32_t total_new = 0;
        bool have_new = false;
        bool differing_unit = false;
        int end_new = after - 1;

        // A real CLS_UNIT token is ALWAYS pre-consumed at
        // classification time regardless of match (parse_nl.c's own
        // consumed[]-marking switch, whose documented convention is:
        // "call unit_word_exact() directly on the token instead" of gating
        // on `!consumed[after]`, which "is always true for a CLS_UNIT token
        // by construction"). The original code gated this check on
        // `!consumed[after]`, so it could never see a real trailing unit
        // word at all -- it always fell through to the bare-reuse branch
        // below, silently reusing `last_match_mult` against the NEW value
        // even when a DIFFERENT unit was stated (e.g. "two hours actually
        // ten minutes" computed 10*3600=36000, 10 HOURS, with zero name
        // residue, when the user said "ten minutes"). Checking
        // unit_word_exact() directly (no consumed[] gate) now correctly
        // detects the trailing unit's presence and multiplier either way.
        if (after < count) {
          int32_t mult2;
          if (unit_word_exact(tokens[after].norm, &mult2)) {
            if (mult2 == last_match_mult) {
              total_new = newval * mult2;
              have_new = true;
              end_new = after;
            } else {
              // A differing unit declines the whole rebind (per this
              // block's own doc comment above, "a differing unit declines,
              // leaving the original value standing rather than guessing")
              // instead of falling through to the bare-reuse branch below,
              // which must fire only when NO unit token follows the
              // corrected number at all.
              differing_unit = true;
            }
          }
        }
        if (!have_new && !differing_unit) {
          total_new = newval * last_match_mult;
          have_new = true;
        }

        if (have_new && total_new >= 1 && total_new <= (int32_t)TIMER_MAX_DURATION_S) {
          duration_s = total_new;
          for (int k = match_end + 1; k <= end_new; k++) consumed[k] = true;
          match_end = end_new;
        }
      }
    }
  }

  bool stopwatch_wins = false;
  bool repeat_wins = false;
  if (ct_matched) {
    // kind forced to KIND_COUNTDOWN below; stopwatch_wins/repeat_wins stay
    // false so neither branch of the kind-assignment if/else chain (right
    // below) fires for a CT match -- it falls to the final `else`.
  } else if (has_duration) {
    bool s_stopwatch = false;
    bool s_repeat = false;

    for (int i = match_start - 1; i >= 0; i--) {
      NlTokClass c = tokens[i].cls;
      if (c == CLS_KIND_STOPWATCH) { s_stopwatch = true; continue; }
      if (c == CLS_KIND_REPEAT) { s_repeat = true; continue; }
      if (c == CLS_KIND_COUNTDOWN) continue;
      if (c == CLS_FILLER || c == CLS_JOINER || c == CLS_UNIT) continue;
      break;  // CLS_WORD or an unconsumed CLS_NUMBER -- name/clause boundary
    }
    for (int i = match_end + 1; i < count; i++) {
      NlTokClass c = tokens[i].cls;
      if (c == CLS_KIND_STOPWATCH) { s_stopwatch = true; continue; }
      if (c == CLS_KIND_REPEAT) { s_repeat = true; continue; }
      if (c == CLS_KIND_COUNTDOWN) continue;
      if (c == CLS_FILLER || c == CLS_JOINER || c == CLS_UNIT) continue;
      break;
    }

    stopwatch_wins = s_stopwatch;
    repeat_wins = !stopwatch_wins && s_repeat;
  } else {
    for (int i = 0; i < count; i++) {
      if (tokens[i].cls == CLS_KIND_STOPWATCH) stopwatch_wins = true;
    }
    if (!stopwatch_wins) {
      for (int i = 0; i < count; i++) {
        if (tokens[i].cls == CLS_KIND_REPEAT) repeat_wins = true;
      }
    }
  }

  if (stopwatch_wins) {
    out->kind = KIND_STOPWATCH;
    // "stopwatch 5 minutes": the kind wins outright -- ALL number tokens
    // (not just the ones in the winning match, if any) are dropped, never
    // left over as name residue ("kind+number conflict").
    for (int i = 0; i < count; i++) {
      if (tokens[i].cls == CLS_NUMBER) consumed[i] = true;
    }
    has_duration = false;
    duration_s = 0;
  } else if (repeat_wins) {
    out->kind = KIND_REPEAT;
  } else {
    out->kind = KIND_COUNTDOWN;
  }

  // "ALARM-words -> COUNTDOWN mapping": a bare NUMBER token with value 1
  // ("un"/"una"/"uno"/"one"/a
  // literal "1") immediately followed by a KIND keyword is structurally
  // always the indefinite article ("a timer"/"un temporizador"/"an
  // alarm"/"una alarma"), never a real quantity -- this app has no "create
  // N timers from one utterance" concept, so there is no legitimate
  // reading where a number sits directly in front of a kind word and means
  // anything else. English "a"/"an" are already FILLER_WORDS and never
  // reach here; Spanish "un"/"una"/"uno" (and English "one") are ALSO valid
  // NUMBER words, so without this an unclaimed one leaks into the name
  // residue verbatim ("ponme una alarma de cinco minutos" -> name "una"
  // instead of ""). The `stopwatch_wins` branch above already drops every
  // stray NUMBER token for the same underlying reason ("kind+number
  // conflict") -- this extends the same structural treatment to
  // COUNTDOWN/REPEAT, narrowed to the one case that's actually ambiguous
  // (value==1, immediately before a kind word) rather than broadly dropping
  // every unclaimed number the way the stopwatch case does (safe there only
  // because a stopwatch utterance has no duration concept at all).
  for (int i = 0; i < count; i++) {
    if (consumed[i] || tokens[i].cls != CLS_NUMBER) continue;
    if (i + 1 >= count) continue;
    NlTokClass next_cls = tokens[i + 1].cls;
    if (next_cls != CLS_KIND_STOPWATCH && next_cls != CLS_KIND_REPEAT &&
        next_cls != CLS_KIND_COUNTDOWN) {
      continue;
    }
    int val;
    bool is_one = is_all_digits(tokens[i].norm)
                      ? (strcmp(tokens[i].norm, "1") == 0)
                      : (number_word_exact(tokens[i].norm, &val) && val == 1);
    if (is_one) consumed[i] = true;
  }

  if (has_duration && (duration_s <= 0 || duration_s > TIMER_MAX_DURATION_S)) {
    has_duration = false;
    duration_s = 0;
  }

  out->duration_s = has_duration ? duration_s : 0;
  out->has_duration = has_duration;
  out->clock = clock_out;  // {0} unless ct_matched -- has_duration stays false either way

  // A residue consisting ONLY of
  // CLS_NUMBER tokens (word-form or digit, no real CLS_WORD token anywhere
  // in it) is a missed time component, not a name -- the original "6
  // minutos 45" bug was this class, one level removed
  // (the trailing "45" not binding via any duration rule and instead
  // leaking into the name verbatim). Naming a timer "45" (or "sesenta",
  // or a bare "six") is silent-wrong metadata: it LOOKS like a deliberate
  // user-given name but is really just an unbound number, so drop it
  // rather than surface it -- a fail-soft/absent name is recoverable via
  // "Add name"; a bogus one Timer Started with is a silently wrong artifact
  // the user didn't ask for.
  // Rescoped from `has_duration`
  // alone to `has_duration || out->clock.has_clock_target` -- a recognized
  // clock target is ALSO a VOICE_OK parse (NlParse.ok) whose
  // name/has_name is read by screen_voice.c on the exact same path
  // (build_rows' "Add name"/"Rename" row, the drawn name line, and
  // do_start()'s store_set_name() on Start) even though has_duration stays
  // false for CT by design (lib/parse_nl never resolves a clock target into
  // a duration itself). Gating this drop on `has_duration` alone silently
  // exempted every CT parse from the exact class of bug this drop exists to
  // close: leftover unclaimed digits ("5 a las seis", "6 minutos 45 a las
  // seis") got persisted verbatim as the created timer's name if the user
  // pressed Start without editing. The scan below is otherwise unchanged --
  // it already has no other has_duration dependency, and still only fires
  // when EVERY unconsumed token is CLS_NUMBER; a MIXED residue (>=1 real
  // CLS_WORD token alongside the numbers, e.g. "5 people"/"5 rounds"/
  // "mil veinte" -- "mil" is a MAGNITUDE_WORDS token, never CLS_NUMBER, see
  // that blob's own doc comment) is untouched either way, clock target or
  // not.
  bool residue_all_numeric = has_duration || out->clock.has_clock_target;
  if (residue_all_numeric) {
    bool any_residue = false;
    for (int i = 0; i < count; i++) {
      if (consumed[i]) continue;
      any_residue = true;
      if (tokens[i].cls != CLS_NUMBER) {
        residue_all_numeric = false;
        break;
      }
    }
    residue_all_numeric = residue_all_numeric && any_residue;
  }

  size_t name_len = 0;
  out->name[0] = '\0';
  for (int i = 0; i < count && !residue_all_numeric; i++) {
    if (consumed[i]) continue;
    if (name_len > 0 && name_len + 1 < sizeof(out->name)) out->name[name_len++] = ' ';
    size_t tlen = strlen(tokens[i].raw);
    size_t space_left = sizeof(out->name) - 1 - name_len;
    if (tlen > space_left) tlen = space_left;
    memcpy(out->name + name_len, tokens[i].raw, tlen);
    name_len += tlen;
    out->name[name_len] = '\0';
    if (name_len + 1 >= sizeof(out->name)) break;
  }
  out->has_name = name_len > 0;

  // A recognized-but-unresolved
  // clock target is a genuinely valid parse (ok=true) even though
  // has_duration stays false -- the caller (dictation_am.c/gallery.c) is
  // expected to resolve it via nl_clock_target_to_duration() immediately
  // after parse_nl() returns, before treating the result as "ready to
  // Start" (see NlClockTarget's own doc comment).
  out->ok = out->has_duration || out->kind == KIND_STOPWATCH || out->clock.has_clock_target;
  return out->ok;
}

// -- nl_clock_target_to_duration (public) ------------------------------------

// Pure arithmetic, no clock read --
// `now_sod` is the caller's own seconds-of-day (0..86399), supplied by an
// adapter (dictation_am.c) that read the real clock, or a gallery scene's
// fixed fixture value (kept deterministic on purpose). A 12h-ambiguous
// target (hour24_known==false) has two candidate occurrences today (an AM
// slot and a PM slot, `hour % 12` collapsing a spoken "twelve" to the
// correct 0/12 pair); an unambiguous target (hour24_known==true) has one.
// Each candidate's delta-from-now wraps forward by +86400 whenever it is
// <= 0 (already passed today, OR exactly equal to `now_sod` -- a duration
// of 0 is never a valid timer, so "right now" resolves to the NEXT
// occurrence, +24h/+12h, not an instant fire); the smallest positive delta
// among the 1-2 candidates wins (nearest-future, the standard alarm-clock
// convention). Always returns a value in 1..86400. `hour`/`minute` are
// clamped defensively into their documented ranges first (0-23 if
// hour24_known else 1-12; 0-59) so a malformed/adversarial NlClockTarget
// can never produce out-of-bounds arithmetic -- this function must never
// crash, matching every other pure helper in this file.
int32_t nl_clock_target_to_duration(const NlClockTarget *target, int now_sod,
                                     int32_t *out_target_sod) {
  int32_t hour = target->hour;
  int32_t minute = target->minute;
  if (minute < 0) minute = 0;
  if (minute > 59) minute = 59;

  int32_t candidates[2];
  int ncand;
  if (target->hour24_known) {
    if (hour < 0) hour = 0;
    if (hour > 23) hour = 23;
    candidates[0] = hour * 3600 + minute * 60;
    ncand = 1;
  } else {
    if (hour < 1) hour = 1;
    if (hour > 12) hour = 12;
    int32_t h = hour % 12;
    candidates[0] = h * 3600 + minute * 60;         // AM slot
    candidates[1] = (h + 12) * 3600 + minute * 60;  // PM slot
    ncand = 2;
  }

  int32_t best_delta = -1;
  int32_t best_target = candidates[0];
  for (int i = 0; i < ncand; i++) {
    int32_t delta = candidates[i] - (int32_t)now_sod;
    if (delta <= 0) delta += 86400;
    if (best_delta < 0 || delta < best_delta) {
      best_delta = delta;
      best_target = candidates[i];
    }
  }

  if (out_target_sod != NULL) *out_target_sod = best_target;
  return best_delta;
}
