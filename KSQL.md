# ksql — Step 2

A follow-on to [`README.md`](README.md). Step 1 (`kparser.c`) parses the
core K grammar. Step 2 (`ksqlparser.c`) adds **ksql**, K's SQL dialect:

```
select [phrases] [by phrases] from e [where phrases]
exec   [phrases] [by phrases] from e [where phrases]
update  phrases  [by phrases] from e [where phrases]
delete [phrases]              from e [where phrases]
```

`ksqlparser.c` is `kparser.c` plus a ~120-line ksql layer. The point of
keeping it as a separate file is that `diff kparser.c ksqlparser.c` *is*
the lesson — you can see exactly what the feature costs.

```sh
make ksqlparser   # build it
make test2        # STEP 1 golden suite (proves superset) + ksql cases
./ksqlparser
```

```
  select a,b by c from t where d>0
(`select;`t;((>;`d;0));(`c);(`a;`b))
```

## The one idea: a query is sugar for a function application

The whole design rests on a single observation, taken from Stevan Apter's
[`ksql.k`](http://nsl.com/k/ksql.k). Its operators all share one
signature:

```k
/ ?/![t;c;b;a]
select:{[t;c;b;a] ... }
update:{[t;c;b;a] ... }
delete:{[t;c;b;a] ... }
exec:{[t;c;b;a] ... }
```

So every query is, underneath, a four-argument application of a verb to
`[t;c;b;a]`:

| slot | meaning      | comes from |
| ---- | ------------ | ---------- |
| `t`  | table        | `from`     |
| `c`  | constraints  | `where`    |
| `b`  | grouping     | `by`       |
| `a`  | phrases      | the select-list |

The same `[t;c;b;a]` order turns up in the kparc/shakti K reference, where
select and update are spelled `#[t;c;b[;a]]` and `_[t;c;b[;a]]`
([ref.kparc.io](https://ref.kparc.io/#select-t-c-b-a)) — independent
confirmation that the argument order is an established K convention, not an
arbitrary choice here.

That shape — head verb, then arguments — is exactly the `t[E]` application
this parser already builds. `select[t;c;b;a]` is the same tree as
`f[1;2;3;4]`:

```
f[1;2]            ->  (`f; 1; 2)
select[t;c;b;a]   ->  (`select; t; c; b; a)
```

So ksql adds **no new AST shape**. A query desugars into the application
tree the printer already knows how to render, with a name sym
(`` `select ``/`` `exec ``/`` `update ``/`` `delete ``) as the head.

## A query is a noun base

Because a query *returns a value* (a table), it is a noun — and its
desugared tree literally lives under the `n` production. So `q` joins the
other bracketed noun bases, the only difference being that its brackets
are keywords instead of punctuation:

```
E : E ; e | e
e : nve | te | empty
t : n | v
v : tA | V
n : t[E] | (E) | {E} | N | q        // q is a fourth noun base
q : `select P B from e W            // also exec / update / delete
B : by L | empty
W : where L | empty
P : L | empty
L : L , e | e                       // comma-separated phrases
```

Being an `n` is what makes queries *compose* and *nest* for free — they
can be operands, arguments, and subexpressions, exactly like `(E)` and
`{E}`:

```
  count select a from t
(`count;(`select;`t;();();(`a)))

  select a from (select b from t) where d
(`select;(`select;`t;();();(`b));(`d);();(`a))
```

If `q` lived at the `e` level instead, none of that nesting would parse.

## Aliases are free

`[alias:]expr` is *syntactically identical* to the `:` assignment Step 1
already parses. `qty:sum amt` parses through the unchanged `parse_e` as the
`nve` shape `(:;`qty;(sum;`amt))` — so a phrase list needs zero special
handling for aliasing. This is the "alias = assignment, emergent not
grammatical" idea, and it's not just rhetoric; it's literally what the
existing code produces:

```
  update qty:sum amt from t
(`update;`t;();();((:;`qty;(`sum;`amt))))
```

## The hard part: the comma

`,` is the join verb. But between phrases in a clause it *separates* —
`select a,b from t` is two columns, while `select (a,b) from t` joins them.
Two refinements pin down exactly when a comma is a separator:

- it must be the **dyadic** join `,` (a bare comma). The monadic enlist
  `,:` is an ordinary verb and never separates.
- it must be at top level **of a clause that is a list** — the select, by,
  and where phrases. The `from` table is a single expression, so a comma
  there is join: `select a from t,u` reads `t,u` as one joined table.

The parser carries this as an explicit **parse context** threaded down the
expression parser — a `QCtx` argument, not mutable parser state:

- `Q_NONE` — ordinary K (all of Step 1): nothing terminates an expr early.
- `Q_FROM` — the `from` table: stop at a clause keyword; `,` still joins.
- `Q_PHRASE` — a select/by/where phrase: stop at a clause keyword **and**
  at a top-level dyadic `,` (the phrase separator).

A thin wrapper `parse_base_q` consults the context: it ends the current
expression (returns `EMPTY` without consuming) at a boundary, so `parse_e`
halts there and the clause loop steps over the separator. The Step-1 core
`parse_base` is left **byte-for-byte unchanged** — all query knowledge
lives in the wrapper, so `diff kparser.c ksqlparser.c` keeps the base
parser clean.

Brackets need no special handling. Every `()`/`[]`/`{}` recurses through
`parse_E`, which always parses at `Q_NONE`, so inside brackets everything
is ordinary again — which is why `select (a,b) from t` joins. The context
is reset *structurally* by the grammar's own recursion, with no
save/clear/restore bookkeeping.

```
  select a,b from t      ->  (`select;`t;();();(`a;`b))      / two phrases
  select (a,b) from t    ->  (`select;`t;();();((,;`a;`b)))  / one join (parens)
  select a from t,u      ->  (`select;(,;`t;`u);();();(`a))  / joined from-table
  select ,:a from t      ->  (`select;`t;();();((,:;`a)))    / ,: is a verb
```

A non-`Q_NONE` context only ever arises inside a query, so Step 1 behavior
is untouched.

## What was already free

Beyond aliases, a lot of ksql needed no work because it was already
ordinary K:

- **Joins and helpers** — `t lj u`, `` `k key u``, `(max;weight) fby city`
  are just names/verbs applied to arguments. They parse today.
- **The printer** — a sym head prints as `` `select ``, and an empty list
  already prints as `()`, so empty `where`/`by`/`select` clauses render
  for free.
- **The scanner** — `select`, `from`, `by`, `where` all scan as ordinary
  `-KS` name tokens. No new token kind.

The genuinely new code is just `parse_query`, `parse_phrase_list`, and the
`parse_base_q` wrapper that holds the context (`QCtx`) handling and the
query dispatch.

## AST shapes

The slots are always `(verb; t; c; b; a)` — from, where, by, phrases.
Absent clauses are the empty list `()`. (Single-element clauses print as a
one-element generic list, e.g. `(`a)`, not with the `,` enlist marker,
which is only for sym vectors.)

| Source | AST |
| ------ | --- |
| `select from t` | `` (`select;`t;();();()) `` |
| `select a,b from t` | `` (`select;`t;();();(`a;`b)) `` |
| `select a by c from t` | `` (`select;`t;();(`c);(`a)) `` |
| `select a from t where d>0` | `` (`select;`t;((>;`d;0));();(`a)) `` |
| `select p:sum i,q:avg j by f,g from t where f=c,h>5` | `` (`select;`t;((=;`f;`c);(>;`h;5));(`f;`g);((:;`p;(`sum;`i));(:;`q;(`avg;`j)))) `` |
| `update a:x+1 from t` | `` (`update;`t;();();((:;`a;(+;`x;1)))) `` |
| `exec a from t` | `` (`exec;`t;();();(`a)) `` |
| `delete from t where d` | `` (`delete;`t;(`d);();()) `` |
| `delete a,b from t` | `` (`delete;`t;();();(`a;`b)) `` |

`delete`'s two jobs — drop rows (`delete from t where …`) versus drop
columns (`delete a,b from t`) — are the *same* tree distinguished by
whether `c` or `a` is non-empty. That's an evaluation-time reading, not a
grammar difference; the parser just gets the shape right and stops, the
same way it leaves projection-vs-application to a valence-aware evaluator.

## Design notes

### Keywords are reserved only positionally

`select`/`exec`/`update`/`delete` are treated as query verbs whenever they
appear as a base term — so you can't use them as variable names. But
`from`/`by`/`where` are recognized as clause keywords *only inside a query*
(when the parse context is not `Q_NONE`); everywhere else they remain
ordinary names:

```
  from:3
(:;`from;3)
  where+1
(+;`where;1)
```

This keeps Step 2 a strict superset of Step 1 except for the four query
verbs — which is also why `make test2` re-runs the entire Step 1 golden
suite against `ksqlparser`.

### A clause keyword must carry its operand

The parser checks *structural* well-formedness only: a clause keyword that
is written has to have its operand. `from` needs a table; `by` and `where`,
if present, need a non-empty list. So these all abort via the usual
`die()` path, like any other malformed input:

```
  select a                  / no from
kparser: ksql: expected 'from'
  select x,y from           / from, but no table
kparser: ksql: expected table after 'from'
  select x by from t        / by, but no columns
kparser: ksql: empty 'by'
  select x from t where     / where, but no conditions
kparser: ksql: empty 'where'
```

The select/phrase list itself may be empty — `select from t` is a valid
"all columns". And an elided *phrase* (e.g. `select x, from t`) still
becomes the generic null `::`, mirroring how the core parser fills a
projection hole (`f[1;;3]`, `2+`). Anything beyond shape — does the table
exist, is the column real — is the evaluator's job, not the parser's.

### Clauses must appear in order

A query is a complete noun, and an unparenthesized one is greedy, so the
only thing that may follow its clauses is an expression terminator — end of
input, `;`, or a closing `)`/`]`/`}`. `parse_query` checks this at the end.
Without it, a clause given out of order leaks past the query and the outer
grammar silently reabsorbs it as juxtaposition. For example `by` after
`from` would otherwise parse as the query *applied to* `(by …)`:

```
  select a,b from t,u by a=2   / `by` belongs before `from`
kparser: ksql: unexpected token after query
```

Together with `expected 'from'`, this pins the clause order from both ends:
`from` must appear where expected, and nothing may dangle after the query.

### What's deferred

`insert` and `upsert` use a different shape (`[t;d]`), not `[t;c;b;a]`, so
they're left out to keep Step 2 focused. The joins (`lj`/`uj`/`pj`/…),
aggregators (`sum`/`avg`/…), and `fby`/`key` already parse as
ordinary verbs and names, so they need nothing from the grammar.

## Code layout

The Step 2 additions are tagged `STEP2` or grouped under the
`===== STEP 2: ksql =====` banner in `ksqlparser.c`:

- a `QCtx` parse context threaded through `parse_e` / `parse_e_from` /
  `parse_term` (brackets reset it for free via `parse_E`, which always
  parses at `Q_NONE`);
- `parse_base_q`, a thin query-aware wrapper around the unchanged Step-1
  `parse_base`, holding the query dispatch and the terminator checks;
- `parse_query` (collects the clauses, emits `(verb; t; c; b; a)`) and
  `parse_phrase_list` (a comma-separated run of `e`s);
- four small helpers: `sym_is`, `is_comma`, and the positional keyword
  predicates `is_query_verb` / `is_clause_kw`.
