# kparser

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

**ANTLR (LL(\*), Java toolchain).** The most plausible "real tool"
choice. Adaptive LL handles `e : nve | te` cleanly, and semantic
predicates can express the context-sensitive role of `-` and the
monadic/dyadic verb decision. The ecosystem is the strongest in the
space — railroad-diagram output, IDE plugins, runtimes for Java,
Python, Go, C++, JavaScript, C#. The `.g4` reads almost verbatim like
the BNF on the wiki page, which is appealing if you want the grammar
itself to be the spec. The cost: heavy generated code (thousands of
lines per language target), a JVM at build time, and the `.g4` will
end up carrying predicates that essentially re-encode the `noun_pos`
flag — so you don't escape the trickiest bit, you just push it into
a different file.

**pest (PEG, Rust).** Pure Rust, clean grammar syntax in a `.pest`
file, ordered choice neatly handles `nve | te` (try `nve` first via
lookahead). Compiles to a typed parse tree that you walk to build
`K`. The cost: PEG is whitespace-explicit — you write `~` for "and
then", not implicit space — so K's whitespace-sensitive vector rule
(`1 -2 3` is one vector, `1-2 3` is two tokens) makes the grammar
busier than it looks. PEGs also assume a context-free tokenizer, so
the sign-vs-subtract rule has to be encoded as alternative productions
guarded by lookahead, which is uglier than the one-line `noun_pos`
check in the hand-rolled scanner.

**Packrat (memoized PEG).** Same syntactic family as pest, with the
addition that every `(rule, position)` parse result is memoized — so
backtracking is linear-time rather than potentially exponential. The
guarantee matters when grammars have deep backtracking (Bryan Ford's
original Pappy paper used it to parse a Haskell-like language without
worst-case blowup). For K, the grammar is unambiguous and shallow
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

**LALRPOP / yacc / bison (LALR(1)).** Awkward fit. LALR-family tools
assume the lexer hands them clean, context-free tokens, and K's lexer
is the hard part. You'd plumb a custom stateful lexer in (matching
what the current scanner already does), then write a `.lalrpop`/`.y`
grammar on top of it. Two layers for no payoff. The grammar itself,
once tokens are disambiguated, is LR-friendly — but you've moved all
the interesting work to the lexer hook, and the generated parser is
just a state machine for `nve | te | empty`.

**Earley (Marpa, bison's GLR mode).** Earley parses any context-free
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
here, `+` is monadic or dyadic depending on what preceded it, so
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
to pay off, and its lexer is the part with all the irreducible
complexity. Reaching for a tool moves the complexity rather than
removing it.

## Helpful links

- <https://www.iro.umontreal.ca/~felipe/IFT2030-Automne2002/Complements/tinyc.c>
- <https://www.craftinginterpreters.com/parsing-expressions.html#recursive-descent-parsing>
- <https://github.com/kparc/ksimple>
- <https://llvm.org/docs/tutorial/>
- <https://norvig.com/lispy.html>

## License

Apache License 2.0.
