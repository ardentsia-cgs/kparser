/* strdup is POSIX, not ISO C; request it explicitly so a strict
 * -std=c99/-std=c11 build on glibc still declares it. Must precede any
 * system header include. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>   /* STEP3: isatty -- show the mode in the prompt only when
                      * interactive, so the piped golden tests see the plain
                      * two-space prompt they strip. */

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
 * STEP 3 (sql): this file is also a STANDARD SQL front-end, selected by a
 * runtime MODE (--sql / --ksql flag, or \sql / \ksql in the REPL; default
 * ksql). The lesson is that SQL and ksql are two SURFACES over ONE AST: a
 * SQL statement desugars into the very same (`verb; t; c; b; a) tree, so
 *   SELECT * FROM t        ==  select from t        (`*` is `all columns`,
 *                                                    i.e. an empty phrase
 *                                                    list a)
 *   SELECT a FROM t WHERE d>0 AND e<5  ==  select a from t where d>0,e<5
 * The relational core (SELECT/FROM/WHERE/GROUP BY) maps slot-for-slot;
 * SQL's POST-relational clauses (DISTINCT/ORDER BY/LIMIT) are not new slots
 * -- they wrap the query noun as ordinary verbs, exactly SQL's own logical
 * processing order:  SELECT a FROM t ORDER BY x LIMIT 10  desugars to the
 * ordinary K expression  10 # `x asc select a from t  ==
 *   (#;10;(`asc;`x;(`select;`t;();();(`a)))).  (asc/dsc are kparc's dyadic
 * sort verbs; # is take, _ is drop, ?: is unique -- all ordinary K.)
 * SQL needs two things K/ksql do not: operator PRECEDENCE (K is
 * precedence-free and right-to-left, so SQL gets its own precedence-climbing
 * expression parser) and a wider LEXER (case-insensitive keywords, string
 * literals, two-char operators, function-call commas). Every STEP 3 addition
 * is tagged STEP3 or lives in the `===== STEP 3: sql =====` block. See
 * SQL.md. */

/* Safety constants. Real K would grow these dynamically; we bound them
 * statically and abort on overflow to keep the pedagogical version short. */
#define MAX_VEC  4096   /* max ints in an int literal, syms in a sym literal,
                         * or exprs separated by ';' inside one (E) clause */
#define MAX_NAME  256   /* max bytes in a single name token */

/* STEP3: which surface syntax the REPL is parsing. The two share one AST and
 * one printer; only the scanner and the top-level parser branch on it. */
typedef enum { M_KSQL, M_SQL } Mode;

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
    T_SEMI,
    T_COMMA    /* STEP3: SQL phrase/argument separator (the K scanner never
                * emits this -- there ',' is the join verb). */
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
 */

typedef enum { R_NONE, R_NOUN, R_VERB } Role;
typedef struct { Role role; K v; } P;
static const P EMPTY = { R_NONE, NULL };

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
static P parse_e(Parser *p, QCtx ctx);
static P parse_e_from(Parser *p, P t, QCtx ctx);
static P parse_term(Parser *p, QCtx ctx);
static P parse_base(Parser *p);              /* Step-1 core, unchanged        */
static P parse_base_q(Parser *p, QCtx ctx);  /* STEP2: query-aware wrapper     */
static P parse_query(Parser *p);             /* STEP2 */
static K parse_phrase_list(Parser *p, QCtx ctx); /* STEP2: ctx = SELECT/BY/WHERE */

/* STEP2: is the current token the name `s? Query keywords scan as ordinary
 * -KS name tokens; the parser recognizes them positionally by string. */
static int sym_is(Token *tk, const char *s) {
    return tk->kind == T_NOUN && tk->k && tk->k->t == -KS && strcmp(tk->k->s, s) == 0;
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
    if (e->n == 1) {
        K only = kK(e)[0];
        kK(e)[0] = NULL;   /* detach so dec_ref(e) won't recurse into it */
        dec_ref(e);
        return only;
    }
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
        /* Lambda marker is the sym `{ — a literal head distinguishable from
         * any verb. The body is a single (sequence) expression: {x+y} is
         * (`{;(+;`x;`y)), {a;b} is (`{;(`;;`a;`b)). A runtime would
         * substitute the actual closure value later. */
        K w = ktn(KL, 2);
        kK(w)[0] = ks("{");
        kK(w)[1] = seq_of(e);
        return (P){R_NOUN, w};
    }
    default:
        return EMPTY;
    }
}

/* STEP2: query-aware base. parse_base itself is exactly the Step-1 core; all
 * query knowledge lives here. A query verb at base position starts a query
 * (the fourth noun base, n : ... | q). Clause keywords stop the current
 * expression only when they are legal stoppers for this context:
 *
 *   Q_SELECT   stops at by / from          (by=content -> error)
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
            if (is_by || is_from) return EMPTY;
            die("ksql: unexpected keyword after select (expected by or from)");
        case Q_BY:
            if (is_from) return EMPTY;
            if (is_by) break;  /* own keyword: content, not stopper */
            die("ksql: unexpected keyword in by phrase (expected from)");
        case Q_FROM:
            if (is_where) return EMPTY;
            if (is_from) break;  /* own keyword: content, not stopper */
            die("ksql: unexpected keyword after from (expected where)");
        case Q_WHERE:
            if (is_where) break;  /* own keyword: content, not stopper */
            die("ksql: unexpected keyword in where phrase");
        default: break;
        }
    }
    if (ctx != Q_NONE && ctx != Q_FROM && is_comma(tk)) return EMPTY;
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
static P parse_e(Parser *p, QCtx ctx) {
    P t = parse_term(p, ctx);
    if (t.role == R_NONE) return EMPTY;
    return parse_e_from(p, t, ctx);
}

/* Continue an e whose leading term t has already been parsed. The parse
 * context flows through unchanged: a top-level ',' or clause keyword ends
 * the expression at whatever recursion depth it appears (so the right
 * operand of a verb also halts at the phrase separator). */
static P parse_e_from(Parser *p, P t, QCtx ctx) {
    P u = parse_term(p, ctx);

    /* No following term: lone term (te with empty rhs). A bare term stands
     * for itself; a lone dyadic verb keeps its dyadic form (no demotion). */
    if (u.role == R_NONE) return t;

    /* nve: noun, then verb-term, then the rest. u is whatever parse_term
     * produced for the second term -- a primitive (KV2) or an adverb-derived
     * verb (KL) -- so this now catches f', {x+y}', (E)' just like +, %, &. */
    if (t.role == R_NOUN && u.role == R_VERB) {
        P e = parse_e(p, ctx);
        K w = ktn(KL, 3);
        kK(w)[0] = u.v;
        kK(w)[1] = t.v;
        /* Empty right operand is an elided argument: 2+ is the projection
         * +[2;], i.e. (+;2;::), so mark the hole with the generic null. */
        kK(w)[2] = e.v ? e.v : knull();
        return (P){R_NOUN, w};
    }

    /* te: t applied to an e whose leading term is the u we just parsed. */
    P e = parse_e_from(p, u, ctx);
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
    buf[n++] = parse_e(p, Q_NONE).v;
    while (at(p, T_SEMI)) {
        if (n >= MAX_VEC) die("too many ';'-separated expressions");
        adv(p);
        buf[n++] = parse_e(p, Q_NONE).v;
    }
    return klist(buf, n);
}

/* STEP2/STEP3: assemble the canonical query node (`verb; t; c; b; a) from its
 * four argument slots. This is the single point where the AST becomes
 * target-independent: ksql's parse_query and SQL's parse_sql_query both end
 * here, which is what makes `select from t` and `SELECT * FROM t` literally
 * the same tree. Consumes head/t/c/b/a. */
static K emit_query(K head, K t, K c, K b, K a) {
    K w = ktn(KL, 5);
    kK(w)[0] = head;
    kK(w)[1] = t;
    kK(w)[2] = c;
    kK(w)[3] = b;
    kK(w)[4] = a;
    return w;
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
    if (first.role == R_NONE) return ktn(KL, 0);   /* no phrases -> () */
    buf[n++] = first.v;
    while (is_comma(cur(p))) {
        adv(p);                                    /* step over separator */
        if (n >= MAX_VEC) die("ksql: too many phrases");
        P f = parse_e(p, ctx);
        buf[n++] = f.v ? f.v : knull();            /* empty phrase -> :: */
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
    if (!tp.v) die("ksql: expected table after 'from'");
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

    return (P){R_NOUN, emit_query(head, t, c, b, a)};
}

/* ===== STEP 3: sql =====
 *
 * A standard-SQL front-end that targets the SAME (`verb; t; c; b; a) AST as
 * ksql. Surface order differs and is reordered into the slots:
 *
 *   SELECT a,b FROM t WHERE d>0 GROUP BY c
 *        ->  (`select; t; ((>;`d;0)); (`c); (`a;`b))
 *
 * Grammar (the relational core; post-relational clauses wrap the result):
 *
 *   stmt   : select | update | delete
 *   select : SELECT [DISTINCT] (`*` | items) FROM e [WHERE bool]
 *                   [GROUP BY list] [ORDER BY list [ASC|DESC]]
 *                   [LIMIT num [OFFSET num]]
 *   update : UPDATE e SET asgns [WHERE bool]
 *   delete : DELETE FROM e [WHERE bool]
 *   item   : expr [[AS] name]            // alias = (:;`name;expr)
 *   bool   : expr                        // full precedence; top-level AND
 *                                         //   chain flattens into the c-list
 *   expr   : precedence-climbing over OR/AND/NOT, comparisons, +-* /,
 *            function calls f(a,...), and (parenthesised) subexpressions
 *
 * Two things SQL needs that K does not: operator PRECEDENCE (parse_sql_expr,
 * below, because K is precedence-free and right-to-left) and a wider LEXER
 * (scan_sql: case-insensitive keywords, 'strings', two-char operators, and
 * `,` as a separator rather than the join verb). Everything else -- the K
 * types, the application shape for calls, the `:` alias shape, the (verb;
 * t;c;b;a) node, and the printer -- is reused unchanged. */

/* STEP3: SQL scanner. Numbers and identifiers reuse the Step-1 helpers and
 * character classes; keywords stay ordinary -KS name tokens (matched
 * case-insensitively in the parser, exactly like ksql's positional `from`/
 * `where`). Operators with a K glyph become that verb so the output matches
 * ksql (WHERE d>0 -> (>;`d;0)); the two-char comparisons K has no glyph for
 * (`<=`/`>=`/`<>`) become sym heads; SQL `/` (divide) maps to K `%`. */
static Tokens scan_sql(const char *src) {
    Token *toks = NULL;
    int n = 0, cap = 0;
    int p = 0;

#define EMITS(TK, KK) do { \
        if (n >= cap) { cap = cap ? cap * 2 : 32; toks = xrealloc(toks, (size_t)cap * sizeof(Token)); } \
        toks[n++] = (Token){.kind = (TK), .start = start, .len = p - start, .k = (KK)}; \
    } while (0)

    for (;;) {
        while (CLASS[(uint8_t)src[p]] & CL_WS) p++;
        if (!src[p]) break;

        int start = p;
        char c = src[p];
        uint8_t cl = CLASS[(uint8_t)c];

        if (cl & CL_DIGIT) {                 /* unsigned int literal ('-' is a verb) */
            I v = scan_int(src, &p);
            EMITS(T_NOUN, ki(v));
        }
        else if (cl & CL_ALPHA) {            /* identifier or keyword */
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
            EMITS(T_NOUN, ks(buf));
        }
        else if (c == '\'') {               /* 'string' -> sym (string-as-sym; see SQL.md) */
            p++;
            int s = p;
            while (src[p] && src[p] != '\'') p++;
            if (!src[p]) die("sql: unterminated string literal");
            int len = p - s;
            if (len >= MAX_NAME) die("string literal too long");
            char buf[MAX_NAME];
            memcpy(buf, src + s, (size_t)len);
            buf[len] = '\0';
            p++;                             /* closing quote */
            EMITS(T_NOUN, ks(buf));
        }
        else {
            switch (c) {
            case '=': p++; EMITS(T_VERB, kverb(0, verb_index('='))); break;
            case '+': p++; EMITS(T_VERB, kverb(0, verb_index('+'))); break;
            case '-': p++; EMITS(T_VERB, kverb(0, verb_index('-'))); break;
            case '*': p++; EMITS(T_VERB, kverb(0, verb_index('*'))); break;
            case '/': p++; EMITS(T_VERB, kverb(0, verb_index('%'))); break;  /* SQL / is K % */
            case '<':
                if (src[p+1] == '=') { p += 2; EMITS(T_VERB, ks("<=")); }
                else if (src[p+1] == '>') { p += 2; EMITS(T_VERB, ks("<>")); }
                else { p++; EMITS(T_VERB, kverb(0, verb_index('<'))); }
                break;
            case '>':
                if (src[p+1] == '=') { p += 2; EMITS(T_VERB, ks(">=")); }
                else { p++; EMITS(T_VERB, kverb(0, verb_index('>'))); }
                break;
            case '!':
                if (src[p+1] == '=') { p += 2; EMITS(T_VERB, ks("<>")); }  /* != normalises to <> */
                else die("sql: unexpected '!'");
                break;
            case ',': p++; EMITS(T_COMMA,  NULL); break;
            case '(': p++; EMITS(T_LPAREN, NULL); break;
            case ')': p++; EMITS(T_RPAREN, NULL); break;
            case ';': p++; EMITS(T_SEMI,   NULL); break;
            default:  die("sql: unexpected character");
            }
        }
    }

    if (n >= cap) { cap = cap ? cap * 2 : 32; toks = xrealloc(toks, (size_t)cap * sizeof(Token)); }
    toks[n++] = (Token){.kind = T_EOF, .start = p, .len = 0, .k = NULL};
#undef EMITS
    return (Tokens){toks, n};
}

/* STEP3: case-insensitive string compare (SQL keywords are case-insensitive;
 * we keep this local to avoid pulling in POSIX strcasecmp). */
static int ci_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

/* STEP3: is the current token the name keyword `kw` (case-insensitive)? */
static int sql_kw(Token *tk, const char *kw) {
    return tk->kind == T_NOUN && tk->k && tk->k->t == -KS && ci_eq(tk->k->s, kw);
}

static const char *SQL_KEYWORDS[] = {
    "select", "update", "delete", "from", "where", "group", "by",
    "having", "order", "asc", "desc", "limit", "offset", "distinct",
    "as", "set", "and", "or", "not", "in", "is", "like", "between",
    "null", NULL
};

/* STEP3: a reserved word ends an expression (it can't be a value); a name is
 * any other identifier. (A string literal whose text equals a keyword is a
 * documented corner case -- see SQL.md.) */
static int sql_is_reserved(Token *tk) {
    if (tk->kind != T_NOUN || !tk->k || tk->k->t != -KS) return 0;
    for (int i = 0; SQL_KEYWORDS[i]; i++)
        if (ci_eq(tk->k->s, SQL_KEYWORDS[i])) return 1;
    return 0;
}
static int sql_is_name(Token *tk) {
    return tk->kind == T_NOUN && tk->k && tk->k->t == -KS && !sql_is_reserved(tk);
}

/* STEP3: small AST constructors (2- and 3-element generic lists). */
static K node2(K a, K b)        { K w = ktn(KL, 2); kK(w)[0]=a; kK(w)[1]=b;            return w; }
static K node3(K a, K b, K c)   { K w = ktn(KL, 3); kK(w)[0]=a; kK(w)[1]=b; kK(w)[2]=c; return w; }

/* STEP3: flatten a top-level AND-chain into a flat list of conditions, so a
 * WHERE of pure ANDs lands in the c-list exactly like ksql's comma-separated
 * `where d>0,e<5`. A non-AND top (e.g. OR) is kept as one combined phrase.
 * Unwrapped `&` nodes are freed; their children are detached and kept. */
static void flatten_and(K e, K *buf, int *n) {
    if (e->t == KL && e->n == 3 && kK(e)[0]
        && kK(e)[0]->t == KV2 && kK(e)[0]->i == verb_index('&')) {
        K l = kK(e)[1], r = kK(e)[2];
        kK(e)[1] = NULL; kK(e)[2] = NULL;
        dec_ref(e);
        flatten_and(l, buf, n);
        flatten_and(r, buf, n);
    } else {
        if (*n >= MAX_VEC) die("sql: where clause too complex");
        buf[(*n)++] = e;
    }
}

static K parse_sql_expr(Parser *p, int min_prec);
static K parse_sql_prefix(Parser *p);
static K parse_sql_primary(Parser *p);

/* STEP3: binary-operator precedence (higher binds tighter); 0 means "not a
 * binary operator here", which ends the expression. SQL HAS precedence --
 * the one thing K's precedence-free, right-to-left grammar cannot model --
 * so SQL gets this dedicated precedence-climbing parser instead of reusing
 * parse_e. (That is why x-y-z is left-associative here but right in K.) */
static int sql_binop_prec(Token *tk) {
    if (tk->kind == T_NOUN) {
        if (sql_kw(tk, "or"))  return 1;
        if (sql_kw(tk, "and")) return 2;
        return 0;
    }
    if (tk->kind == T_VERB && tk->k) {
        if (tk->k->t == -KS) return 4;                 /* <= >= <> */
        if (tk->k->t == KV2) {
            int i = tk->k->i;
            if (i == verb_index('=') || i == verb_index('<') || i == verb_index('>')) return 4;
            if (i == verb_index('+') || i == verb_index('-')) return 5;
            if (i == verb_index('*') || i == verb_index('%')) return 6;
        }
    }
    return 0;
}

/* STEP3: consume the binary operator at the cursor, yielding its K head.
 * AND/OR map to the K verbs &/|; glyph and two-char operators already carry
 * their head on the token, so we just lift it. */
static K sql_binop_head(Parser *p) {
    Token *tk = cur(p);
    K head;
    if (tk->kind == T_NOUN) head = kverb(0, verb_index(sql_kw(tk, "and") ? '&' : '|'));
    else { head = tk->k; tk->k = NULL; }
    adv(p);
    return head;
}

static K parse_sql_expr(Parser *p, int min_prec) {
    K left = parse_sql_prefix(p);
    for (;;) {
        int prec = sql_binop_prec(cur(p));
        if (prec == 0 || prec < min_prec) break;
        K head = sql_binop_head(p);
        K right = parse_sql_expr(p, prec + 1);     /* left-associative */
        left = node3(head, left, right);
    }
    return left;
}

/* STEP3: prefix operators. NOT is looser than comparison (NOT a=b is
 * NOT(a=b)); unary minus binds tightest. Both reuse K's monadic verbs:
 * NOT -> ~: (not), unary minus -> -: (negate). */
static K parse_sql_prefix(Parser *p) {
    Token *tk = cur(p);
    if (sql_kw(tk, "not")) {
        adv(p);
        return node2(kverb(1, verb_index('~')), parse_sql_expr(p, 3));
    }
    if (tk->kind == T_VERB && tk->k && tk->k->t == KV2 && tk->k->i == verb_index('-')) {
        adv(p);
        return node2(kverb(1, verb_index('-')), parse_sql_expr(p, 7));
    }
    return parse_sql_primary(p);
}

/* STEP3: primary -- a value, a function call, or a parenthesised expr. A name
 * followed by '(' is a call, built as the ordinary K application (`fn;arg;..)
 * -- so sum(amt) is (`sum;`amt), the same tree ksql gets from `sum amt`. */
static K parse_sql_primary(Parser *p) {
    Token *tk = cur(p);
    if (tk->kind == T_NOUN) {
        if (sql_is_reserved(tk)) die("sql: expected an expression");
        K v = tk->k; tk->k = NULL;
        adv(p);
        if (at(p, T_LPAREN)) {                      /* function call */
            adv(p);
            K buf[MAX_VEC]; int m = 0;
            buf[m++] = v;
            if (!at(p, T_RPAREN)) {
                buf[m++] = parse_sql_expr(p, 0);
                while (at(p, T_COMMA)) {
                    if (m >= MAX_VEC) die("sql: too many arguments");
                    adv(p);
                    buf[m++] = parse_sql_expr(p, 0);
                }
            }
            expect(p, T_RPAREN, "sql: expected ')'");
            return klist(buf, m);
        }
        return v;
    }
    if (at(p, T_LPAREN)) {
        adv(p);
        K e = parse_sql_expr(p, 0);
        expect(p, T_RPAREN, "sql: expected ')'");
        return e;
    }
    die("sql: expected an expression");
    return NULL;   /* unreachable */
}

/* STEP3: a select item is expr with an optional alias. The alias is the
 * existing `:` assignment shape, so SELECT sum(amt) AS qty yields
 * (:;`qty;(`sum;`amt)) -- identical to the ksql phrase qty:sum amt, just
 * written the other way round. Both `AS name` and the bare `expr name` form
 * are accepted. */
static K parse_select_item(Parser *p) {
    K e = parse_sql_expr(p, 0);
    K alias = NULL;
    if (sql_kw(cur(p), "as")) {
        adv(p);
        if (!sql_is_name(cur(p))) die("sql: expected an alias name after 'as'");
        alias = cur(p)->k; cur(p)->k = NULL; adv(p);
    } else if (sql_is_name(cur(p))) {
        alias = cur(p)->k; cur(p)->k = NULL; adv(p);
    }
    if (alias) return node3(kverb(0, verb_index(':')), alias, e);
    return e;
}

static K parse_select_list(Parser *p) {
    K buf[MAX_VEC]; int n = 0;
    buf[n++] = parse_select_item(p);
    while (at(p, T_COMMA)) {
        if (n >= MAX_VEC) die("sql: too many columns");
        adv(p);
        buf[n++] = parse_select_item(p);
    }
    return klist(buf, n);
}

/* A bare comma-separated expression list (GROUP BY / ORDER BY columns). */
static K parse_expr_list(Parser *p) {
    K buf[MAX_VEC]; int n = 0;
    buf[n++] = parse_sql_expr(p, 0);
    while (at(p, T_COMMA)) {
        if (n >= MAX_VEC) die("sql: list too long");
        adv(p);
        buf[n++] = parse_sql_expr(p, 0);
    }
    return klist(buf, n);
}

static K parse_where(Parser *p) {
    K e = parse_sql_expr(p, 0);
    K buf[MAX_VEC]; int n = 0;
    flatten_and(e, buf, &n);
    return klist(buf, n);
}

/* SELECT [DISTINCT] (* | items) FROM t [WHERE ...] [GROUP BY ...]
 *        [ORDER BY cols [ASC|DESC]] [LIMIT n [OFFSET m]]
 * The relational core fills (`select; t; c; b; a); DISTINCT/ORDER BY/LIMIT
 * wrap the result as ordinary K verbs, in SQL's logical processing order:
 *   SELECT a FROM t ORDER BY x LIMIT 10  ==  10 # `x asc select a from t  ==
 *     (#;10;(`asc;`x;(`select;`t;();();(`a)))). */
static P parse_select(Parser *p) {
    adv(p);                                        /* 'select' */

    int distinct = 0;
    if (sql_kw(cur(p), "distinct")) { adv(p); distinct = 1; }

    K a = NULL;
    if (cur(p)->kind == T_VERB && cur(p)->k && cur(p)->k->t == KV2
        && cur(p)->k->i == verb_index('*')) {      /* `*` is `all columns` -> () */
        adv(p);
        a = ktn(KL, 0);
    } else if (sql_kw(cur(p), "from")) {
        die("sql: expected a select list or '*'");
    } else {
        a = parse_select_list(p);
    }

    if (!sql_kw(cur(p), "from")) die("sql: expected 'from'");
    adv(p);
    K t = parse_sql_expr(p, 0);                    /* single-table from */

    K c = ktn(KL, 0);
    if (sql_kw(cur(p), "where")) { adv(p); c = parse_where(p); }

    K b = ktn(KL, 0);
    if (sql_kw(cur(p), "group")) {
        adv(p);
        if (!sql_kw(cur(p), "by")) die("sql: expected 'by' after 'group'");
        adv(p);
        b = parse_expr_list(p);
    }

    if (sql_kw(cur(p), "having")) die("sql: 'having' is not supported (deferred)");

    int has_order = 0, desc = 0;
    K ocols = NULL;
    if (sql_kw(cur(p), "order")) {
        adv(p);
        if (!sql_kw(cur(p), "by")) die("sql: expected 'by' after 'order'");
        adv(p);
        K cols = parse_expr_list(p);
        if (cols->n == 1) { ocols = kK(cols)[0]; kK(cols)[0] = NULL; dec_ref(cols); }
        else ocols = cols;
        if (sql_kw(cur(p), "asc")) adv(p);
        else if (sql_kw(cur(p), "desc")) { adv(p); desc = 1; }
        has_order = 1;
    }

    int has_limit = 0, has_off = 0;
    I lim = 0, off = 0;
    if (sql_kw(cur(p), "limit")) {
        adv(p);
        if (!(cur(p)->kind == T_NOUN && cur(p)->k && cur(p)->k->t == -KI))
            die("sql: expected a number after 'limit'");
        lim = cur(p)->k->i; adv(p);
        if (sql_kw(cur(p), "offset")) {
            adv(p);
            if (!(cur(p)->kind == T_NOUN && cur(p)->k && cur(p)->k->t == -KI))
                die("sql: expected a number after 'offset'");
            off = cur(p)->k->i; adv(p); has_off = 1;
        }
        has_limit = 1;
    }

    if (!(at(p, T_EOF) || at(p, T_SEMI))) die("sql: unexpected token after query");

    K r = emit_query(ks("select"), t, c, b, a);
    if (distinct)  r = node2(kverb(1, verb_index('?')), r);          /* ?: unique */
    if (has_order) r = node3(ks(desc ? "dsc" : "asc"), ocols, r);    /* kparc dyadic sort */
    if (has_limit) {
        if (has_off) r = node3(kverb(0, verb_index('_')), ki(off), r);  /* _ drop offset */
        r = node3(kverb(0, verb_index('#')), ki(lim), r);              /* # take limit */
    }
    return (P){R_NOUN, r};
}

/* UPDATE t SET col=expr, ... [WHERE ...]. SET assignments are the same
 * (:;`col;expr) shape as a SELECT alias, so they fill the a-slot; b is (). */
static P parse_update(Parser *p) {
    adv(p);                                        /* 'update' */
    if (!sql_is_name(cur(p))) die("sql: expected a table name after 'update'");
    K t = cur(p)->k; cur(p)->k = NULL; adv(p);

    if (!sql_kw(cur(p), "set")) die("sql: expected 'set'");
    adv(p);

    K buf[MAX_VEC]; int n = 0;
    for (;;) {
        if (!sql_is_name(cur(p))) die("sql: expected a column name in 'set'");
        K col = cur(p)->k; cur(p)->k = NULL; adv(p);
        if (!(cur(p)->kind == T_VERB && cur(p)->k && cur(p)->k->t == KV2
              && cur(p)->k->i == verb_index('='))) die("sql: expected '=' in 'set'");
        adv(p);                                    /* the `=` is assignment, built as `: */
        if (n >= MAX_VEC) die("sql: too many assignments");
        buf[n++] = node3(kverb(0, verb_index(':')), col, parse_sql_expr(p, 0));
        if (at(p, T_COMMA)) { adv(p); continue; }
        break;
    }
    K a = klist(buf, n);

    K c = ktn(KL, 0);
    if (sql_kw(cur(p), "where")) { adv(p); c = parse_where(p); }

    if (!(at(p, T_EOF) || at(p, T_SEMI))) die("sql: unexpected token after query");
    return (P){R_NOUN, emit_query(ks("update"), t, c, ktn(KL, 0), a)};
}

/* DELETE FROM t [WHERE ...] -> (`delete; t; c; (); ()). SQL's DELETE only
 * drops rows (fills c); ksql's column-drop `delete cols from t` has no SQL
 * surface. */
static P parse_delete(Parser *p) {
    adv(p);                                        /* 'delete' */
    if (!sql_kw(cur(p), "from")) die("sql: expected 'from'");
    adv(p);
    K t = parse_sql_expr(p, 0);

    K c = ktn(KL, 0);
    if (sql_kw(cur(p), "where")) { adv(p); c = parse_where(p); }

    if (!(at(p, T_EOF) || at(p, T_SEMI))) die("sql: unexpected token after query");
    return (P){R_NOUN, emit_query(ks("delete"), t, c, ktn(KL, 0), ktn(KL, 0))};
}

static P parse_sql_query(Parser *p) {
    Token *tk = cur(p);
    if (sql_kw(tk, "select")) return parse_select(p);
    if (sql_kw(tk, "update")) return parse_update(p);
    if (sql_kw(tk, "delete")) return parse_delete(p);
    die("sql: expected SELECT, UPDATE, or DELETE");
    return EMPTY;
}

static void run(const char *src, Mode mode) {
    /* STEP3: pick the scanner and top-level parser by mode; both converge on
     * the same print_k / dec_ref / free_tokens below. */
    if (mode == M_SQL) {
        Tokens ts = scan_sql(src);
        Parser p = {.src = src, .t = ts, .pos = 0};
        K e = parse_sql_query(&p).v;
        if (at(&p, T_SEMI)) adv(&p);               /* optional trailing ';' */
        if (!at(&p, T_EOF)) die("sql: unexpected token");
        print_k(e);
        putchar('\n');
        dec_ref(e);
        free_tokens(ts);
        return;
    }
    Tokens ts = scan(src);
    Parser p = {.src = src, .t = ts, .pos = 0};
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

int main(int argc, char **argv) {
    init_class();
    Mode mode = M_KSQL;   /* default keeps STEP 1 + STEP 2 behaviour intact */
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--sql"))  mode = M_SQL;
        else if (!strcmp(argv[i], "--ksql")) mode = M_KSQL;
        else { fprintf(stderr, "usage: %s [--sql|--ksql]\n", argv[0]); return 2; }
    }
    /* STEP3: an interactive prompt names the active mode (ksql> / sql>) so SQL
     * typed into ksql mode -- which parses as legal-but-wrong K -- is obvious.
     * When stdin is piped (the golden test runner), keep the plain two-space
     * prompt the runner strips, so the mode indicator never reaches tests. */
    int interactive = isatty(STDIN_FILENO);
    char line[1024];
    for (;;) {
        fputs(interactive ? (mode == M_SQL ? "sql> " : "ksql> ") : "  ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) { putchar('\n'); break; }
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;
        /* STEP3: live mode toggles (consumed, not parsed). */
        if (!strcmp(line, "\\sql"))  { mode = M_SQL;  continue; }
        if (!strcmp(line, "\\ksql")) { mode = M_KSQL; continue; }
        run(line, mode);
    }
    return 0;
}
