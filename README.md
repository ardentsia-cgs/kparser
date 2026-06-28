# kparser

[![CI](https://github.com/ardentsia-cgs/kparser/actions/workflows/ci.yml/badge.svg)](https://github.com/ardentsia-cgs/kparser/actions/workflows/ci.yml)
[![codecov](https://codecov.io/github/ardentsia-cgs/kparser/graph/badge.svg?token=U3C7E272T5)](https://codecov.io/github/ardentsia-cgs/kparser)
[![License: Apache 2.0](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

A small, readable parser for the K programming language. One C file,
about 570 lines including comments. It's a pedagogical reference — its
job is to make the K grammar legible.

There is no evaluator here. The parser reads K source from a REPL and
prints the resulting AST in a lisp-style nested-list form. Once you can
see how `2+3` becomes `(+;2;3)`, why `1 -2 3` is one vector but `1-2` is
subtraction, and how lambdas, adverbs, and indexing all reuse the same
shape, the rest of K stops being mysterious.

## Quick start

```sh
make
./kparser
```

You get a two-space prompt and one line of AST per input. Ctrl-D exits.

`make test` runs a golden suite (`tests/run.sh` + `tests/cases.tsv`) that
exercises every route through the scanner, parser, and printer.

```
  2+3
(+;2;3)
  a:b:42
(:;`a;(:;`b;42))
  1 -2 3
1 -2 3
  +/1 2 3 4
((`/;+);1 2 3 4)
  {x+y}
(`{;(+;`x;`y))
  f[1;2]
(`f;1;2)
```

## The grammar

K has one of the most compact grammars of any programming language.
Lifted from <https://k.miraheze.org/wiki/Grammar>:

```
E : E ; e | e             // semicolon-separated expressions
e : nve | te | empty      // dyadic, juxtaposition, or empty
t : n | v                 // term is noun or verb
v : tA | V                // verb base, or term modified by adverb
n : t[E] | (E) | {E} | N  // noun base, or term followed by index

A : ' | / | \ | ': | /: | \:
V : <one of> : + - * % ! & | < > = ~ , ^ # _ $ ? @ .  (optionally + ':')
N : <Name> | <Int>(s) | <Symbol>(s)
```

Those five productions are the *entire* structural grammar. Every K
program parses through them.

## Reading the AST

Each parsed expression becomes a nested list where the head is whatever
operates on the rest. The five shapes the parser produces:

| Source            | AST shape                       |
| ----------------- | ------------------------------- |
| `n v e`           | `(v; n; e)`                     |
| `t e`             | `(t; e)`, or just `t` if e is empty |
| `t[e1;e2;...]`    | `(t; e1; e2; ...)`              |
| `{e1;e2;...}`     | `` (`{; e1; e2; ...) ``         |
| `tA`              | `` (`A; t) ``                   |

Leaf values come from the K type system. There are five types in this
parser:

| Source        | K type | Display     |
| ------------- | ------ | ----------- |
| `42`          | `-KI`  | `42`        |
| `1 2 3`       | `KI`   | `1 2 3`     |
| `x`           | `-KS`  | `` `x ``    |
| `` `x ``      | `KS` (n=1) | `` ,`x ``   |
| `` `a`b`c ``  | `KS`   | `` `a`b`c ``|
| `+`, `-`, `:` (dyadic) | `KV2` | `+`         |
| `+:`          | `KV1`  | `+:`        |
| nested expr   | `KL`   | `(...)`     |

That's the entire type surface. Floats, strings, hex — anything that
would expand the type system — is deliberately out of scope.

A subtle point worth noticing: `x` (a name) and `` `x `` (a literal
symbol) are *both* sym-typed, but `x` is an atom and `` `x `` is a
one-element vector. The leading `,` in the printed form is the "enlist"
marker that distinguishes them. The K type code (negative for atom,
positive for vector) is doing the same work.

This lisp-style form is doing more than looking tidy. Because it is
prefix and fully parenthesized, every grouping decision K makes —
right-to-left order, which verb is monadic, where an argument list ends
— is made explicit, with no precedence left to infer. `2*3+4` reads
ambiguously to anyone carrying precedence habits; `(*;2;(+;3;4))` cannot
be misread. It is, in effect, a Rosetta stone between K's terse surface
and the S-expression shape that decades of Lisp have made ubiquitous —
one reason a parse tree is a useful thing to hand a tool reasoning about
what a K expression *means*.

But it is a *reading* aid, and a narrow one: best for seeing how a single
fragment grouped. A deeply nested tree is its own thing to read
carefully — the form earns its keep on shallow fragments and quietly
loses it as the nesting deepens.

## Code layout

The file is one continuous read in top-to-bottom order:

1. **Allocation helpers** (`die`, `xmalloc`, ...) — wrap the standard
   allocators with OOM-aborts so the parser never has to thread error
   returns through every function.
2. **K struct** — the runtime value type, a tagged union of atoms and
   vector storage. `t` is the type code, `r` is the refcount, `G0[]` is
   the flexible-array tail for vector data.
3. **Constructors** (`ki`, `ks`, `ktn`, `klist`) — make K values of each
   kind.
4. **Ref counting** (`inc_ref`, `dec_ref`) — every K is born with `r=0`
   ("one implicit owner"). `dec_ref` on `r=0` frees, recursing into
   container types.
5. **Verb tables** — primitive verbs indexed by their position in
   `VERB_CHARS`. The function-pointer arrays are stubbed; an evaluator
   would fill them in.
6. **Printer** (`print_k`) — emits the AST in the lisp-style form
   shown above.
7. **Scanner** (`scan`) — a single forward pass that builds tokens
   carrying K objects.
8. **Parser** (`parse_E`, `parse_e`, `parse_term`, `parse_base`) — one
   function per non-trivial production. If you want to understand the K
   grammar, this is the section to read; each function maps cleanly to
   one production rule.
9. **REPL** (`main`) — `fgets` loop.

## Design notes

A few decisions that took thought, in case you wonder why.

### Negative numbers vs subtraction

`-` is ambiguous: it can be the dyadic minus (`a-b`) or the sign on a
literal (`-1`). The rule:

> A leading `-` followed by a digit is a sign whenever the previous
> meaningful token was a verb, an adverb, an opener (`(`/`[`/`{`), `;`,
> or start of input. Otherwise it's the verb.

So `a-1` is subtraction, `a:-1` is assigning `-1` to `a`, and `1 -2 3`
is a three-element vector. The scanner tracks a `noun_pos` flag to
decide each `-` in one pass — no backtracking.

### Two-phase: the scanner builds K objects directly

A first cut had the scanner emit "tokens with parsed payloads" that the
parser then converted to K values. That meant every name was strdup'd
twice, every int vector was allocated twice, and every sym was copied
twice. The current design has the scanner build the K objects directly
and store them on the token; the parser just lifts them out (and sets
the slot to `NULL` so cleanup is uniform). Half the allocation traffic.

### Verbs default to dyadic, demote at AST-build time

A bare `+` parses as a dyadic verb (type `KV2`, code 102). The
te-with-verb-head case in `parse_e` builds a fresh monadic (`KV1`, code
101) when the verb is found in unary position — so `+1` becomes
`(+:;1)`. No mutation of K nodes; the demoted form is a new node.

### Choosing nve vs te by term role, not by token

`e : nve | te` turns on one question: after a leading noun, is the next
term a verb? A single token can't answer it, because a term's role isn't
fixed by its first character — `{x+y}` is a noun but `` {x+y}' `` is a
verb, and `f` is a noun but `` f' `` is a verb. So `parse_e` parses the
next *term* and branches on its role (`R_NOUN`/`R_VERB`) rather than
peeking the next token. This stays predictive and linear: `parse_term`
consumes nothing when there is no term, so the one-term lookahead is free
and never backtracks. The payoff is that adverb-derived verbs work in
infix position — `` 1 2 f' 3 4 `` parses as the `nve` shape
`` ((`';`f);1 2;3 4) ``, not as juxtaposition.

### Projections and compositions are emergent, not grammatical

Two things a K programmer treats as distinct features — *projections*
(fixing some of a function's arguments) and *compositions* (chaining
functions) — have no productions of their own. They fall out of the
same `t[E]` and `te`/`nve` shapes everything else uses, because the
grammar is arity-agnostic: it builds application and juxtaposition
trees and never asks how many arguments a verb wants. Whether a tree is
an application, a projection, or a composition is an *evaluation*-time
reading, settled once you know a verb's valence — which is exactly why
the verb tables exist but stay empty in a parser.

A **projection** is an application with a missing argument. K marks the
hole with its *generic null* `::` — and notably there is no separate
"missing" type: the generic null (the monadic colon, `KV1` index 0)
*is* the hole. The empty `e` (the `e : empty` rule) in an argument
position becomes `::`:

```
2+        ->  (+;2;::)        right arg of dyadic + elided
f[1;;3]   ->  (`f;1;::;3)     middle arg elided
f[;2]     ->  (`f;::;2)       left arg elided
f[]       ->  (`f;::)         f[] is f[::]
```

Note `::` is distinct from `()`: `()` is the empty *list* (a noun), while
`::` is the generic null filling an elided slot — so the parser keeps
them apart (the hole substitution happens only in `t[E]` argument lists
and the `nve` empty-operand case; an empty `(E)` stays `()`). An
evaluator counts the non-`::` slots against the verb's valence; too few
means "return a function awaiting the rest" — a projection. No new node
and no new type, just generic nulls in an ordinary apply tree.

A **composition** is juxtaposition (`te`) where the term and the
expression are both functions rather than function-and-noun:

```
|+        ->  (|:;+)          reverse atop plus
*|+       ->  (*:;(|:;+))     a right-associated verb train
```

That is the same `(t;e)` shape as `f x` application — the only
difference is whether `e` resolves to a noun (apply) or a verb
(compose), which again needs valence to settle. One wrinkle lives in
the te-branch demotion above: a bare verb head is demoted to monadic
(`` +* `` → `` (+:;*) ``) while a parenthesized one is not
(`` (+)(*) `` → `` (+;*) ``, because `(E)` yields a noun-typed term).
The parser commits to the application reading; a real K runtime would
revisit it knowing arities.

The lesson is the point of the whole grammar: K's surface stays tiny
because projection and composition are *not* new syntax. They are what
the five productions already mean under a valence-aware evaluator. The
parser's job is to get the shape right, and stop there.

### Lambda marker uses the sym `` `{ ``

A wrapping list for `{x+y}` is `` (`{; (+;`x;`y)) ``. Using a sym
(rather than inventing a new type) keeps the type surface small. A
real K runtime would replace this marker with an actual closure value;
for parse output, the sym is enough.

### Ref counting without an arena

Each parse allocates ~190 K nodes for a typical input. `dec_ref`
walks the AST at the end of each REPL line, which is correct and fast
enough. An arena allocator would shave allocation time, but the parser
isn't allocation-bound — an evaluator would be the right time to add
one.

### Bracket matching is the call stack, not a counter

The parser keeps no bracket-depth counter. In recursive descent the call
stack already *is* the counter — one `parse_E` per nesting level — so the
only discipline needed is that every level hit the terminator it expects:
a bracketed level its closer (`)`/`}`/`]`), the outermost level
end-of-input. A one-line `expect` helper enforces the closers; `run`
asserts `T_EOF` after the top-level parse. A missing closer or a stray
one is a hard `die`, fail-fast like the rest of the parser:

```
  1+(        kparser: expected ')'
  1+)        kparser: unexpected token
```

This is independent of the deliberate hole-filling: `f[]`, `f[;2]`, and
`2+` are *balanced*, so their elided slots still become `::`. Only
genuinely unbalanced input errors. (Richer messages naming the offending
position are the `start`/`len` plumbing listed under *Source-position
errors* below.)

## What's missing

In rough order of how much work each would be to add:

- **Float literals** (`1.5`, `1e10`) and float vectors — small
  extension to `scan_int` and the K type set.
- **String literals** (`"hello"`) — escape handling, new K type.
- **Hex byte vectors** (`0xff`) — separate scanner branch, new K type.
- **Source-position errors** — tokens already carry `start`/`len`, so
  this is plumbing.
- **An evaluator**. The verb function tables are sized and named but
  empty.

## Next: ksql (Step 2)

[`KSQL.md`](KSQL.md) is a follow-on that extends this parser with K's SQL
dialect — `select` / `exec` / `update` / `delete … by … from … where …`.
The surprise is how little it costs: a query turns out to be a fourth
*noun base* (`n : t[E] | (E) | {E} | N | q`) that desugars into the
application shape you already have, `` (`verb; t; c; b; a) ``. Aliases
(`qty:sum amt`) need no new syntax at all — they are the existing `:`
assignment. The Step 2 code lives in [`ksqlparser.c`](ksqlparser.c)
(`kparser.c` plus a ~120-line ksql layer); build it with `make ksqlparser`
and test it with `make test2`.

## Appendix: ksimple's grammar

[`KSIMPLE.md`](KSIMPLE.md) goes the other direction: it reads the implicit
grammar back out of Arthur Whitney's [`ksimple`](https://github.com/kparc/ksimple),
a ~100-line learning interpreter that has *no parser* — tokenizing,
parsing, and eval are fused into one recursive function over a tape of
one-byte tokens. It boils down to five productions, and keeps exactly the
two ideas that make K K (right-to-left with no precedence; arity from left
context) while dropping all structure (`(E)`, `{E}`, `t[E]`, `;`).

## Why hand-rolled?

For five productions, a parser generator is mostly overhead — but that's
the easy answer. The more interesting reason is that K's *lexer* is where
the difficulty lives. The grammar itself is small and uniform; the
tokenization is context-dependent in three places, and most parser tools
assume those three places don't exist. (Worth noting that production
compilers — clang, rustc, GCC's C frontend — are all hand-rolled
recursive descent for similar reasons: the grammar is small relative to
the surrounding semantic machinery, and the tricky bits live outside
what a generator can express.)

### The lexer is the hard part

Three K tokenization rules can't be expressed by a context-free regex:

- **`-` sign vs subtract.** In `1-2`, the `-` is the dyadic subtract
  verb; in `1 -2 3` it's a sign on the second integer. The disambiguator
  is "did the previous token leave us expecting a noun?" — the scanner
  tracks this with a single `noun_pos` flag and decides in one forward
  pass, no backtracking.
- **Monadic vs dyadic verb.** A bare `+` is monadic or dyadic depending
  on whether a noun preceded it. `+1` becomes `(+:; 1)`; `2+1` becomes
  `(+; 2; 1)`. Same token, different role, decided by left context. The
  parser threads this as the `Role` return value through each layer.
- **Name vs symbol literal.** `x` is a name (sym atom), but `` `x `` is
  a symbol literal (one-element sym vector). Same K type code, atom vs
  vector — the printer prints the latter with a leading `,` ("enlist")
  to disambiguate.

These three rules are why this implementation splits scanning and
parsing into two passes: the scanner resolves all the context-sensitive
classification, hands the parser clean tokens, and the parser sees an
unambiguous stream. Any tool you pick has to confront these — usually
by letting you hook in a custom lexer, which is mostly what the
hand-rolled scanner already is.

### Tool by tool

**Hand-rolled recursive descent (what you're reading).** For a
5-production grammar, this *is* the right tool. Each parser function
maps 1:1 to a production rule, and the file you read top-to-bottom is
the language. Anything else is layering machinery on something small
enough to fit on screen.

**ANTLR (ALL(\*), Java toolchain).** The most plausible "real tool"
choice. Adaptive LL handles `e : nve | te` cleanly — and it has to be
*adaptive*: choosing `nve` vs `te` means looking past a whole term to
see whether a trailing adverb makes it a verb, which no fixed-token
LL(1) can do (it's exactly the case the hand-rolled parser settles with
one-term lookahead). Semantic predicates can express the
context-sensitive role of `-` and the monadic/dyadic verb decision. The
ecosystem is the strongest in the space — railroad-diagram output, IDE
plugins, runtimes for Java, Python, Go, C++, JavaScript, C#. The `.g4`
reads almost verbatim like the BNF on the wiki page, which is appealing
if you want the grammar itself to be the spec. The cost: heavy generated
code (thousands of lines per language target), a JVM at build time, and
the `.g4` will end up carrying predicates that essentially re-encode the
`noun_pos` flag — so you don't escape the trickiest bit, you just push
it into a different file.

**pest (PEG, Rust).** Pure Rust, clean grammar syntax in a `.pest`
file, ordered choice neatly handles `nve | te` (try `nve` first,
backtracking to `te` if the second term turns out not to be a verb) —
its backtracking even gets the derived-verb case right for free, the one
a naive single-token recursive descent has to special-case. Compiles to
a typed parse tree that you walk to build `K`. The cost: PEG is
whitespace-explicit — you write `~` for "and then", not implicit space —
so K's whitespace-sensitive vector rule (`1 -2 3` is one vector, `1-2 3`
is two tokens) makes the grammar busier than it looks. PEGs also assume
a context-free tokenizer, so the sign-vs-subtract rule has to be encoded
as alternative productions guarded by lookahead, which is uglier than
the one-line `noun_pos` check in the hand-rolled scanner.

**Packrat (memoized PEG).** Same syntactic family as pest, with the
addition that every `(rule, position)` parse result is memoized — so
backtracking is linear-time rather than potentially exponential. The
guarantee matters when grammars have deep backtracking (Bryan Ford's
original Pappy generator — itself written in Haskell — used it to parse
Java in linear time, without worst-case blowup). For K, the grammar is
unambiguous and shallow
once the lexer has done its job; recursive descent already runs in
linear time without memoization, and the memo table is pure memory
overhead. Janet's PEG and some PEG.js variants do packrat; pest
itself doesn't memoize by default but accepts the same critique.

**nom / chumsky (parser combinators, Rust).** Same expressive shape as
recursive descent, just structured as composable functions returning
parser results. You'd end up with something topologically identical
to the code in this repo, with a heavier type system overhead. nom is
older and more general (it's also the standard choice for binary
formats); chumsky is newer with standout error reporting — labelled
spans, recovery, "expected X, found Y" diagnostics. The error story
is the only real win, and matters more for a production language than
for a pedagogical reference.

**LALRPOP / yacc / bison (LR(1)/LALR(1)).** Awkward fit. LALRPOP
defaults to LR(1) (yacc and bison are LALR(1)), but they're the same
family for this discussion: LR-family tools assume the lexer hands
them clean, context-free tokens, and K's lexer
is the hard part. You'd plumb a custom stateful lexer in (matching
what the current scanner already does), then write a `.lalrpop`/`.y`
grammar on top of it. Two layers for no payoff. The grammar itself,
once tokens are disambiguated, is LR-friendly — but you've moved all
the interesting work to the lexer hook, and the generated parser is
just a state machine for `nve | te | empty`.

**Earley (Marpa).** Earley parses any context-free
grammar, including ambiguous ones, in O(n³) worst case but O(n) for
unambiguous LR(k)-class grammars like K's. The selling point is
"handles anything" — invaluable when grammars are genuinely
ambiguous or evolving (natural language, legacy SQL dialects,
recovering ill-defined historical languages). For K, that's exactly
the wrong tradeoff. Once the lexer disambiguates the three
context-sensitive cases above, the grammar is unambiguous, and
Earley's worst-case headroom buys you nothing. You'd carry
table-driven dynamic-state-set overhead per token just to recover
the linear-time behavior recursive descent already has. Earley
shines when the grammar is the hard part — here, the grammar fits
on the back of a napkin and the lexer doesn't.

**tree-sitter (C, GLR).** Designed for editor tooling: incremental
reparsing on every keystroke, error tolerance, parse trees usable for
syntax highlighting and refactoring. GLR can handle the grammar's
ambiguity directly, and tree-sitter has "external scanners" written
in C for exactly the context-sensitive lexer cases K has. The catch:
tree-sitter targets editor integration, not language implementation
— it produces parse trees, not your own AST shape, so you'd still
walk it to build `K`. Worth pulling in if you wanted K syntax
highlighting in VS Code or Neovim, but overkill as a parser layer.

**A Pratt parser.** Frequently suggested whenever someone says
"parsing expressions," and a poor fit here. Pratt's whole reason to
exist is **operator precedence**: encoding 10–20 binding-power levels
(`*` binds tighter than `+` binds tighter than `==` ...) cleanly via
per-token nud/led functions and binding-power numbers. K has **zero
precedence levels** — every dyadic verb has the same strength and
the grammar is uniformly right-associative. The rule `e : nve`
literally says "parse a noun, a verb, then an expr on the right,"
and direct recursion gets that for free. Pratt's machinery would
degenerate to "every infix token has bp=10, right-bp=9" — using a
dispatch table to do what a plain recursive call already does.
Worse, Pratt assumes a token's role is fixed at its dispatch entry;
here, `+` is monadic or dyadic depending on what preceded it (and a
term's role isn't fixed either — `f` is a noun, `` f' `` a verb), so
you'd be special-casing every verb's nud-vs-led decision anyway.
Pratt amortizes over many operators with varying precedences and
associativities; with five productions and one associativity rule,
it's pure overhead.

### Bottom line

For pedagogy, this hand-rolled implementation is the artifact: the
parser functions *are* the grammar, and you can read them in one
sitting.

If the goal shifted to a single document that doubled as a formal
spec, an ANTLR `.g4` paired with this reference would be the most
legible combination — the grammar file reads almost verbatim like the
BNF on the wiki page, and the C source shows what the predicates
actually compute. For high-quality parse errors in a Rust
re-implementation, chumsky is the best library to reach for, but only
worth pulling in if you start caring about parse-error UX. For editor
integration, tree-sitter is the obvious choice and is orthogonal to
whatever your "real" parser does.

The general principle: parser generators amortize their scaffolding
across a large grammar. K's grammar is too small for the amortization
to pay off, and its lexer is the part with nearly all the irreducible
complexity — the one structural parsing wrinkle is the `nve`-vs-`te`
decision, which needs a term of lookahead rather than a token (see
"Choosing nve vs te by term role" above). Reaching for a tool moves the
complexity rather than removing it.

## Helpful links

- <https://www.iro.umontreal.ca/~felipe/IFT2030-Automne2002/Complements/tinyc.c>
- <https://www.craftinginterpreters.com/parsing-expressions.html#recursive-descent-parsing>
- <https://github.com/kparc/ksimple>
- <https://ref.kparc.io/> (kparc/shakti K reference; documents select/update as `#[t;c;b[;a]]` / `_[t;c;b[;a]]`)
- <http://nsl.com/k/ksql.k> (Stevan Apter's ksql.k)
- <https://llvm.org/docs/tutorial/>
- <https://norvig.com/lispy.html>

## License

Apache License 2.0. See `NOTICE` for attributions.

The K value layout in `kparser.c` is adapted from KX Systems' C header
[`k.h`](https://github.com/KxSystems/kdb/blob/master/c/c/k.h), which is
licensed under the Apache License 2.0. This project is unaffiliated with
and not endorsed by KX; KX and its product names are trademarks of their
owner.
