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

The named-monadic move is the headline, but this step implements the other
q surface features that are *pure grammar* — that reuse existing node
shapes and cost the parser nothing new: named dyads (`lj`, `bin`, …),
table/dict literals (they desugar to `flip` and `!`), and lambda parameter
lists. What it leaves out is everything that would need new value types or
an evaluator, for the same reason `kparser` defers floats, strings, and
eval: the project's contract is to demonstrate *grammar* — how the surface
maps to trees — and stop. q's value types, keyword dictionary, and
namespace system are eval-time work, not grammar work.

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
- **Control forms** (`$[c;t;f]`, `if[…]`, `while[…]`, `do[…]`) — these
  ride the existing `t[E]` application shape with a reserved head, the
  same desugaring trick used for `select` in [`KSQL.md`](KSQL.md). Free
  for the parser, but again a separate feature.
- **`insert` / `upsert`** — the `[t;d]` shape, deferred in ksql too.

What *is* implemented beyond the named verbs, because it's pure grammar
over nodes K already has:

- **Table/dict literals** (`([]a:1 2;b:3 4)`, `([k:v] a:1)`) — desugar to
  `flip` (monadic `+`) and `!` (dict), the same verbs K already has, so
  `([]a:1 2;b:3 4)` parses to `` (flip;(!;`a`b;(1 2;3 4))) ``. `parse_base`
  peeks for a `[` right after `(` and hands off to a small `parse_table`;
  no new type code, just assembly of existing `KV1`/`KV2` nodes.
- **Lambda parameter lists** (`{[a;b] a+b}`) — the `[...]` inside `{...}`
  is parsed by `parse_base`'s `T_LBRACE` handler before `parse_term` sees
  it, so there's no conflict with `f[x]` indexing. Each param must be a
  bare name; without params the AST is the same 2-element KL as before.

And one thing that *looks* like a q addition but isn't:

- **`::`** (global assign, view, identity) is already in K. kparser
  represents it as `knull()` — the monadic colon, `KV1` index 0. q adds
  nothing here.

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

For a parser this is a gift: a bare glyph is unambiguously the dyadic
verb (`KV2`), and a keyword is the named monadic verb (`KV1`). Explicit
glyph-colon forms such as `+:` and `::` are still `KV1` verb terms, so the
parser also keeps one provenance bit saying whether a `KV1` came from a q
named keyword or from glyph-colon syntax.

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

And here's the payoff: **the existing `nve`/`te` machinery already routes
q keywords the right way**, as long as the lexer tags them as verbs:

- `count x` → leading verb → `te` → `(count; x)` (unary application)
- `t lj u`  → noun, dyadic verb, rest → `nve` → `(lj; t; u)` (binary infix)
- `n til x`  → noun, *monadic* verb → not an infix → `te` → `(n; (til; x))`

That's mostly the same role-based decision `parse_e` already makes (branch
on `R_NOUN`/`R_VERB`). The one thing q adds is that the infix choice must
now consult source provenance: a named monadic keyword can't be claimed as
an infix verb, but an explicit glyph-colon verb such as `+:` can. This is
cheap because the scanner sets a bit on keyword-origin `KV1` nodes. Full
rank (does monadic `til` accept *this* value?) still stays an evaluation-time
concern, consistent with the "verbs are arity-agnostic" design note in
[`README.md`](README.md).

## The code diff

Three spots, each small and localized. The `parse_term` adverb loop,
`parse_base`, `parse_E`, the K types, the ref-counting, and the ksql layer
are all untouched. The `nve`/`te` role machinery in `parse_e_from` gains
two small arity checks — the two halves of §2 — but its structure is
otherwise unchanged.

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
    EMIT(T_VERB, kverb_qname(midx)); /* KV1 plus named-keyword provenance */
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

The monadic node keeps the same `KV1` type and same `VERB_CHARS` index in
`i`; the existing unused `u` byte stores the spelling provenance. No new
type code is needed. The parser uses that bit to distinguish keyword
monadics from explicit glyph-colon forms, and the printer uses it to render
keyword spellings by name (see §3).

Each table is a flat `strcmp` loop. The monadic table uses the same
`VERB_CHARS` indices kparser already defines (`flip`→`+`, `neg`→`-`,
`til`→`!`, `where`→`&`, …), ~20 entries; the dyadic table (`lj`, `bin`,
`wavg`, `xbar`, `asof`, `cross`) uses indices starting at `NVERBS` so they
don't collide with the glyphs. Both are a tiny slice of q's ~170-entry
dictionary; the rest are ordinary names that need no lexer recognition,
because only verbs affect the parser's role/arity logic.

### 2. Parser: two arity checks in `parse_e_from`

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

So `+1` is a parse error in q; the user writes `flip 1`. The demotion is
replaced by rejection: the parser gets smaller (the demotion logic is gone)
and stricter (a dyadic glyph in monadic position is caught at parse time,
not eval).

That handles a *dyadic* glyph in *monadic* position. The mirror case is a
*named monadic keyword* in infix position, and it lives in the nve branch of
the same function. In K every `noun verb rest` is an infix `nve`. In q a
named monadic such as `til` or `flip` must not be claimed as an infix, while
an explicit glyph-colon `KV1` such as `+:` or `::` still may be. The nve
branch therefore skips only nodes with named-keyword provenance:

```c
/* nve fires for ordinary verb terms, but not q named monadic keywords.
 * Explicit glyph-colon KV1 forms keep infix/update behavior. */
if (t.role == R_NOUN && u.role == R_VERB && !is_q_named_monadic(u.v)) {
    ...   /* build the 3-element infix (u; t; rest) */
}
```

Skipping the branch lets the named monadic fall through to `te`, where it
heads its own application. So `f til 10` is *not* `til[f;10]`; it is `f`
applied to `(til 10)` — `` (`f;(til;10)) `` — and `x til 10` is
`` (`x;(til;10)) ``. Explicit glyph-colon forms still infix: `f +: x` becomes
`` (+:;`f;`x) ``. Adverb-derived verbs are `KL` (variadic), not provenance-
marked `KV1`, so `1 +/ 2 3` and `1 count/ 2 3` still infix. Between them,
the two checks are the parser-side diff: reject a `KV2` in monadic position,
and don't infix a q named monadic keyword.

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
not `#:`. But explicit glyph-colon source like `+:` should remain visible
as glyph+colon. The printer splits the `KV1` and `KV2` cases: a `KV1` with
named-keyword provenance and a name in `MONADIC_NAMES` prints the name,
otherwise glyph+colon; a `KV2` whose index is in the dyadic range prints
its `DYADIC_NAMES` entry, otherwise the bare glyph:

```c
case KV1:
    if ((x->u & V_QNAME) && x->i >= 1 && x->i - 1 < (int)NMONADICS)
        printf("%s", MONADIC_NAMES[x->i - 1]);
    else { putchar(VERB_CHARS[x->i]); putchar(':'); }
    break;
case KV2:
    if (x->i < (int)NVERBS) putchar(VERB_CHARS[x->i]);
    else printf("%s", DYADIC_NAMES[x->i - (int)NVERBS]);
    break;
```

No new type code is needed — the existing `u` byte carries the provenance
bit, and the name is looked up from the index at print time. A named dyad is
just a `KV2` whose index falls past the glyphs.

## What changes at the parse tree

The demotion check is the one that *changes a tree* between K and q: a
dyadic glyph with no left operand. The nve check is different — it doesn't
create a K-vs-q divergence, it *preserves the K shape*: because a named
monadic follows a noun as a `te` (not an infix), `x til 10` has the same
structure in both languages, just a different head rendering.

| input | K (kparser today) | q (named monadics) |
|-------|-------------------|--------------------|
| `+1` | `(+:;1)` | error² |
| `2+1` | `(+;2;1)` | `(+;2;1)` |
| `til 10` | `(`til;10)`¹ | `(til;10)` |
| `x til 10` | `` (`x;(`til;10)) ``¹ | `` (`x;(til;10)) `` |
| `f +: x` | `(+:;`f;`x)` | `(+:;`f;`x)` |
| `2+/til 10` | `((`/;+);2;(`til;10))`¹ | `((`/;+);2;(til;10))` |

¹ In K, `til` is an ordinary name (noun); `til 10` is juxtaposition `te`
→ `(`til;10)`. In q, `til` is a `KV1` verb; `til 10` is `te` with verb
head → `(til;10)`. Same shape, different head — the `KV1` node is the
same, but the printer renders it as `til` (from the name table) rather
than `` `til `` (a sym atom).

² In q, `+` is strictly dyadic. With no left operand and no demotion,
`+1` is a parse error — `+` can't be monadic (that's `flip`), and there's
no left operand for dyadic add. The user writes `flip 1` instead.

The `x til 10` row is where the nve check earns its keep. `til` is a `KV1`
verb with named-keyword provenance, but K's rule "noun, verb, rest → infix"
would wrongly build `` (til;`x;10) `` — a two-argument application of a
one-argument verb. The nve check excludes that provenance, so the verb falls
through to `te`: `til` heads its own application `(til 10)`, and the leading
`x` applies to that. Explicit glyph-colon `KV1` terms such as `+:` do not
carry that provenance, so they still take the nve branch.

The `2+/til 10` row is worth noting: it works in *both* K and q, because
the `2` gives `+` a left operand, making `+/` dyadic-over (fold with
seed). The derived verb `(/; +)` takes two arguments — seed and list —
and both languages parse it the same way. The difference only appears
when a glyph has *no* left operand and would need monadic inference:
`+1` (K demotes, q rejects).

One intentional divergence remains around adverb-derived verbs as implicit
`te` heads. The small grammar treats `f'`, `+/`, `count/`, and longer adverb
trains as ordinary verb terms, so forms like `f'1 2 3` or `+/til 10` parse
compositionally as implicit application; stricter q surfaces require bracket
application or parenthesizing for those cases (`f'[1 2 3]`, `(f')1 2 3`). We
keep the cleaner grammar here. To enforce the stricter rule, add an
`is_adverb_derived(K)` predicate for `KL` nodes headed by an adverb symbol and,
in the q `te` branch of `parse_e_from`, reject only when `t.role == R_VERB &&
is_adverb_derived(t.v)`. That preserves lone derived verbs, bracket calls,
infix `nve` use, and parenthesized heads.

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
  named monadic is `KV1`. No inference, no transformation — the demotion
  is gone. In its place are two trivial token tests: reject a `KV2` in
  monadic position, and don't infix a `KV1`. Guessing (a mutation of the
  tree) is replaced by reading (a one-bit check on the token).

This is the same lesson [`KSQL.md`](KSQL.md) reaches from the query side
(a query is sugar for an application, not new syntax) and
[`KSIMPLE.md`](KSIMPLE.md) reaches from the minimal side (strip K down
and the grammar nearly vanishes). q's contribution to the *parser* is
not new machinery: it deletes the one place K guesses about arity and
replaces it with two checks that just read the bit the token already
carries.

## The through-line

q looks like a different language, but structurally it is K with a
different verb naming convention. The complexity people associate with q
— the keyword dictionary, the literal/type surface, the namespaces — lives
in the lexer and the evaluator, not the grammar. The one grammatical move
q makes is naming the monadics, and that move trades the parser's arity
*inference* (K's demotion) for two arity *reads* off the token — less
logic, not more, and no new machinery.

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
