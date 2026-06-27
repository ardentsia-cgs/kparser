# Appendix: ksimple's implicit grammar

A side note to [`README.md`](README.md). Where `kparser` writes the K
grammar down as explicit productions and one parser function per rule,
Arthur Whitney's [`ksimple`](https://github.com/kparc/ksimple) (MIT, a
~100-line learning interpreter) takes the opposite tack: it has **no
parser**. Tokenizing, parsing, and evaluation are fused into a single
recursive function, `e()`, that walks a tape of **one-byte tokens**
right-to-left.

So the grammar is never stated — it lives in the control flow of `e()`.
This is an exercise in reading it back out.

## The extracted grammar

```
e : N            // a lone noun
  | V e          // monadic verb application
  | V A e        // adverb-derived verb (over / scan)
  | N V e        // dyadic verb application
  | N ':' e      // assignment (N must be a name)

N : 0..9 | a..z          // digit literal, or global variable
V : + - ! # , @ = ~ & | *
A : / \                  // over, scan
```

That is the whole language: five productions. Two are the monadic side
(`V e`, `V A e`), two are the "noun, then the rest" side (`N V e`,
`N ':' e`), and `N` is the base case that ends the recursion.

## Mapping each production to `e()`

With the macros expanded, `e(s)` reads the first token `i`, points `t` at
the rest of the tape, and branches:

```
i = s[0];  t = s+1;

!*t        -> n(i)                      // (e : N)      last token, must be a noun
v(i)?                                    // i is a verb:
  d(*t)    -> D[d(*t)](v(i), e(t+1))    // (e : V A e)  verb + adverb; eval past the adverb
  else     -> f[v(i)](e(t))            // (e : V e)    monadic verb on eval of the rest
: // i is a noun:
  y = e(t+1)                           //              eval everything past the 2nd token
  *t==':'  -> ag(i, y)                 // (e : N ':' e) assign y to the name i
  else     -> F[v(*t)](n(i), y)        // (e : N V e)  dyadic verb between n(i) and y
```

The first token decides the shape: noun or verb, and if a noun, whether
the token after it is `:` (assign) or a verb (dyadic). Whatever follows
the operator is one recursive `e`.

## Token classes

These come straight from the dispatch tables in `a.c`:

```
V = " +-!#,@=~&|*"
f[] (monadic): _  -  -   !   #   ,   @     _  _  _  |    _
                  neg  til cnt enl first         rev
F[] (dyadic):  _  +  -   !   #   ,   @   =   ~   &   |   *
                 add sub mod take cat index eq  neq and or  mul
AV = " /\"  ->  / = over,  \ = scan
```

A few monadic slots are `foo` (not-yet-implemented), e.g. monadic `+`,
`=`, `~`, `&`, `*`. Nouns are a single digit (`0..9`, a literal) or a
single lowercase letter (`a..z`, a global variable).

## The two ideas that make it K

Almost everything is stripped away, but two properties survive — and they
are exactly the ones that make K *feel* like K:

- **Right-to-left, no precedence.** A dyadic verb takes the *entire*
  expression to its right as its right operand (`N V e` recurses on the
  whole tail). There are no precedence levels to remember.
- **Arity is chosen by left context.** Whether a verb is monadic or
  dyadic is settled by one question: is there a noun to its left? That is
  the same decision `kparser` makes with its `nve`-vs-`te` split —
  resolved here positionally on a flat tape instead of by a term's role.

A smaller third point: **assignment is its own production**, not a verb.
`ksimple` special-cases `:` because its left side must be an *unevaluated*
name (`a`), not a value, so it can't go through the dyadic path. (In
`kparser`, `:` is just a verb in `V`, and assignment falls out of `nve`.)

## How it relates to the full grammar

Against the five-rule reference grammar `kparser` targets:

```
E : E ; e | e
e : nve | te | empty
t : n | v
v : tA | V
n : t[E] | (E) | {E} | N
```

`ksimple` is a deliberate subset:

| feature                  | full grammar         | ksimple                      |
| ------------------------ | -------------------- | ---------------------------- |
| dyadic `n v e`           | `nve`                | yes — `N V e`                |
| monadic verb             | `te` (verb head)     | yes — `V e`                  |
| adverbs                  | `tA` (any term)      | only `V A e` (leading verb)  |
| application / index `t[E]` | yes                | no                           |
| grouping `(E)`, lambda `{E}` | yes              | no                           |
| `;`-sequencing           | `E : E ; e`          | no                           |
| assignment               | emergent (`:` is a `V`) | explicit `N ':' e`        |
| nouns                    | names, ints, syms, vectors | digits + `a..z` only   |

So `ksimple` keeps the two load-bearing K ideas and drops everything
structural — parens, brackets, lambdas, sequencing, multi-character
tokens. It is the grammar boiled down until only the part that makes K K
is left. `kparser` then adds back the structure (`(E)`, `{E}`, `t[E]`,
`;`), and [`KSQL.md`](KSQL.md) adds the query layer on top of that.
