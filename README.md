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

## Helpful links

<https://www.iro.umontreal.ca/~felipe/IFT2030-Automne2002/Complements/tinyc.c>
<https://www.craftinginterpreters.com/parsing-expressions.html#recursive-descent-parsing>
<https://github.com/kparc/ksimple>
<https://llvm.org/docs/tutorial/>
<https://norvig.com/lispy.html>

## License

Apache License 2.0.
