/* strdup is POSIX, not ISO C; request it explicitly so a strict
 * -std=c99/-std=c11 build on glibc still declares it. Must precede any
 * system header include. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>

/* Pedagogical, minimal parser for the k grammar at
 *   https://k.miraheze.org/wiki/Grammar
 * Builds a K-typed AST (lisp-ish), printed in a parse-tree style.
 * Supports: int atoms/vectors, sym atoms/vectors, names, primitive verbs
 * (101/102), adverbs (as sym wrappers), parens, lambdas, indexing. No
 * eval, no float/string, no error recovery.
 *
 * STEP 2 (ksql): this file is kparser.c plus a ksql layer. A query
 *   select/exec/update/delete [phrases] [by ...] from e [where ...]
 * is a fourth NOUN BASE (n : t[E] | (E) | {E} | N | q) that desugars
 * into the ordinary t[E] application shape
 *   (`verb; t; c; b; a)        / t=from, c=where, b=by, a=phrases
 * matching the functional form ?/![t;c;b;a] in Stevan Apter's ksql.k
 * (http://nsl.com/k/ksql.k). Aliases (qty:expr) need no special handling
 * -- they are the existing `:` assignment shape. Every addition over
 * STEP 1 is tagged STEP2 or lives in the `===== STEP 2: ksql =====`
 * block. See KSQL.md.
 *
 * STEP 4 (q): this file is ksqlparser.c plus a tiny q layer. q gives each
 * monadic verb its own name (flip, neg, count, ...), so a bare glyph is
 * dyadic (KV2) and a named monadic scans as a KV1 verb directly from the
 * keyword table. Explicit glyph-colon forms such as +: are also KV1, so the
 * scanner stores one provenance bit to distinguish them from named keywords.
 * The parser's demotion block (which inferred monadic arity from position) is
 * replaced with a hard error: a dyadic glyph in monadic position is rejected,
 * not demoted. See QPARSER.md. */

/* Safety constants. Real K would grow these dynamically; we bound them
 * statically and abort on overflow to keep the pedagogical version short. */
#define MAX_VEC  4096   /* max ints in an int literal, syms in a sym literal,
                         * or exprs separated by ';' inside one (E) clause */
#define MAX_NAME  256   /* max bytes in a single name token */

static _Noreturn void die(const char *msg) {
    fprintf(stderr, "kparser: %s\n", msg);
    exit(1);
}

/* Allocation wrappers: parsers tend to malloc in tight loops; an OOM
 * three layers deep is hard to recover from cleanly. Abort instead. */
static void *xmalloc(size_t n)             { void *p = malloc(n);     if (!p) die("out of memory"); return p; }
static void *xcalloc(size_t n, size_t sz)  { void *p = calloc(n, sz); if (!p) die("out of memory"); return p; }
static void *xrealloc(void *q, size_t n)   { void *p = realloc(q, n); if (!p) die("out of memory"); return p; }
static char *xstrdup(const char *s)        { char *p = strdup(s);     if (!p) die("out of memory"); return p; }

/* ===== K struct =====
 *
 * The value representation below (struct k0, the I/J/S type
 * abbreviations, the kI/kS/kK accessors, the KI/KS type codes, and the
 * ka/ki/ks/ktn constructors) is adapted from KX Systems' kdb+ C header
 * k.h (https://github.com/KxSystems/kdb, c/c/k.h), licensed under the
 * Apache License 2.0. It has been reduced for this parser: the unused
 * union members and the H/E/F types are dropped, G is signed here, the
 * tail is G0[1] rather than a flexible array, and there is no evaluator.
 *
 * Type codes: positive = vector, negative = atom.
 *
 *   KI/-KI  int vector / atom
 *   KS/-KS  sym vector / atom
 *   KL      generic list
 *   KV1     monadic verb (i = index into MONADS)
 *   KV2     dyadic  verb (i = index into DYADS)
 *
 * Names and symbols share the sym type, distinguished by atom vs vector:
 *   name "x"   ->  `x        (sym atom)
 *   sym  "`x"  ->  ,`x       (1-element sym vector, "enlisted")
 *   sym  "`a`b`c" -> `a`b`c
 */

typedef signed char G;
typedef int         I;
typedef long long   J;
typedef char       *S;

#define KL  0
#define KI  6
#define KS  11
#define KV1 101
#define KV2 102

/* m and a are header bytes kept only for layout parity with other K
 * implementations; we don't use them. u carries attribute bits (V_QNAME,
 * V_ELIDED below). r is the refcount used by dec_ref/inc_ref. The union is
 * the classic K idiom: for atoms we use i/s/k directly; for vectors n is
 * the element count and G0[] is a flexible-array tail accessed via the
 * kI/kS/kK casts. */
typedef struct k0 {
    signed char m, a, t;       /* m, a unused here; t = type code */
    G u;                       /* attribute bits (V_QNAME, V_ELIDED) */
    I r;                       /* reference count */
    union {
        I i;                   /* atom: int value, or verb-table index */
        S s;                   /* atom: sym/name string (owned) */
        struct k0 *k;          /* unused in this minimal subset */
        struct { J n; G G0[1]; };  /* vector: n elements at G0[..] */
    };
} *K;

/* Note: anonymous-struct-in-union is C11; gcc/clang accept it in c99 too.
 * If you port to MSVC or pure c99, name the inner struct. */

#define kI(x) ((I*)((x)->G0))   /* int   vector elements */
#define kS(x) ((S*)((x)->G0))   /* sym   vector elements (each owned strdup) */
#define kK(x) ((K*)((x)->G0))   /* list  elements (each owned K) */

static K ka(signed char t) {
    K x = xcalloc(1, sizeof(struct k0));
    x->t = t;
    return x;
}
static K ki(I v)           { K x = ka(-KI); x->i = v; return x; }
static K ks(const char *s) { K x = ka(-KS); x->s = xstrdup(s); return x; }

static K ktn(signed char t, J n) {
    /* Sizeof one element by type. Includes one byte already in struct
     * (the G0[1] tail), so we slightly over-allocate; harmless. */
    size_t elt = (t == KI) ? sizeof(I) :
                 (t == KS) ? sizeof(S) :
                 (t == KL) ? sizeof(K) : sizeof(G);
    K x = xcalloc(1, sizeof(struct k0) + (size_t)n * elt);
    x->t = t; x->n = n;
    return x;
}

static K klist(K *xs, J n) {
    K x = ktn(KL, n);
    for (J i = 0; i < n; i++) kK(x)[i] = xs[i];
    return x;
}

/* ===== ref counting =====
 *
 * Lifetime model: every K is born with r=0 ("one implicit owner"). When a
 * function returns a K, its r stays 0 — the caller becomes the new owner.
 * inc_ref bumps to share; dec_ref releases. dec_ref on r>0 decrements;
 * dec_ref on r=0 means the last owner is gone, so free, recursing into
 * container types so leaves go too.
 *
 * To discard a container while keeping a child (e.g. unwrapping (E) when
 * E has one element), set the corresponding slot to NULL before dec_ref —
 * the recursion will skip the detached child. */

__attribute__((unused))
static K inc_ref(K x) { if (x) x->r++; return x; }

static void dec_ref(K x) {
    if (!x) return;
    if (x->r > 0) { x->r--; return; }
    switch (x->t) {
    case KL:
        for (J i = 0; i < x->n; i++) dec_ref(kK(x)[i]);
        break;
    case KS:
        for (J i = 0; i < x->n; i++) free(kS(x)[i]);
        break;
    case -KS:
        free(x->s);
        break;
    /* -KI, KI, KV1, KV2: no owned pointers, just free the struct. */
    }
    free(x);
}

/* ===== verb tables =====
 *
 * VERB_CHARS doubles as the canonical verb-index table: the position of a
 * verb char in this string is its index into MONADS / DYADS. Stubbed NULL
 * until eval lands. */

static const char VERB_CHARS[] = ":+-*%!&|<>=~,^#_$?@.";
#define NVERBS (sizeof(VERB_CHARS) - 1)

typedef K (*monad_fn)(K);
typedef K (*dyad_fn)(K, K);

static monad_fn MONADS[NVERBS] __attribute__((unused)) = { 0 };
static dyad_fn  DYADS [NVERBS] __attribute__((unused)) = { 0 };

static int verb_index(char c) {
    const char *p = strchr(VERB_CHARS, c);
    return p ? (int)(p - VERB_CHARS) : -1;
}

/* Attribute bits in the u byte, shared across the file series: bit 0 is
 * named-verb provenance (q parsers only), bit 1 marks an elided slot. */
#define V_QNAME  1   /* q named-monadic spelling (flip, neg, count, ...). */
#define V_ELIDED 2   /* this null fills an elided slot; print as nothing */

static K kverb(int monadic, int idx) {
    K x = ka(monadic ? KV1 : KV2);
    x->i = idx;
    return x;
}

static K kverb_qname(int idx) {
    K x = kverb(1, idx);
    x->u |= V_QNAME;
    return x;
}

static int is_q_named_monadic(K x) {
    return x && x->t == KV1 && (x->u & V_QNAME);
}

/* STEP4: the monadic keyword table. Maps each of q's ~20 named monadic
 * verbs to its index in VERB_CHARS (the same index K's demotion would
 * have produced). Only the monadic names need this -- they are the ones
 * that skip the demotion. q's other keywords (sum, avg, lj, ...) are
 * ordinary names and parse as -KS nouns.
 *
 * The array is indexed by VERB_CHARS position minus 1 (skipping ':',
 * which has no named monadic -- '::' is the generic null). So
 * MONADIC_NAMES[0] is the monadic for '+' (flip), [1] for '-' (neg), etc. */
static const char *MONADIC_NAMES[] = {
    "flip",     /* +  */
    "neg",      /* -  */
    "first",    /* *  */
    "reciprocal",/* %  */
    "til",      /* !  */
    "where",    /* &  */
    "reverse",  /* |  */
    "iasc",     /* <  */
    "idesc",    /* >  */
    "group",    /* =  */
    "not",      /* ~  */
    "enlist",   /* ,  */
    "null",     /* ^  */
    "count",    /* #  */
    "floor",    /* _  */
    "string",   /* $  */
    "distinct", /* ?  */
    "type",     /* @  */
    "get"       /* .  */
};
#define NMONADICS (sizeof(MONADIC_NAMES) / sizeof(MONADIC_NAMES[0]))

/* STEP4: is `name` a monadic keyword? If so, return its VERB_CHARS index;
 * otherwise return -1. The index is the position of the glyph in
 * VERB_CHARS, which is the same index K's demotion block would have used
 * to build the KV1 -- so the AST node is identical (KV1 with that index),
 * only the printer renders it by name. */
static int monadic_keyword(const char *name) {
    for (int i = 0; i < (int)NMONADICS; i++) {
        if (strcmp(name, MONADIC_NAMES[i]) == 0)
            return i + 1;   /* +1 to skip ':' at VERB_CHARS[0] */
    }
    return -1;
}

/* STEP4: dyadic keyword table. In real q, lj, bin, wavg etc. are library
 * functions defined in the .q context, not parser primitives.  We include a
 * small representative set to demonstrate that the same nve machinery that
 * handles glyph infix also handles named dyadic verbs — the only difference
 * is the scanner tag.  Indices start at NVERBS so they don't collide with
 * glyph indices; the printer resolves name-vs-glyph by which range i falls
 * in. */
static const char *DYADIC_NAMES[] = {
    "lj",
    "bin",
    "wavg",
    "xbar",
    "asof",
    "cross",
};
#define NDYADICS (sizeof(DYADIC_NAMES) / sizeof(DYADIC_NAMES[0]))

static int dyadic_keyword(const char *name) {
    for (int i = 0; i < (int)NDYADICS; i++)
        if (strcmp(name, DYADIC_NAMES[i]) == 0)
            return (int)NVERBS + i;
    return -1;
}

/* An elided position -- f[;2], f[], 2+, (1;;3) -- is filled with K's
 * generic null `::`, the monadic colon (KV1, index 0). There is no separate
 * "missing" type: the generic null *is* the hole, and an explicit `::` in
 * the source scans to the very same value. kelide() tags the parser-inserted
 * one with V_ELIDED, which is display provenance only: print_k renders an
 * elided slot as nothing (f[;2] -> (`f;;2)) and a written :: as ::, so the
 * printed AST preserves the source's spelling. An evaluator ignores the
 * bit -- f[;2] and f[::;2] are the same projection. */
static K kelide(void) { K x = kverb(1, 0); x->u |= V_ELIDED; return x; }

static const char *ADVERB_NAMES[] = { "'", "/", "\\", "':", "/:", "\\:" };

/* ===== lisp-style printer =====
 *
 * Finished ASTs never contain C NULLs (see the invariant at the parser
 * section); a NULL here is a parser bug, printed loudly. */
static void print_k(K x) {
    if (!x) { printf("?[null]"); return; }
    switch (x->t) {
    case -KI:
        printf("%d", x->i);
        break;
    case  KI:
        for (J i = 0; i < x->n; i++) printf("%s%d", i ? " " : "", kI(x)[i]);
        break;
    case -KS:
        printf("`%s", x->s);
        break;
    case  KS:
        if (x->n == 1) putchar(',');
        for (J i = 0; i < x->n; i++) printf("`%s", kS(x)[i]);
        break;
    case  KL:
        putchar('(');
        for (J i = 0; i < x->n; i++) {
            if (i) putchar(';');
            print_k(kK(x)[i]);
        }
        putchar(')');
        break;
    case KV1:
        if (x->u & V_ELIDED) break;   /* elided slot: prints as nothing */
        /* STEP4: print q named-monadic spellings by name, but keep explicit
         * glyph-colon forms such as +: and :: visible as glyph+colon. */
        if ((x->u & V_QNAME) && x->i >= 1 && x->i - 1 < (int)NMONADICS) {
            printf("%s", MONADIC_NAMES[x->i - 1]);
        } else if (x->i >= 0 && x->i < (int)NVERBS) {
            putchar(VERB_CHARS[x->i]);
            putchar(':');
        } else {
            printf("?[t=%d,i=%d]", x->t, x->i);
        }
        break;
    case KV2:
        if (x->i >= 0 && x->i < (int)NVERBS) {
            putchar(VERB_CHARS[x->i]);
        } else if (x->i >= (int)NVERBS && x->i - (int)NVERBS < (int)NDYADICS) {
            printf("%s", DYADIC_NAMES[x->i - (int)NVERBS]);
        } else {
            printf("?[t=%d,i=%d]", x->t, x->i);
        }
        break;
    default:
        printf("?[t=%d]", x->t);
    }
}

/* ===== scanner =====
 *
 * Two-phase parsing: a forward pass produces a flat array of Tokens. The
 * parser consumes that stream — it never looks at raw chars.
 *
 * The scanner builds the K objects directly (ints, syms, names, verbs,
 * adverbs) and stores them on the token. The parser doesn't re-allocate
 * or re-copy; it just lifts the K out (and sets the slot to NULL so
 * cleanup is uniform). Token kinds collapse to T_NOUN/T_VERB/T_ADVERB +
 * punctuators — atom-vs-vector and int-vs-sym are encoded in the K's
 * own type code, so there's no need to track them again at the token
 * level.
 *
 * A single character-class lookup table replaces strchr/isdigit/isalpha
 * in the hot loop. Classes are bit flags so combined tests like
 * "alpha-or-digit" are one mask. */

enum {
    CL_DIGIT  = 1 << 0,
    CL_ALPHA  = 1 << 1,   /* includes '_' */
    CL_VERB   = 1 << 2,
    CL_ADVERB = 1 << 3,
    CL_WS     = 1 << 4,
};

/* CLASS[c] is the set of character classes for byte c. The '\0' slot
 * stays zero, which makes 1-byte lookaheads like src[p+1] safe at end
 * of string: any class test returns false, so we never read past the
 * terminator. init_class() must run before any call to scan(). */
static uint8_t CLASS[256];

static void init_class(void) {
    for (int c = '0'; c <= '9'; c++) CLASS[c] |= CL_DIGIT;
    for (int c = 'a'; c <= 'z'; c++) CLASS[c] |= CL_ALPHA;
    for (int c = 'A'; c <= 'Z'; c++) CLASS[c] |= CL_ALPHA;
    /* '_' lands in BOTH classes: CL_ALPHA here and CL_VERB below (it is in
     * VERB_CHARS). The scanner resolves the ambiguity with one char of
     * lookahead: at token start, '_' begins a name iff a name char follows
     * (_abc), otherwise it is the drop/floor verb (1_2, _ 3). Inside a name
     * it always continues (a_bc). Purely lexical -- context plays no part,
     * so _abc is a name everywhere. */
    CLASS[(int)'_'] |= CL_ALPHA;
    for (const char *p = VERB_CHARS; *p; p++) CLASS[(uint8_t)*p] |= CL_VERB;
    CLASS[(int)'\''] |= CL_ADVERB;
    CLASS[(int)'/']  |= CL_ADVERB;
    CLASS[(int)'\\'] |= CL_ADVERB;
    CLASS[(int)' ']  |= CL_WS;
    CLASS[(int)'\t'] |= CL_WS;
}

typedef enum {
    T_EOF,
    T_NOUN,    /* k is an int/sym atom or vector */
    T_VERB,    /* k is KV1 or KV2 */
    T_ADVERB,  /* k is the adverb's name as a sym atom */
    T_LPAREN, T_RPAREN,
    T_LBRACE, T_RBRACE,
    T_LBRACK, T_RBRACK,
    T_SEMI
} TKind;

typedef struct {
    TKind kind;
    int   start, len;   /* source offset + length (for diagnostics) */
    K     k;            /* owned by the token until the parser lifts it
                         * (parser sets to NULL on consume so free_tokens
                         * doesn't double-release). */
} Token;

typedef struct { Token *t; int n; } Tokens;

/* Read one Int starting at src[*p]: optional '-' then run of digits.
 *
 * The magnitude is accumulated in a J (64-bit), which can't overflow for
 * any digit run we accept: as soon as it exceeds the I range we abort, so
 * the accumulator never climbs past ~INT_MAX*10. This avoids the signed-int
 * overflow (undefined behavior) a naive `I` accumulator would hit on inputs
 * like 99999999999. The bound allows INT_MIN for negative literals. */
static I scan_int(const char *src, int *p) {
    int neg = 0;
    if (src[*p] == '-') { neg = 1; (*p)++; }
    J limit = (J)INT_MAX + (neg ? 1 : 0);   /* |INT_MIN| = INT_MAX + 1 */
    J n = 0;
    while (CLASS[(uint8_t)src[*p]] & CL_DIGIT) {
        n = n * 10 + (src[*p] - '0');
        if (n > limit) die("integer literal out of range");
        (*p)++;
    }
    return (I)(neg ? -n : n);
}

static Tokens scan(const char *src) {
    Token *toks = NULL;
    int n = 0, cap = 0;
    int p = 0;
    /* "noun position" tracks whether the previous emitted token leaves us
     * expecting a noun on the right. After verbs, adverbs, openers, ';',
     * or at start of input, a leading '-' on a digit is a sign; otherwise
     * it's the subtract verb. */
    int noun_pos = 0;

#define EMIT(TK, KK) do { \
        if (n >= cap) { cap = cap ? cap * 2 : 32; toks = xrealloc(toks, (size_t)cap * sizeof(Token)); } \
        toks[n++] = (Token){.kind = (TK), .start = start, .len = p - start, .k = (KK)}; \
    } while (0)

    for (;;) {
        while (CLASS[(uint8_t)src[p]] & CL_WS) p++;
        if (!src[p]) break;

        int start = p;
        char c = src[p];
        uint8_t cl = CLASS[(uint8_t)c];

        int neg_sign = (c == '-' && (CLASS[(uint8_t)src[p+1]] & CL_DIGIT) && !noun_pos);

        /* '_' starts a name iff a name char follows; otherwise it falls
         * through to the CL_VERB branch as drop/floor. One char of
         * lookahead, no context (contrast neg_sign above). */
        int name_start = (cl & CL_ALPHA) &&
            (c != '_' || (CLASS[(uint8_t)src[p+1]] & CL_ALPHA));

        if ((cl & CL_DIGIT) || neg_sign) {
            I buf[MAX_VEC]; int m = 0;
            buf[m++] = scan_int(src, &p);
            for (;;) {
                int sp = p;
                while (CLASS[(uint8_t)src[sp]] & CL_WS) sp++;
                if (sp == p) break;
                int has_dig = CLASS[(uint8_t)src[sp]] & CL_DIGIT;
                int has_neg = src[sp] == '-' && (CLASS[(uint8_t)src[sp+1]] & CL_DIGIT);
                if (!has_dig && !has_neg) break;
                if (m >= MAX_VEC) die("int vector literal too long");
                p = sp;
                buf[m++] = scan_int(src, &p);
            }
            K k;
            if (m == 1) {
                k = ki(buf[0]);
            } else {
                k = ktn(KI, m);
                memcpy(kI(k), buf, (size_t)m * sizeof(I));
            }
            EMIT(T_NOUN, k);
            noun_pos = 1;
        }
        else if (name_start) {
            while (CLASS[(uint8_t)src[p]] & (CL_ALPHA | CL_DIGIT)) p++;
            while (src[p] == '.' && (CLASS[(uint8_t)src[p+1]] & CL_ALPHA)) {
                p++;
                while (CLASS[(uint8_t)src[p]] & (CL_ALPHA | CL_DIGIT)) p++;
            }
            int len = p - start;
            if (len >= MAX_NAME) die("name too long");
            char buf[MAX_NAME];
            memcpy(buf, src + start, (size_t)len);
            buf[len] = '\0';
            /* STEP4: a monadic keyword (flip, neg, count, ...) scans as a
             * KV1 verb, not a -KS noun. A dyadic keyword (lj, bin, ...)
             * scans as a KV2 verb. This is the only scanner change:
             * classify the ~20 monadic and ~6 dyadic names as verbs.
             * Everything else stays a noun. */
            int midx = monadic_keyword(buf);
            if (midx >= 0) {
                EMIT(T_VERB, kverb_qname(midx));
                noun_pos = 0;
            } else {
                int didx = dyadic_keyword(buf);
                if (didx >= 0) {
                    EMIT(T_VERB, kverb(0, didx));
                    noun_pos = 0;
                } else {
                    EMIT(T_NOUN, ks(buf));
                    noun_pos = 1;
                }
            }
        }
        else if (c == '`') {
            /* Build sym vector directly: xstrdup each name once, hand
             * ownership of those pointers to the K's kS storage. */
            S sbuf[MAX_VEC]; int m = 0;
            while (src[p] == '`') {
                if (m >= MAX_VEC) die("sym vector literal too long");
                p++;
                int s = p;
                while ((CLASS[(uint8_t)src[p]] & (CL_ALPHA | CL_DIGIT)) || src[p] == '.') p++;
                int len = p - s;
                S str = xmalloc((size_t)len + 1);
                memcpy(str, src + s, (size_t)len);
                str[len] = '\0';
                sbuf[m++] = str;
            }
            K k = ktn(KS, m);
            memcpy(kS(k), sbuf, (size_t)m * sizeof(S));
            EMIT(T_NOUN, k);
            noun_pos = 1;
        }
        else if (cl & CL_VERB) {
            int idx = verb_index(c);
            p++;
            int monadic = (src[p] == ':');
            if (monadic) p++;
            EMIT(T_VERB, kverb(monadic, idx));
            noun_pos = 0;
        }
        else if (cl & CL_ADVERB) {
            int base = (c == '\'') ? 0 : (c == '/') ? 1 : 2;
            p++;
            int two = (src[p] == ':');
            if (two) p++;
            EMIT(T_ADVERB, ks(ADVERB_NAMES[base + (two ? 3 : 0)]));
            noun_pos = 0;
        }
        else {
            TKind kk;
            switch (c) {
            case '(': kk = T_LPAREN; noun_pos = 0; break;
            case ')': kk = T_RPAREN; noun_pos = 1; break;
            case '{': kk = T_LBRACE; noun_pos = 0; break;
            case '}': kk = T_RBRACE; noun_pos = 1; break;
            case '[': kk = T_LBRACK; noun_pos = 0; break;
            case ']': kk = T_RBRACK; noun_pos = 1; break;
            case ';': kk = T_SEMI;   noun_pos = 0; break;
            default:  die("unexpected character");   /* fail fast, like the parser */
            }
            p++;
            EMIT(kk, NULL);
        }
    }

    if (n >= cap) { cap = cap ? cap * 2 : 32; toks = xrealloc(toks, (size_t)cap * sizeof(Token)); }
    toks[n++] = (Token){.kind = T_EOF, .start = p, .len = 0, .k = NULL};
#undef EMIT
    return (Tokens){toks, n};
}

static void free_tokens(Tokens ts) {
    /* dec_ref any K the parser didn't consume (NULL slots are no-ops). */
    for (int i = 0; i < ts.n; i++) dec_ref(ts.t[i].k);
    free(ts.t);
}

/* ===== parser =====
 *
 *   E : E ; e | e
 *   e : nve | te | empty
 *   t : n | v
 *   v : tA | V
 *   n : t[E] | (E) | {E} | N | q        // STEP2: q is a fourth noun base
 *   q : `select P B from e W            // STEP2 (also exec/update/delete)
 *   B : by L | empty                    // STEP2
 *   W : where L | empty                 // STEP2
 *   P : L | empty                       // STEP2
 *   L : L , e | e                       // STEP2: comma-separated phrases
 *
 *   AST shapes the parser produces:
 *     nve   -> (v; n; e)
 *     te    -> (t; e)              if e non-empty
 *           -> t                    if e empty (lone term collapses)
 *     t[E]  -> (t; e1; e2; ...)    function-application shape
 *     {E}   -> (`{; e1; e2; ...)
 *     tA    -> (`A; t)              role flips to verb
 *     q     -> (`verb; t; c; b; a)  STEP2: t=from c=where b=by a=phrases
 *
 * Each parser function returns a P{role, K v}. role drives the nve/te
 * decision in parse_e; the K v is the assembled AST fragment whose
 * ownership transfers up the call chain (no inc_ref — every K has one
 * implicit owner).
 *
 * Emptiness invariant: parse functions never return a C NULL for valid
 * input -- NULL is reserved for a future error channel. The empty
 * production (e : nve | te | EMPTY) parses to role R_NONE carrying an
 * elided null (kelide()), an ordinary owned node like any other; the
 * caller either attaches it (it is a hole: f[;2], 2+) or releases it
 * (the trailing lookahead miss after a complete e). A zero-element (E)
 * is the genuine empty list. So the three nothings stay distinct:
 *
 *   ()        empty list, a noun            prints ()
 *   (;2;3)    elided slot = implicit null   prints (;2;3)
 *   (::;2;3)  written ::  = identity verb   prints (::;2;3)
 */

typedef enum { R_NONE, R_NOUN, R_VERB } Role;
typedef struct { Role role; K v; } P;

/* The parse of the empty production: no expression here, value is an
 * elided null. Every miss allocates a fresh node -- interning the shared
 * verb values (this one included) is deliberately deferred; the parser
 * isn't allocation-bound (see README on the arena). */
static P empty_e(void) { return (P){R_NONE, kelide()}; }

/* STEP2: the parse context for an expression. A query clause bounds the
 * expression it contains; everywhere else there are no boundaries. The
 * context is passed explicitly down the expression parser rather than held
 * as mutable parser state -- so brackets reset it for free: every ()/[]/{}
 * recurses through parse_E, which always parses at Q_NONE.
 *
 *   Q_NONE    ordinary K (all of Step 1): nothing terminates an expr early
 *   Q_SELECT  select-phrase list: comma-separated; stops at by/from
 *   Q_BY      by-phrase list: comma-separated; stops at from; by is content
 *   Q_FROM    the from-table: one expression; stops at where; from is content
 *   Q_WHERE   where-phrase list: comma-separated; no stoppers; where is content */
typedef enum { Q_NONE, Q_SELECT, Q_BY, Q_FROM, Q_WHERE } QCtx;

typedef struct {
    Tokens t;
    int pos;
} Parser;

static Token *cur(Parser *p) { return &p->t.t[p->pos]; }
static int at(Parser *p, TKind k) { return cur(p)->kind == k; }
static void adv(Parser *p) { p->pos++; }

/* Match discipline: an opener requires its closer. Recursive descent needs
 * no bracket counter -- the call stack already tracks nesting, one parse_E
 * per level -- so each level just asserts the terminator it expects. A
 * missing closer is a hard error (fail-fast, like the rest of the parser);
 * source positions in the message are deferred (see README "What's
 * missing"). */
static void expect(Parser *p, TKind k, const char *msg) {
    if (at(p, k)) adv(p); else die(msg);
}

static K parse_E(Parser *p);
static P parse_e(Parser *p, QCtx ctx);
static P parse_e_from(Parser *p, P t, QCtx ctx);
static P parse_term(Parser *p, QCtx ctx);
static P parse_base(Parser *p);              /* Step-1 core, unchanged        */
static P parse_base_q(Parser *p, QCtx ctx);  /* STEP2: query-aware wrapper     */
static P parse_query(Parser *p);             /* STEP2 */
static K parse_phrase_list(Parser *p, QCtx ctx); /* STEP2: ctx = SELECT/BY/WHERE */
static K parse_table(Parser *p);             /* STEP4: ([]col;...) / ([keys]cols;...) */

/* Unwrap a one-element list: detach the only child, release the husk,
 * return the child. Shared by (E) with one element and seq_of. */
static K unwrap_singleton(K e) {
    K only = kK(e)[0];
    kK(e)[0] = NULL;   /* detach so dec_ref(e) won't recurse into it */
    dec_ref(e);
    return only;
}

/* STEP2: is the current token the name `s? Query keywords scan as -KS
 * name tokens; the parser recognizes them positionally by string.
 * STEP4: a monadic keyword (where, count, ...) scans as a KV1 verb, and a
 * dyadic keyword (lj, bin, ...) as a KV2 verb, so sym_is must also match
 * both. */
static int sym_is(Token *tk, const char *s) {
    if (!tk->k) return 0;
    if (tk->kind == T_NOUN && tk->k->t == -KS)
        return strcmp(tk->k->s, s) == 0;
    if (tk->kind == T_VERB && is_q_named_monadic(tk->k)
        && tk->k->i >= 1 && tk->k->i - 1 < (int)NMONADICS)
        return strcmp(MONADIC_NAMES[tk->k->i - 1], s) == 0;
    if (tk->kind == T_VERB && tk->k->t == KV2
        && tk->k->i >= (int)NVERBS && tk->k->i - (int)NVERBS < (int)NDYADICS)
        return strcmp(DYADIC_NAMES[tk->k->i - (int)NVERBS], s) == 0;
    return 0;
}

/* STEP2: is the current token the dyadic join verb ',' used as a phrase
 * separator? The monadic enlist `,:` (KV1) is an ordinary verb and never a
 * separator, so the token must be the dyadic form (KV2). */
static int is_comma(Token *tk) {
    return tk->kind == T_VERB && tk->k && tk->k->t == KV2 && tk->k->i == verb_index(',');
}

/* STEP2: the four query verbs are reserved at base position (they start a
 * query); the three clause keywords terminate a clause expression but are
 * ordinary names anywhere else. Both are recognized positionally by name. */
static int is_query_verb(Token *tk) {
    return sym_is(tk, "select") || sym_is(tk, "exec") ||
           sym_is(tk, "update") || sym_is(tk, "delete");
}
static int is_clause_kw(Token *tk) {
    return sym_is(tk, "by") || sym_is(tk, "from") || sym_is(tk, "where");
}

/* A `;`-separated run of statements -- the top-level program, and a lambda
 * body -- is a *sequence*: evaluate each, yield the last. That is not the
 * same as a list literal (E), which keeps every element; it is the thing
 * that distinguishes the sequence 1;2;3 from the list (1;2;3). A lone
 * statement collapses to itself (mirroring the (E) one-element rule); two
 * or more are wrapped under the sym `; so an evaluator can dispatch on the
 * head, exactly as it would on `{ for a lambda. Consumes e. */
static K seq_of(K e) {
    if (e->n == 0) { dec_ref(e); return kelide(); }   /* empty body: elided */
    if (e->n == 1) return unwrap_singleton(e);
    K w = ktn(KL, e->n + 1);
    kK(w)[0] = ks(";");
    for (J i = 0; i < e->n; i++) { kK(w)[i+1] = kK(e)[i]; kK(e)[i] = NULL; }
    dec_ref(e);
    return w;
}

static P parse_base(Parser *p) {
    Token *tk = cur(p);
    switch (tk->kind) {
    case T_NOUN: {
        K v = tk->k; tk->k = NULL;
        adv(p);
        return (P){R_NOUN, v};
    }
    case T_VERB: {
        K v = tk->k; tk->k = NULL;
        adv(p);
        return (P){R_VERB, v};
    }
    case T_LPAREN: {
        adv(p);
        /* STEP4: a table literal is ([] ...) or ([keys] ...). The [ appears
         * before any term, so peek without consuming. */
        if (at(p, T_LBRACK)) {
            K tbl = parse_table(p);
            expect(p, T_RPAREN, "expected ')'");
            return (P){R_NOUN, tbl};
        }
        K e = parse_E(p);
        expect(p, T_RPAREN, "expected ')'");
        /* (x) is just x; () is the empty list; (a;b;...) stays a list. */
        if (e->n == 1) return (P){R_NOUN, unwrap_singleton(e)};
        return (P){R_NOUN, e};
    }
    case T_LBRACE: {
        adv(p);
        /* STEP4: optional parameter list [a;b;...] after the opening brace.
         * Each parameter must be a bare name (sym atom). When present, the
         * AST is a 3-element KL (`{; params; body); without params it's the
         * existing 2-element KL (`{; body). */
        K params;
        if (at(p, T_LBRACK)) {
            adv(p);
            params = parse_E(p);   /* empty [] is already the empty list */
            expect(p, T_RBRACK, "expected ']'");
            /* Every param must be a bare name (sym atom). */
            for (J i = 0; i < params->n; i++) {
                K p = kK(params)[i];
                if (!p || p->t != -KS)
                    die("q: lambda parameter must be a name");
            }
        } else {
            params = ktn(KL, 0);
        }
        K body = parse_E(p);
        expect(p, T_RBRACE, "expected '}'");
        /* Lambda marker is the sym `{ — a literal head distinguishable from
         * any verb. The body is a single (sequence) expression: {x+y} is
         * (`{;(+;`x;`y)), {a;b} is (`{;(`;;`a;`b)). With parameters
         * {[a;b] x+y} is (`{;(`a;`b);(+;`x;`y)). A runtime would
         * substitute the actual closure value later. */
        int has_params = params->n > 0;
        int arity = has_params ? 3 : 2;
        K w = ktn(KL, arity);
        kK(w)[0] = ks("{");
        if (has_params) kK(w)[1] = params; else dec_ref(params);
        kK(w)[has_params ? 2 : 1] = seq_of(body);
        return (P){R_NOUN, w};
    }
    default:
        return empty_e();
    }
}

/* STEP2: query-aware base. parse_base itself is exactly the Step-1 core; all
 * query knowledge lives here. A query verb at base position starts a query
 * (the fourth noun base, n : ... | q). Clause keywords stop the current
 * expression only when they are legal stoppers for this context:
 *
 *   Q_SELECT   stops at by / from          (where -> error)
 *   Q_BY       stops at from;              by=content, where=error
 *   Q_FROM     stops at where;             from=content, by=error
 *   Q_WHERE    no stoppers;                where=content, by/from=error
 *
 * Phrase contexts (SELECT/BY/WHERE) also stop at top-level ','.
 * Brackets need no special handling: parse_base recurses through parse_E,
 * which always parses at Q_NONE, so inside ()/[]/{} commas join and
 * keywords are plain names. */
static P parse_base_q(Parser *p, QCtx ctx) {
    Token *tk = cur(p);
    if (is_query_verb(tk)) return parse_query(p);
    if (ctx != Q_NONE && is_clause_kw(tk)) {
        int is_by = sym_is(tk, "by"), is_from = sym_is(tk, "from"),
            is_where = sym_is(tk, "where");
        switch (ctx) {
        case Q_SELECT:
            if (is_by || is_from) return empty_e();
            die("ksql: unexpected keyword after select (expected by or from)");
        case Q_BY:
            if (is_from) return empty_e();
            if (is_by) break;  /* own keyword: content, not stopper */
            die("ksql: unexpected keyword in by phrase (expected from)");
        case Q_FROM:
            if (is_where) return empty_e();
            if (is_from) break;  /* own keyword: content, not stopper */
            die("ksql: unexpected keyword after from (expected where)");
        case Q_WHERE:
            if (is_where) break;  /* own keyword: content, not stopper */
            die("ksql: unexpected keyword in where phrase");
        default: break;
        }
    }
    if (ctx != Q_NONE && ctx != Q_FROM && is_comma(tk)) return empty_e();
    return parse_base(p);
}

static P parse_term(Parser *p, QCtx ctx) {
    P t = parse_base_q(p, ctx);
    if (t.role == R_NONE) return t;

    for (;;) {
        Token *tk = cur(p);
        if (tk->kind == T_LBRACK) {
            adv(p);
            K e = parse_E(p);
            expect(p, T_RBRACK, "expected ']'");
            /* Elided slots already arrive as elided nulls from parse_e; the
             * one extra rule is f[] = f[::], a single elided argument. */
            J argc = e->n ? e->n : 1;
            K w = ktn(KL, argc + 1);
            kK(w)[0] = t.v;
            if (e->n == 0) kK(w)[1] = kelide();
            for (J i = 0; i < e->n; i++) { kK(w)[i+1] = kK(e)[i]; kK(e)[i] = NULL; }
            dec_ref(e);
            t.v = w; t.role = R_NOUN;
        } else if (tk->kind == T_ADVERB) {
            K w = ktn(KL, 2);
            kK(w)[0] = tk->k; tk->k = NULL;
            kK(w)[1] = t.v;
            adv(p);
            t.v = w; t.role = R_VERB;
        } else {
            break;
        }
    }
    return t;
}

/* parse_e parses one term, then hands off to parse_e_from. The split lets
 * us choose nve-vs-te by the *role* of the next term instead of a one-token
 * peek. A term's role isn't fixed by its first token: {x+y} is a noun but
 * {x+y}' is a verb, and f is a noun but f' is a verb. Deciding on the
 * leading token (the old `at(p, T_VERB)` test) only worked for primitive
 * verbs and mis-structured noun-derived verbs (tA) in infix position.
 *
 * This stays predictive and linear, not backtracking: parse_term consumes
 * nothing when there is no term (parse_base returns the empty production
 * without calling adv), so speculatively parsing the next term is cheap
 * one-term lookahead. No token is ever un-read, and each term is parsed
 * exactly once. */
static P parse_e(Parser *p, QCtx ctx) {
    P t = parse_term(p, ctx);
    if (t.role == R_NONE) return t;   /* empty e: the elided null flows up */
    return parse_e_from(p, t, ctx);
}

/* Continue an e whose leading term t has already been parsed. The parse
 * context flows through unchanged: a top-level ',' or clause keyword ends
 * the expression at whatever recursion depth it appears (so the right
 * operand of a verb also halts at the phrase separator). */
static P parse_e_from(Parser *p, P t, QCtx ctx) {
    P u = parse_term(p, ctx);

    /* No following term: lone term (te with empty rhs). A bare term stands
     * for itself; a lone dyadic verb keeps its dyadic form (no demotion).
     * The lookahead miss carries an unused elided null; release it. */
    if (u.role == R_NONE) { dec_ref(u.v); return t; }

    /* nve: noun, then DYADIC verb-term, then the rest. u is whatever
     * parse_term produced for the second term -- a primitive (KV2), a
     * named dyad (KV2), or an adverb-derived verb (KL) -- so this catches
     * f', {x+y}', (E)' just like +, %, &.
     *
     * STEP4: a named monadic has no dyadic form, so it is NOT an infix
     * here. `f til 10` is not `til[f;10]`; it is `f` applied to `(til 10)`
     * -- a te with f as head. Explicit glyph-colon forms (`+:`, `::`, ...)
     * are still ordinary verb terms in this syntactic slot, even though they
     * are also KV1; only q named-monadic provenance excludes the nve branch.
     * Adverb-derived verbs are KL (variadic), not KV1, so `1 +/ 2 3` still
     * infixes. */
    if (t.role == R_NOUN && u.role == R_VERB && !is_q_named_monadic(u.v)) {
        /* An empty right operand is an elided argument: 2+ is the projection
         * +[2;], i.e. (+;2;) -- and parse_e already returns that hole. */
        P e = parse_e(p, ctx);
        K w = ktn(KL, 3);
        kK(w)[0] = u.v;
        kK(w)[1] = t.v;
        kK(w)[2] = e.v;
        return (P){R_NOUN, w};
    }

    /* te: t applied to an e whose leading term is the u we just parsed. */
    P e = parse_e_from(p, u, ctx);
    /* STEP4: the demotion block that was here is removed. In K, a dyadic
     * glyph (KV2) in monadic position was demoted to KV1 by inferring
     * monadic arity from position. In q, glyphs are always dyadic and
     * monadic verbs arrive as KV1 from the scanner -- so a KV2 glyph in
     * monadic position is an error, not something to infer. The user must
     * write the named monadic (flip 1, not +1). This is the entire
     * parser-side diff: q's naming convention removes parser logic (the
     * demotion) and replaces it with a hard error. */
    if (t.role == R_VERB && t.v && t.v->t == KV2) {
        die("q: dyadic verb in monadic position; use the named monadic");
    }
    K w = ktn(KL, 2);
    kK(w)[0] = t.v;
    kK(w)[1] = e.v;
    return (P){R_NOUN, w};
}

/* Elided positions -- (;2;3), (1;;3), 1;;2 -- need no special handling
 * here: an empty e already arrives as the elided null. The one distinction
 * parse_E draws is zero expressions vs one empty expression: (E) with no
 * expression at all (and hence no ';') is the genuine empty list (). */
static K parse_E(Parser *p) {
    K buf[MAX_VEC]; int n = 0;
    P first = parse_e(p, Q_NONE);
    if (first.role == R_NONE && !at(p, T_SEMI)) {
        dec_ref(first.v);
        return ktn(KL, 0);                    /* no expressions at all: () */
    }
    buf[n++] = first.v;
    while (at(p, T_SEMI)) {
        if (n >= MAX_VEC) die("too many ';'-separated expressions");
        adv(p);
        buf[n++] = parse_e(p, Q_NONE).v;
    }
    return klist(buf, n);
}

/* ===== STEP 4: table literals =====
 *
 * A table literal desugars to existing K verbs, the same way ksql queries
 * desugar to t[E] application. In q, `([]a:til 10;b:til 10)` is flip of
 * a dict (!) built from the column names and values; `([k:v]a:1)` is a
 * keyed table formed by the dyadic ! of two flipped dicts. No new AST
 * shape, no new type code -- just assembly of KV1 (flip) and KV2 (!) nodes
 * that the grammar already has.
 *
 *   ([]  cols)   ->  (flip; (!; names; values))
 *   ([keys] cols) ->  (!; (flip; (!; knames; kvals)); (flip; (!; cnames; cvals)))
 *
 * Each column `name:expr` is the existing `:` assignment shape, so parse_E
 * on `a:1;b:2` produces ((:;`a;1);(:;`b;2)). The function splits those
 * into a sym vector of names and a list of value expressions for the
 * dict constructor (!). */

/* Collect columns from a parse_E result into name and value lists for !.
 * Each element of `e` must be an assignment (:;`name;expr).
 * Steals ownership of e's children; caller should dec_ref the husk. */
static void unzip_cols(K e, K *names_out, K *vals_out) {
    J n = e->n;
    K names = ktn(KS, n), vals = ktn(KL, n);
    for (J i = 0; i < n; i++) {
        K col = kK(e)[i];
        if (!col || col->t != KL || col->n != 3) die("q: malformed column definition");
        K name = kK(col)[1];   /* the name sym — index 1 in nve (:;`name;expr) */
        K expr = kK(col)[2];   /* the value expression */
        if (!name || name->t != -KS) die("q: expected name in column definition");
        kS(names)[i] = xstrdup(name->s);
        kK(col)[2] = NULL;     /* detach expr so dec_ref(col) won't free it */
        kK(vals)[i] = expr;
        kK(e)[i] = NULL;       /* detach col from e, then release its husk */
        dec_ref(col);
    }
    *names_out = names;
    *vals_out  = vals;
}

static K mkdict(K names, K vals) {
    K d = ktn(KL, 3);
    kK(d)[0] = kverb(0, verb_index('!'));   /* dyadic ! */
    kK(d)[1] = names;
    kK(d)[2] = vals;
    return d;
}

static K mkflip(K dict) {
    K f = ktn(KL, 2);
    kK(f)[0] = kverb_qname(verb_index('+'));   /* monadic + (flip) */
    kK(f)[1] = dict;
    return f;
}

static K parse_table(Parser *p) {
    /* Consume the '[' that parse_base peeked at. */
    adv(p);

    /* Key columns inside [...] — the empty list if ([] ...) i.e. no key. */
    K keys = parse_E(p);
    expect(p, T_RBRACK, "expected ']'");

    /* Value columns: name:expr ; ... */
    K vals = parse_E(p);

    K kdict = NULL, vdict = NULL;

    if (keys->n == 0) {
        /* ([] cols): unkeyed. Build flip of dict(names, values). */
        dec_ref(keys);
        K ns, vs;
        unzip_cols(vals, &ns, &vs);
        dec_ref(vals);
        K d = mkdict(ns, vs);
        return mkflip(d);
    }

    /* ([keys] cols): keyed. Build ! of (flip key-dict) and (flip val-dict). */
    K kns, kvs;
    unzip_cols(keys, &kns, &kvs);
    dec_ref(keys);
    kdict = mkdict(kns, kvs);

    K cns, cvs;
    unzip_cols(vals, &cns, &cvs);
    dec_ref(vals);
    vdict = mkdict(cns, cvs);

    K tbl = ktn(KL, 3);
    kK(tbl)[0] = kverb(0, verb_index('!'));   /* dyadic ! for keyed table */
    kK(tbl)[1] = mkflip(kdict);
    kK(tbl)[2] = mkflip(vdict);
    return tbl;
}

/* ===== STEP 2: ksql =====
 *
 * A query desugars to the ordinary t[E] application (`verb; t; c; b; a):
 *
 *   select s by b from f where w   ->   (`select; f; (w); (b); (s))
 *
 * with t=from-expr, c=where-list, b=by-list, a=select/phrase-list -- the
 * argument order of nsl ksql.k's select:{[t;c;b;a]...}. Absent clauses are
 * the empty list (). Each phrase is an ordinary e, so an aliased phrase
 * `qty:expr` is just the (:;`qty;expr) assignment shape -- no new code.
 *
 * parse_phrase_list reads a comma-separated run of phrases in the given
 * context (Q_SELECT / Q_BY / Q_WHERE), so parse_e stops at each top-level
 * ',' and at the legal stoppers for that context. */
static K parse_phrase_list(Parser *p, QCtx ctx) {
    K buf[MAX_VEC]; int n = 0;
    P first = parse_e(p, ctx);
    if (first.role == R_NONE && !is_comma(cur(p))) {
        dec_ref(first.v);
        return ktn(KL, 0);                         /* no phrases -> () */
    }
    buf[n++] = first.v;
    while (is_comma(cur(p))) {
        adv(p);                                    /* step over separator */
        if (n >= MAX_VEC) die("ksql: too many phrases");
        buf[n++] = parse_e(p, ctx).v;              /* empty phrase -> elided */
    }
    return klist(buf, n);
}

/* Parse one query, the current token being its verb keyword. Surface order
 * is verb, phrases, [by ...], from, [where ...]; we collect those and emit
 * them reordered to (verb; t; c; b; a). The keyword/separator tokens are
 * stepped over with adv(); their K payloads stay owned by the token array
 * and are released by free_tokens().
 *
 * Structural well-formedness only: a clause keyword that is written must
 * carry its operand -- `from` needs a table, and `by`/`where` need a
 * non-empty list. (The select/phrase list itself may be empty: that is a
 * valid "all columns".) Anything past that -- does the table exist, is the
 * column real -- is the evaluator's job, not the parser's. */
static P parse_query(Parser *p) {
    K head = ks(cur(p)->k->s);   /* `select / `exec / `update / `delete */
    adv(p);

    K a = parse_phrase_list(p, Q_SELECT);        /* select / phrase list */

    K b;
    if (sym_is(cur(p), "by")) {
        adv(p);
        b = parse_phrase_list(p, Q_BY);
        if (b->n == 0) die("ksql: empty 'by'");
    } else b = ktn(KL, 0);

    if (!sym_is(cur(p), "from")) die("ksql: expected 'from'");
    adv(p);
    P tp = parse_e(p, Q_FROM);                   /* table is one expr: ',' joins, stops at where */
    if (tp.role == R_NONE) die("ksql: expected table after 'from'");
    K t = tp.v;

    K c;
    if (sym_is(cur(p), "where")) {
        adv(p);
        c = parse_phrase_list(p, Q_WHERE);
        if (c->n == 0) die("ksql: empty 'where'");
    } else c = ktn(KL, 0);

    /* A query is a complete noun: nothing may trail the clauses but an
     * expression terminator. A leftover clause keyword here means the
     * clauses were given out of order (e.g. `by` after `from`); without
     * this guard it would silently reattach to the query as juxtaposition. */
    if (!(at(p, T_EOF) || at(p, T_SEMI) ||
          at(p, T_RPAREN) || at(p, T_RBRACK) || at(p, T_RBRACE)))
        die("ksql: unexpected token after query");

    K w = ktn(KL, 5);
    kK(w)[0] = head;
    kK(w)[1] = t;
    kK(w)[2] = c;
    kK(w)[3] = b;
    kK(w)[4] = a;
    return (P){R_NOUN, w};
}

static void run(const char *src) {
    Tokens ts = scan(src);
    Parser p = {.t = ts, .pos = 0};
    K e = parse_E(&p);
    /* The outermost level is terminated by end-of-input, the same way a
     * bracketed level is terminated by its closer. A leftover token here is
     * a stray closer or trailing junk -- the parser stopped before the end
     * but the input didn't, so it is malformed. */
    if (!at(&p, T_EOF)) die("unexpected token");
    K prog = seq_of(e);   /* the program is a statement sequence, not a list */
    print_k(prog);
    putchar('\n');
    dec_ref(prog);        /* recursively releases the whole AST */
    free_tokens(ts);
}

int main(void) {
    init_class();
    /* STEP4: the interactive prompt shows q> so you know which binary you're
     * in. When stdin is piped (the golden test runner), keep the plain
     * two-space prompt the runner strips, matching the other binaries. */
    int interactive = isatty(STDIN_FILENO);
    char line[1024];
    for (;;) {
        fputs(interactive ? "q> " : "  ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) { putchar('\n'); break; }
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;
        run(line);
    }
    return 0;
}
