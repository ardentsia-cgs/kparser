/* strdup is POSIX, not ISO C; request it explicitly so a strict
 * -std=c99/-std=c11 build on glibc still declares it. Must precede any
 * system header include. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

/* Pedagogical, minimal parser for the k grammar at
 *   https://k.miraheze.org/wiki/Grammar
 * Builds a K-typed AST (lisp-ish), printed in a parse-tree style.
 * Supports: int atoms/vectors, sym atoms/vectors, names, primitive verbs
 * (101/102), adverbs (as sym wrappers), parens, lambdas, indexing. No
 * eval, no float/string, no error recovery. */

/* Safety constants. Real K would grow these dynamically; we bound them
 * statically and abort on overflow to keep the pedagogical version short. */
#define MAX_VEC  4096   /* max ints in an int literal, syms in a sym literal,
                         * or exprs separated by ';' inside one (E) clause */
#define MAX_NAME  256   /* max bytes in a single name token */

static void die(const char *msg) {
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

/* m, a, u are header bytes kept only for layout parity with other K
 * implementations; we don't use them. r is the refcount used by
 * dec_ref/inc_ref. The union is the classic K idiom: for atoms we use
 * i/s/k directly; for vectors n is the element count and G0[] is a
 * flexible-array tail accessed via the kI/kS/kK casts. */
typedef struct k0 {
    signed char m, a, t;       /* m, a unused here; t = type code */
    G u;                       /* attribute flags, unused */
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

static K kverb(int monadic, int idx) {
    K x = ka(monadic ? KV1 : KV2);
    x->i = idx;
    return x;
}

/* Generic null `::` -- K's identity value, and the marker for an elided
 * argument in a projection (f[;2] -> (`f;::;2), 2+ -> (+;2;::)). It is the
 * monadic colon (KV1, index 0), which print_k renders as "::". K has no
 * separate "missing" type; the generic null *is* the hole. */
static K knull(void) { return kverb(1, 0); }

static const char *ADVERB_NAMES[] = { "'", "/", "\\", "':", "/:", "\\:" };

/* ===== lisp-style printer ===== */
static void print_k(K x) {
    if (!x) { printf("()"); return; }
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
    case KV1: case KV2:
        if (x->i >= 0 && x->i < (int)NVERBS) {
            putchar(VERB_CHARS[x->i]);
            if (x->t == KV1) putchar(':');
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
        else if (cl & CL_ALPHA) {
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
            EMIT(T_NOUN, ks(buf));
            noun_pos = 1;
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
            default:  p++; continue;   /* skip unknown */
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
 *   n : t[E] | (E) | {E} | N
 *
 *   AST shapes the parser produces:
 *     nve   -> (v; n; e)
 *     te    -> (t; e)              if e non-empty
 *           -> t                    if e empty (lone term collapses)
 *     t[E]  -> (t; e1; e2; ...)    function-application shape
 *     {E}   -> (`{; e1; e2; ...)
 *     tA    -> (`A; t)              role flips to verb
 *
 * Each parser function returns a P{role, K v}. role drives the nve/te
 * decision in parse_e; the K v is the assembled AST fragment whose
 * ownership transfers up the call chain (no inc_ref — every K has one
 * implicit owner).
 */

typedef enum { R_NONE, R_NOUN, R_VERB } Role;
typedef struct { Role role; K v; } P;
static const P EMPTY = { R_NONE, NULL };

typedef struct {
    const char *src;
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
static P parse_e(Parser *p);
static P parse_e_from(Parser *p, P t);
static P parse_term(Parser *p);
static P parse_base(Parser *p);

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
        K e = parse_E(p);
        expect(p, T_RPAREN, "expected ')'");
        if (e->n == 1) {
            K only = kK(e)[0];
            kK(e)[0] = NULL;  /* detach so dec_ref(e) won't recurse into it */
            dec_ref(e);
            return (P){R_NOUN, only};
        }
        return (P){R_NOUN, e};
    }
    case T_LBRACE: {
        adv(p);
        K e = parse_E(p);
        expect(p, T_RBRACE, "expected '}'");
        /* Lambda marker is the sym `{ — chosen so it appears in the AST
         * as a literal head distinguishable from any verb. The parse tree
         * for {x+y} is (`{; (+;`x;`y)); a runtime would substitute the
         * actual closure value later. */
        K w = ktn(KL, e->n + 1);
        kK(w)[0] = ks("{");
        for (J i = 0; i < e->n; i++) { kK(w)[i+1] = kK(e)[i]; kK(e)[i] = NULL; }
        dec_ref(e);
        return (P){R_NOUN, w};
    }
    default:
        return EMPTY;
    }
}

static P parse_term(Parser *p) {
    P t = parse_base(p);
    if (t.role == R_NONE) return t;

    for (;;) {
        Token *tk = cur(p);
        if (tk->kind == T_LBRACK) {
            adv(p);
            K e = parse_E(p);
            expect(p, T_RBRACK, "expected ']'");
            K w = ktn(KL, e->n + 1);
            kK(w)[0] = t.v;
            /* An elided argument slot (NULL) is the generic null `::`: the
             * projection f[;2] is (`f;::;2), and f[] is (`f;::). */
            for (J i = 0; i < e->n; i++) {
                kK(w)[i+1] = kK(e)[i] ? kK(e)[i] : knull();
                kK(e)[i] = NULL;
            }
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
 * nothing when there is no term (parse_base returns EMPTY without calling
 * adv), so speculatively parsing the next term is free one-term lookahead.
 * No token is ever un-read, and each term is parsed exactly once. */
static P parse_e(Parser *p) {
    P t = parse_term(p);
    if (t.role == R_NONE) return EMPTY;
    return parse_e_from(p, t);
}

/* Continue an e whose leading term t has already been parsed. */
static P parse_e_from(Parser *p, P t) {
    P u = parse_term(p);

    /* No following term: lone term (te with empty rhs). A bare term stands
     * for itself; a lone dyadic verb keeps its dyadic form (no demotion). */
    if (u.role == R_NONE) return t;

    /* nve: noun, then verb-term, then the rest. u is whatever parse_term
     * produced for the second term -- a primitive (KV2) or an adverb-derived
     * verb (KL) -- so this now catches f', {x+y}', (E)' just like +, %, &. */
    if (t.role == R_NOUN && u.role == R_VERB) {
        P e = parse_e(p);
        K w = ktn(KL, 3);
        kK(w)[0] = u.v;
        kK(w)[1] = t.v;
        /* Empty right operand is an elided argument: 2+ is the projection
         * +[2;], i.e. (+;2;::), so mark the hole with the generic null. */
        kK(w)[2] = e.v ? e.v : knull();
        return (P){R_NOUN, w};
    }

    /* te: t applied to an e whose leading term is the u we just parsed. */
    P e = parse_e_from(p, u);
    /* Verb head in monadic position: build a fresh KV1 from the dyadic
     * primitive's index, then release the old. Adverb-modified verbs are
     * KL, so this check skips them. */
    if (t.role == R_VERB && t.v && t.v->t == KV2) {
        K old = t.v;
        t.v = kverb(1, old->i);
        dec_ref(old);
    }
    K w = ktn(KL, 2);
    kK(w)[0] = t.v;
    kK(w)[1] = e.v;
    return (P){R_NOUN, w};
}

static K parse_E(Parser *p) {
    K buf[MAX_VEC]; int n = 0;
    buf[n++] = parse_e(p).v;
    while (at(p, T_SEMI)) {
        if (n >= MAX_VEC) die("too many ';'-separated expressions");
        adv(p);
        buf[n++] = parse_e(p).v;
    }
    return klist(buf, n);
}

static void run(const char *src) {
    Tokens ts = scan(src);
    Parser p = {.src = src, .t = ts, .pos = 0};
    K e = parse_E(&p);
    /* The outermost level is terminated by end-of-input, the same way a
     * bracketed level is terminated by its closer. A leftover token here is
     * a stray closer or trailing junk -- the parser stopped before the end
     * but the input didn't, so it is malformed. */
    if (!at(&p, T_EOF)) die("unexpected token");
    if (e->n == 1) print_k(kK(e)[0]); else print_k(e);
    putchar('\n');
    dec_ref(e);          /* recursively releases the whole AST */
    free_tokens(ts);
}

int main(void) {
    init_class();
    char line[1024];
    for (;;) {
        fputs("  ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) { putchar('\n'); break; }
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;
        run(line);
    }
    return 0;
}
