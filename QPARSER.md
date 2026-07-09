# q — Step 4: named monadics

A follow-on to [`README.md`](README.md), [`KSQL.md`](KSQL.md), and
[`SQL.md`](SQL.md). Step 1 (`kparser.c`) parses the core K grammar. Step 2
(`ksqlparser.c`) adds ksql. Step 3 (`sqlparser.c`) was a side-quest showing
that standard SQL targets the same AST as ksql. Step 4 demonstrates the one
q idea that touches the *parser*: **naming the monadic verbs**.

q is the query/analytics language layered on top of K. It keeps K's
evaluation model — right-to-left, no precedence, juxtaposition,
`f[x;y]` application, `{...}` lambdas, `;` sequencing — but resolves the
ambiguity around verbs by giving each monadic verb its own name. In K a
single glyph carries two verbs at once: `+` is *add* between two operands
but *flip* in front of one, and which you get depends on context. q splits
them: the glyph is always the dyadic verb, and the monadic counterpart gets
its own name. `+` is always add; `flip` is monadic `+`. `-` is always
subtract; `neg` is monadic `-`.

This step is *not* an implementation of q. It is an analysis of the
minimum change `kparser` would need to demonstrate that one idea — and the
surprising result is that the change is *negative*: q's naming convention
removes parser logic rather than adding it.

## Scope

This step demonstrates **only** the named-monadic design move. Everything
else q adds over K is out of scope, for the same reason `kparser` defers
floats, strings, and an evaluator: the project's contract is to demonstrate
*grammar* — how the surface maps to trees — and stop. Representing q's
value types, keyword dictionary, and namespace system is eval-time work,
not grammar work.

What is explicitly deferred:

- **q's literal/type surface** — type suffixes (`1b`, `0x2a`, `3h`),
  char/string (`"abc"`), temporals (`2024.01.01`), nulls/infinities
  (`0N`, `0W`). These are value-representation concerns, the same line
  `README.md` draws under "what's missing." q has ~19 types; kparser has
  5, and that's fine for demonstrating grammar.
- **q's full keyword table** (~170 entries: `sum`, `avg`, `lj`, `asof`,
  `fby`, …). These are ordinary names defined in the `.q` context; they
  parse as `-KS` nouns and apply via the existing `t[E]` / `nve` shapes.
  No grammar change. Only the ~20 monadic verb names (`flip`, `neg`,
  `count`, …) and a handful of dyadic verb names (`lj`, `bin`, …) need
  lexer recognition — the rest are ordinary names.
- **Namespace/context references** (`.q.foo`, `.z.ts`) — a lexer concern
  (leading-dot names), not a grammar one.
- **Table/dict literals** (`([]a:1 2;b:3 4)`, `([k:v] a:1)`) — desugar to
  `!` (dict) and `flip` (monadic `+`), the same verbs K already has.  No
  new type code; the parser assembles existing KV1/KV2 nodes.
- **Control forms** (`$[c;t;f]`, `if[…]`, `while[…]`, `do[…]`) — these
  ride the existing `t[E]` application shape with a reserved head, the
  same desugaring trick used for `select` in [`KSQL.md`](KSQL.md). Free
  for the parser, but again a separate feature.
- **`insert` / `upsert`** — the `[t;d]` shape, deferred in ksql too.

A note on two things that *look* like q additions but aren't:

- **`::`** (global assign, view, identity) is already in K. kparser
  represents it as `knull()` — the monadic colon, `KV1` index 0. q adds
  nothing here.
- **Lambda parameter lists** (`{[a;b] a+b}`) — implemented. The `[...]`
  inside `{...}` is parsed by `parse_base`'s T_LBRACE handler before
  `parse_term` sees it, so there's no conflict with `f[x]` indexing.
  Each param must be a bare name; without params the AST is the same
  2-element KL as before.

## The one idea: one token, one verb

q was a targeted redesign aimed at removing the ambiguity around verbs.
The full table:

```
glyph  dyadic         monadic (named)
  +    add            flip
  -    subtract       neg
  *    multiply       first
  %    divide         reciprocal
  &    and / min      where
  |    or / max       reverse
  <    less           iasc
  >    greater        idesc
  =    equal          group
  ~    match          not
  !    key            til
  #    take           count
  _    drop / cut     floor
  ,    join           enlist
  ^    fill           null
  $    cast           string
  ?    find           distinct
  @    apply / index  type
  .    apply          get
```

For a parser this is a gift: a glyph is unambiguously the dyadic verb
(`KV2`), and a keyword is the named monadic verb (`KV1`), so there is far
less to work out at parse time than in K.

## The grammar change

There isn't one. The five productions are untouched:

```
E : E ; e | e
e : nve | te | empty
t : n | v
v : tA | V
n : t[E] | (E) | {E} | N
```

What changes is the *terminal* `V` — the set of tokens the scanner
recognizes as a verb. In K, `V` is exactly the primitive glyphs
(`+ - * % …`, optionally with a trailing `:`), and every name (`flip`,
`count`, `lj`) lexes as a noun `N`. In q, the scanner also tags a fixed
set of names as verbs: the ~20 named monadics as `KV1` and a handful of
named dyads as `KV2`. So a q keyword verb enters the grammar through the
same `v : … | V` slot a glyph does — the parser never learns a new rule,
it just sees more tokens in the `V` class. This is a lexer change, not a
grammar one: `parse_base` returns `R_VERB` for a `T_VERB` token regardless
of whether it came from a glyph or a keyword.

And here's the payoff: **the existing `nve`/`te` machinery already absorbs
q keywords for free**, as long as the lexer tags them as verbs:

- `count x` → leading verb → `te` → `(count; x)` (unary application)
- `t lj u`  → noun, named-verb, rest → `nve` → `(lj; t; u)` (binary infix)

That's the same role-based decision `parse_e` already makes (branch on
`R_NOUN`/`R_VERB`); it never needed a verb's *arity*, only its *role*. So
rank stays an evaluation-time concern — consistent with the "verbs are
arity-agnostic" design note in [`README.md`](README.md). We wouldn't need a
rank table to get the tree shapes right.

## The code diff

Three spots, each small and localized. The `nve`/`te` role machinery, the
`parse_term` adverb loop, `parse_base`, `parse_E`, the K types, the
ref-counting, the ksql layer — all untouched.

### 1. Scanner: small keyword tables

Today every alphanumeric token becomes a noun (`-KS`):

```c
else if (cl & CL_ALPHA) {
    while (CLASS[(uint8_t)src[p]] & (CL_ALPHA | CL_DIGIT)) p++;
    ...
    EMIT(T_NOUN, ks(buf));
    noun_pos = 1;
}
```

(`kparser.c`, the `CL_ALPHA` branch of `scan`.)

q needs two tables: one mapping the ~20 monadic names to their `VERB_CHARS`
index (`KV1`), and a small one mapping a handful of named dyads (`lj`,
`bin`, …) to `KV2` verb indices. After scanning a name, look it up; a
monadic keyword emits a `KV1` verb, a dyadic keyword a `KV2` verb, and
everything else stays a `-KS` noun as before:

```c
int midx = monadic_keyword(buf);     /* -1 if not a monadic keyword */
if (midx >= 0) {
    EMIT(T_VERB, kverb(1, midx));    /* KV1, same node K's demotion would build */
    noun_pos = 0;
} else {
    int didx = dyadic_keyword(buf);  /* -1 if not a dyadic keyword */
    if (didx >= 0) {
        EMIT(T_VERB, kverb(0, didx)); /* KV2, a named dyadic verb */
        noun_pos = 0;
    } else {
        EMIT(T_NOUN, ks(buf));       /* ordinary name, as before */
        noun_pos = 1;
    }
}
```

The monadic node is identical to what K's demotion would have produced —
same `KV1` type, same `VERB_CHARS` index in `i`. No new type code, no new
storage. The only difference is the printer renders it by name (see §3).

Each table is a flat `strcmp` loop. The monadic table uses the same
`VERB_CHARS` indices kparser already defines (`flip`→`+`, `neg`→`-`,
`til`→`!`, `where`→`&`, …), ~20 entries; the dyadic table (`lj`, `bin`,
`wavg`, `xbar`, `asof`, `cross`) uses indices starting at `NVERBS` so they
don't collide with the glyphs. Both are a tiny slice of q's ~170-entry
dictionary; the rest are ordinary names that need no lexer recognition,
because only verbs affect the parser's role/arity logic.

### 2. Parser: the demotion block becomes a hard error

This is the heart of the demonstration. kparser's `parse_e_from` contains
a block that exists *only* because K overloads one glyph for two arities
(`kparser.c`, inside `parse_e_from`):

```c
/* Verb head in monadic position: build a fresh KV1 from the dyadic
 * primitive's index, then release the old. Adverb-modified verbs are
 * KL, so this check skips them. */
if (t.role == R_VERB && t.v && t.v->t == KV2) {
    K old = t.v;
    t.v = kverb(1, old->i);          /* <-- the demotion */
    dec_ref(old);
}
```

When a dyadic glyph (`KV2`) lands in monadic position — as the head of a
`te`, e.g. `+ 1 2 3` — the parser *infers* it must be the monadic form and
builds a fresh `KV1` from the same verb index. `+1` becomes `(+:;1)`.

In q that inference is wrong: a glyph is always dyadic, and there is no
monadic form to demote to. The block is replaced with a hard error:

```c
if (t.role == R_VERB && t.v && t.v->t == KV2) {
    die("q: dyadic verb in monadic position; use the named monadic");
}
```

So `+1` is a parse error in q; the user writes `flip 1`. This is the
entire parser-side diff: the demotion is replaced by rejection. The
parser gets smaller (the demotion logic is gone) and stricter (a
dyadic glyph in monadic position is caught at parse time, not eval).

### 3. Printer: render named monadics by name

kparser's printer renders `KV1` as glyph+colon (`+:`, `-:`):

```c
case KV1: case KV2:
    if (x->i >= 0 && x->i < (int)NVERBS) {
        putchar(VERB_CHARS[x->i]);
        if (x->t == KV1) putchar(':');
    }
    ...
```

(`kparser.c`, `print_k`.)

For q's named monadics this is wrong — `count` should print as `count`,
not `#:`. But the `KV1` node is the same in both cases (same type, same
index); the difference is purely in print. The printer splits the `KV1`
and `KV2` cases: a `KV1` with a name in `MONADIC_NAMES` prints the name,
otherwise glyph+colon; a `KV2` whose index is in the dyadic range prints
its `DYADIC_NAMES` entry, otherwise the bare glyph:

```c
case KV1:
    if (x->i >= 1 && x->i - 1 < (int)NMONADICS)
        printf("%s", MONADIC_NAMES[x->i - 1]);
    else { putchar(VERB_CHARS[x->i]); putchar(':'); }
    break;
case KV2:
    if (x->i < (int)NVERBS) putchar(VERB_CHARS[x->i]);
    else printf("%s", DYADIC_NAMES[x->i - (int)NVERBS]);
    break;
```

No new type code, no new storage on the K node — the name is looked up
from the index at print time. The same `KV1` node that K's demotion
would have built is rendered differently because the name table exists,
and a named dyad is just a `KV2` whose index falls past the glyphs.

## What changes at the parse tree

The demotion block's replacement with a hard error becomes visible in
exactly one place: a dyadic glyph in monadic position. In K, the parser
demotes it to `KV1`. In q, it is a parse error — there is no monadic form
of a glyph, and the parser no longer infers one.

The simplest example is `+1` — a dyadic glyph with one operand:

| input | K (kparser today) | q (named monadics) |
|-------|-------------------|--------------------|
| `+1` | `(+:;1)` | error² |
| `2+1` | `(+;2;1)` | `(+;2;1)` |
| `til 10` | `(`til;10)`¹ | `(til;10)` |
| `2+/til 10` | `((`/;+);2;(`til;10))`¹ | `((`/;+);2;(til;10))` |

¹ In K, `til` is an ordinary name (noun); `til 10` is juxtaposition `te`
→ `(`til;10)`. In q, `til` is a `KV1` verb; `til 10` is `te` with verb
head → `(til;10)`. Same shape, different head — the `KV1` node is the
same, but the printer renders it as `til` (from the name table) rather
than `` `til `` (a sym atom).

² In q, `+` is strictly dyadic. With no left operand and no demotion,
`+1` is a parse error — `+` can't be monadic (that's `flip`), and there's
no left operand for dyadic add. The user writes `flip 1` instead.

The `2+/til 10` row is worth noting: it works in *both* K and q, because
the `2` gives `+` a left operand, making `+/` dyadic-over (fold with
seed). The derived verb `(/; +)` takes two arguments — seed and list —
and both languages parse it the same way. The difference only appears
when a glyph has *no* left operand and would need monadic inference:
`+1` (K demotes, q rejects).

### ksql: `where`/`count`/`first` become the monadic names

A nice consequence: the three ksql clause keywords that are also q monadic
verbs — `where`, `count`, `first` — scan as `KV1` verbs in q, not `-KS`
nouns. The ksql layer's `sym_is` helper (which recognizes clause keywords
positionally by name) is extended to look up a `KV1`'s name from its
`VERB_CHARS` index via the `MONADIC_NAMES` table. So `where` is *both* a q
monadic verb and a ksql clause keyword — the same token, recognized
positionally in both roles, exactly the ksql design. No separate keyword
table for ksql; the q monadic names *are* the ksql keywords where they
overlap.

## Why this is a simplification, not a complication

Giving monadic verbs their own names makes the parser's arity logic
simpler, not harder:

- In K, the parser *infers* arity from position. A glyph in monadic
  position is demoted to `KV1` by the demotion block — a runtime
  transformation on the AST node. The parser is doing work that, strictly,
  belongs to a valence-aware evaluator.
- In q, the parser *reads* arity from the token. A glyph is `KV2`; a
  named monadic is `KV1`. No inference, no transformation. The demotion
  block is dead code.

This is the same lesson [`KSQL.md`](KSQL.md) reaches from the query side
(a query is sugar for an application, not new syntax) and
[`KSIMPLE.md`](KSIMPLE.md) reaches from the minimal side (strip K down
and the grammar nearly vanishes). q's contribution to the *parser* is
negative: it removes the one place the parser guesses about arity.

## The through-line

q looks like a different language, but structurally it is K with a
different verb naming convention. The complexity people associate with q
— the keyword dictionary, the literal/type surface, the namespaces — lives
in the lexer and the evaluator, not the grammar. The one grammatical move
q makes is naming the monadics, and that move *removes* parser logic
rather than adding it.

The five steps then form a clean progression, each demonstrating one
grammatical idea with a minimal diff:

- **Step 1** (`kparser.c`): K's five productions, with arity inferred by
  position.
- **Step 2** (`ksqlparser.c`): queries are a fourth noun base desugaring
  to `t[E]`.
- **Step 3** (`sqlparser.c`): standard SQL is a second surface over the
  same query AST.
- **Step 4** (`qparser.c`): naming the monadics removes the parser's arity
  inference.
- **Step 5** (`uparser.c`): K and q differ in exactly two places; they
  share one parser with a mode switch ([`UPARSER.md`](UPARSER.md)).

Each step is a minimal diff that makes one structural point. Step 2 shows
that queries aren't new syntax. Step 3 shows that SQL and ksql converge on
one tree. Step 4 shows that q's verb-naming isn't new syntax either — it's
the *removal* of a parser-side workaround.
