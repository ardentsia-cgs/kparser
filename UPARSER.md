# uparser — Step 5: the universal parser

A follow-on to [QPARSER.md](QPARSER.md). Step 4 (`qparser.c`) demonstrates
that naming the monadic verbs removes parser logic.  Step 5 (`uparser.c`)
asks: if K and q differ in only two places, can they share the same parser
and switch at runtime?  The answer is yes, and the switch is ~15 lines.

```
make uparser
./uparser         # starts in q mode (q> prompt)
\                 # toggle to K mode (k> prompt)
k)+1              # parse one line in K mode without switching
q)flip 1          # parse one line in q mode without switching
```

## The one idea: K and q are the same parser

K and q disagree in exactly two decisions:

| Decision | K | q |
|---|---|---|
| Is a keyword name (`flip`, `lj`) a verb? | No — it's an ordinary name (`-KS` noun) | Yes — a named monadic (`KV1`) or dyad (`KV2`) |
| `+1` (bare glyph, monadic position)? | Demote `KV2` → `KV1`: `(+:;1)` | Hard error: glyphs are always dyadic |

Everything else — the nve/te machinery, adverbs, brackets, lambdas, ksql
queries, table literals, ref-counting, K types — is identical.  So the
parser doesn't need two implementations; it needs a mode flag consulted at
three points: the scanner (keyword lookup on or off), the demotion branch
(demote or reject), and the printer (render a verb by name or by glyph).
The first two are the two decisions above; the third is the rendering
consequence of the first (a KV1/KV2 node has to print differently
depending on how it was tagged).

## The three gates

### Gate 1: scanner

In the `CL_ALPHA` branch of `scan()`, the keyword tables (`MONADIC_NAMES`,
`DYADIC_NAMES`) are consulted only in q mode.  In K mode the lookup is
skipped; every alphanumeric token is a noun, as in `kparser.c`.

```c
if (mode == M_Q) {
    int midx = monadic_keyword(buf);
    if (midx >= 0) { EMIT(T_VERB, kverb(1, midx)); ... }
    else { int didx = dyadic_keyword(buf); ... }
} else {
    EMIT(T_NOUN, ks(buf));   // K mode: everything is a name
}
```

This is the heavy lifter.  Once tokens are tagged, the parser doesn't care
how they got their types.

### Gate 2: demotion

In `parse_e_from()`, a bare dyadic glyph in monadic position:

```c
if (t.role == R_VERB && t.v && t.v->t == KV2) {
    if (parser_mode == M_Q)
        die("q: dyadic verb in monadic position; use the named monadic");
    K old = t.v;
    t.v = kverb(1, old->i);   // K demotion
    dec_ref(old);
}
```

In K mode, `+1` → `(+:;1)`.  In q mode, `+1` is an error — the user must
write `flip 1`.  Named monadics arrive as `KV1` from the scanner and never
hit this branch.

### Gate 3: printer

The printer renders `KV1` and `KV2` nodes by name only in q mode:

```c
case KV1:
    if (parser_mode == M_Q && x->i - 1 < (int)NMONADICS)
        printf("%s", MONADIC_NAMES[x->i - 1]);
    else { putchar(VERB_CHARS[x->i]); putchar(':'); }
    break;
case KV2:
    if (x->i < (int)NVERBS) putchar(VERB_CHARS[x->i]);
    else if (parser_mode == M_Q) printf("%s", DYADIC_NAMES[x->i - (int)NVERBS]);
    break;
```

In K mode, `(+:;1)` prints as `(+:;1)`.  In q mode, `(flip;1)` prints as
`(flip;1)`, and a named dyad like `lj` prints as `lj` rather than a glyph.
Same node type, same index — the printer resolves the rendering from the
mode.

## Mode control

Default is q.  Three mechanisms:

| Input | Effect |
|---|---|
| `\` (solo backslash) | Toggle between K and q mode |
| `k)expr` | Parse `expr` in K mode (one-shot, doesn't change default) |
| `q)expr` | Parse `expr` in q mode (one-shot, doesn't change default) |

The `k)`/`q)` prefixes are designed for golden tests — no need to send a
separate toggle line.  The REPL prompt shows `k> ` or `q> ` when
interactive, plain `  ` when piped (so the test runner can strip it).

## Test suite

```
make test5
```

Three suites, one binary:

| Suite | Mode | Cases | Result |
|---|---|---|---|
| `tests/q_cases.tsv` | q (default) | 80 | 80 pass |
| `tests/cases.tsv` | K (`k)` prefix) | 102 | 102 pass |
| `tests/ksql_cases.tsv` | K (`k)` prefix) | 45 | 45 pass |

The K-mode passes are the proof: the mode switch is a strict superset of
`kparser.c` and `ksqlparser.c`.  Send `k)` before each line and the
universal parser produces byte-for-byte identical output to the Step 1 and
Step 2 binaries.

## Code layout

`uparser.c` is `qparser.c` plus the mode plumbing.  The additions:

- `Mode` enum (`M_K`, `M_Q`) and a module-level `parser_mode` variable
- Scanner gate in `scan()` (the `CL_ALPHA` branch)
- Demotion gate in `parse_e_from()` (the te branch)
- Printer gate in `print_k()` (KV1 and KV2 cases)
- Mode toggle / prefix handling in `main()`

Everything else — the K struct, ref-counting, verb tables, keyword tables,
the parser productions, the ksql layer, table literals, lambda parameter
lists — is shared.

## Why this belongs in the progression

The five steps form a clean narrative about grammar:

| Step | Lesson |
|---|---|
| 1 (`kparser.c`) | K's five productions, arity inferred by position |
| 2 (`ksqlparser.c`) | Queries are a fourth noun base, desugaring to `t[E]` |
| 3 (`sqlparser.c`) | SQL and ksql are two surfaces over one AST |
| 4 (`qparser.c`) | Naming monadics removes parser logic |
| 5 (`uparser.c`) | K and q differ in exactly two places; the rest is one parser |

Step 5 closes the circle.  Step 1 infers arity from position.  Step 4
removes that inference by naming verbs.  Step 5 shows that both
approaches coexist in a single codebase, gated by a single mode flag read
at three points (scanner, demotion, printer).  The two aren't different
parsers — they're two answers to the same question.
