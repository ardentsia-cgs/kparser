# sql via libpg_query — Step 3b

A follow-on to [`README.md`](README.md), [`KSQL.md`](KSQL.md), and
[`SQL.md`](SQL.md). Step 3 (`sqlparser.c`) hand-writes a **standard-SQL query
front-end** — a scanner, a precedence-climbing expression parser, a mode switch
— and shows it can target the *same* `(verb; t; c; b; a)` AST as ksql.

Step 3b asks the complementary question: **what if we don't write the parser at
all?** Instead of scanning and grouping SQL ourselves, we hand the text to
[`libpg_query`](https://github.com/pganalyze/libpg_query) — the *actual
PostgreSQL parser*, extracted into a standalone C library — and then **lower**
its parse tree into the very same AST.

```
  SELECT * FROM t
(`select;`t;();();())
```

is still the exact tree ksql produces for `select from t`. The difference is
where the tree comes from: Step 3 *parsed* it, Step 3b *translated* it from
Postgres's own tree. The Step 3b code lives in [`pglower.c`](pglower.c) (the K
value core and `emit_query` reused verbatim from `sqlparser.c`, plus a lowering
pass); build it with `make pglower` and test it with `make test3b`.

```sh
make pglower       # build it (needs libpg_query + protobuf-c; see below)
make test3b        # lower the STEP 3 sql cases from PG's own parse tree
./pglower          # a REPL: type SQL, see the K AST
```

> **This is a branch, not a successor.** Step 3b does not replace Step 3. It is
> the mirror image of it, and the two are most instructive side by side. Step 3
> owns the *front-end* (text → AST) and teaches scanning and precedence; Step 3b
> owns a *back-end* (PG tree → AST) and teaches lowering. Neither is "more
> finished" than the other.

## Why this belongs in the project

The [`README.md`](README.md) essay "Why hand-rolled?" argues that for a small
grammar the interesting difficulty lives in the **lexer**, and that reaching for
a heavyweight tool *moves* complexity rather than removing it. `libpg_query` is
the concrete, honest test of that claim. It is the extreme end of "use the real
thing": you outsource lexing, precedence, and total grammar coverage to
PostgreSQL, and then you get to see exactly what is *left over*.

What is left over turns out to be small and clean: a tree-to-tree lowering whose
rules are almost entirely already named in [`SQL.md`](SQL.md). That is the
lesson of Step 3b — not "parsing is easy if you cheat", but "once someone else
pays the parsing cost, the shape gap between an industrial parse tree and this
project's compact AST is legible and shallow".

## What `libpg_query` is (and is not)

- It is **not** a parser generator or a grammar you author. It is Postgres's own
  `gram.y` + scanner, compiled outside the server.
- It returns the canonical Postgres parse tree in two interchangeable forms: as
  **JSON** (`pg_query_parse`) and as **protobuf** (`pg_query_parse_protobuf`).
  Both carry the same tree; each node is one tagged variant — `SelectStmt`,
  `A_Expr`, `ColumnRef`, `A_Const`, `FuncCall`, `BoolExpr`, … — with source
  `location` offsets and the node's fields.
- `pglower.c` uses the **protobuf** form. See
  [Why protobuf, not JSON](#why-protobuf-not-json) below — briefly, once you
  depend on the heavyweight PG parser, the protobuf-c glue is a small marginal
  cost that buys **typed C structs**, so the lowering reads as straight-line
  field access (`s->from_clause`, `e->lexpr`) rather than a hand-rolled JSON
  walk.

For example, `SELECT 1` returns (JSON form shown for readability; whitespace
added):

```json
{
  "version": 180004,
  "stmts": [
    { "stmt": { "SelectStmt": {
      "targetList": [
        { "ResTarget": { "val": { "A_Const": { "ival": { "ival": 1 },
          "location": 7 } }, "location": 7 } }
      ],
      "limitOption": "LIMIT_OPTION_DEFAULT",
      "op": "SETOP_NONE"
    } } }
  ]
}
```

In protobuf that same `SelectStmt` is a `PgQuery__SelectStmt` C struct whose
`n_target_list` / `target_list` fields hold the `ResTarget` nodes — the shape
`pglower.c`'s `lower_select` reads directly.

The consequence worth internalizing: `libpg_query` does *none* of the work the
"Why hand-rolled?" essay calls hard (context-sensitive lexing, the sign-vs-verb
and monadic-vs-dyadic decisions — none of which SQL even has), and *all* of the
work Step 3 treats as ordinary (precedence, clause completeness). It hands back
a fully-grouped, spec-complete tree — in **its** shape, not ours.

## The one idea: lowering, not parsing

Step 3 is a front-end; Step 3b is a back-end. They meet at the same node.

| | Step 3 (`sqlparser.c`) | Step 3b (`pglower.c`) |
| --- | --- | --- |
| Owns | front-end: SQL text → AST | back-end: PG tree → AST |
| Hard part it teaches | scanning + precedence | tree lowering + the shape gap |
| Precedence | hand-written climbing | **already resolved** by Postgres |
| Where the subset lives | in the parser (what it *accepts*) | in the lowering (what it *maps*) |
| Dependencies | none | large (libpg_query + protobuf-c) |
| Spirit | matches the project's ethos | deliberately breaks it, on purpose |

In Step 3, both `parse_query` (ksql) and `parse_sql_query` (sql) end at the
shared `emit_query(head, t, c, b, a)`. Step 3b adds a third caller of that same
`emit_query`: a `lower_select` that reads the fields of a Postgres `SelectStmt`
and fills the four slots. The node builder does not change. The AST does not
change. Only the source of the four arguments changes.

## What maps slot-for-slot

The Postgres `SelectStmt` fields line up almost exactly with the four slots
[`SQL.md`](SQL.md) already defined — and almost every lowering rule below has a
named counterpart there.

| Postgres node / field | Lowers to | Named in `SQL.md` as |
| --- | --- | --- |
| `fromClause` (single `RangeVar`) | `t` | FROM → `t` |
| `whereClause` | `c` (flatten a top-level `AND`) | "AND is the phrase separator" |
| `groupClause` | `b` | GROUP BY → `b` |
| `targetList` (`ResTarget` list) | `a` | select list → `a` |
| `ResTarget.name` (the `AS` alias) | `(:; \`name; expr)` | "aliases are assignment" |
| `FuncCall` | `(\`sum; \`amt)` application | "function calls are the application shape" |
| `ColumnRef` | sym atom | leaf: name |
| `A_Const` (integer) | `KI` / `-KI` | leaf: int |
| `A_Star` (the `*`) | omit → empty phrase list `()` | "`SELECT *` is `all columns` — i.e. nothing" |
| `A_Expr` `= < > + - *` | the K glyph verb | operator mapping |
| `A_Expr` `/` | K `%` | "SQL `/` is division, which in K is `%`" |
| `A_Expr` `<= >= <>` (`!=` → `<>`) | sym head `` `<= `` etc. | "no two-glyph comparison" |
| `BoolExpr` AND / OR | K `&` / `` `|` `` | operator mapping |
| `BoolExpr` NOT | K `~:` | "NOT maps to K's monadic `~:`" |
| `sortClause` | `` (`asc; c; query) `` / `` (`dsc; …) `` | "wrap the query noun" |
| `limitCount` | `(#; n; query)` (take) | LIMIT wrapper |
| `limitOffset` | `(_; m; query)` (drop) | LIMIT … OFFSET wrapper |
| `distinctClause` | `(?:; query)` (unique) | DISTINCT wrapper |
| `UpdateStmt` (`targetList` = SET, `whereClause`) | `` (`update; t; c; (); a) `` | UPDATE |
| `DeleteStmt` (`whereClause`) | `` (`delete; t; c; (); ()) `` | DELETE |

Two alignments are worth dwelling on, because they are exactly the places Step
3b turns `SQL.md`'s prose into a visible before/after.

### `SELECT *`: presence in the PG tree, absence in ours

Postgres represents `*` as a *real node* — a `ColumnRef` whose field list is a
single `A_Star`. This project represents "all columns" as *absence*: an empty
phrase list `a = ()`. So the lowering rule "a `targetList` that is exactly one
`A_Star` `ColumnRef` becomes `()`" is a single legible line — and it is the same
desugaring `SQL.md` already argues for, only now you can see the fuller tree it
collapses *from*. The two front-ends diverge here (presence vs. absence) and
reconverge on the identical `` (`select;`t;();();()) ``. That divergence-then-
reconvergence is the clearest single demonstration of "two surfaces, one AST".

### Precedence and post-relational clauses arrive already solved

Postgres has already grouped `x - y - z` left-associatively (`A_Expr` nested on
the left), and has already folded `ORDER BY` / `LIMIT` / `DISTINCT` into fields
*on* the `SelectStmt`. So Step 3b needs **no precedence climber at all**, and
re-expressing sort/limit/distinct as the `` `asc ``/`` `dsc ``/`#`/`_`/`?:`
wrappers is a lowering rule rather than parser control flow. This makes
`SQL.md`'s "The hard part: precedence" section legible *by contrast*: Step 3
earns that code; Step 3b shows what you keep when someone else has already paid
for it. The grouping is identical in both — Step 3b just reads it off the tree
instead of building it.

## The payoff: a cross-check against ground truth

Step 3 can only check the SQL surface against *itself* (does `sqlparser` in
`--sql` mode agree with the golden file). Step 3b enables a stronger claim: run
the **same** SQL through both front-ends and assert the ASTs are **byte
identical**.

```
                    SELECT sum(x) AS mysum FROM t GROUP BY w
                     /                                      \
       sqlparser --sql                                    pglower
   (hand-rolled front-end)                        (PostgreSQL + lowering)
                     \                                      /
             (`select;`t;();(`w);((:;`mysum;(`sum;`x))))   ← must be equal
```

Two entirely independent parsers — a ~530-line teaching parser and the actual
PostgreSQL grammar — converging on the same tree is the project's central thesis
validated against ground truth, not against a fixture the author also wrote.
That is a materially stronger statement than Step 3 can make alone.

Concretely, `make test3b` runs `pglower` against the **same**
`tests/sql_cases.tsv` golden file that `sqlparser --sql` is tested against in
`make test3`. Because both are checked against the *same* expected ASTs,
agreement is the convergence proof. Of the 39 cases (including every `@ABORT`
case, which the real PostgreSQL parser rejects for its own reasons), **38 match
exactly**; one is a documented, legitimate divergence (below).

### The one place they legitimately differ: identifier case

`SeLeCt A FROM T` is the single golden case `pglower` does *not* reproduce, and
the reason is instructive rather than a bug. PostgreSQL **case-folds unquoted
identifiers to lowercase** (`A` → `a`, `T` → `t`), so it lowers to
`` (`select;`t;();();(`a)) ``; the hand-rolled `sqlparser` preserves the source
case and yields `` (`select;`T;();();(`A)) ``. This is a real semantic decision
of the SQL standard that the teaching parser skips, so `tests/run_pg.sh` marks
the case `SKIP` with that reason printed, rather than hiding it. It is a small
bonus lesson: reach for the real parser and you inherit *all* of its behaviour,
including the parts a teaching subset quietly omitted.

## Scope: same corner of SQL, chosen differently

Step 3b covers the same teaching subset as Step 3 (`SELECT` / `UPDATE` /
`DELETE`; `FROM` / `WHERE` / `GROUP BY`; `DISTINCT` / `ORDER BY` / `LIMIT`; no
DDL/DCL/TCL). But note *where* the subset now lives.

Postgres will happily parse joins, subqueries, CTEs (`WITH`), set operations,
window functions (`OVER`), `HAVING`, `CASE`, `IN`/`LIKE`/`BETWEEN` — none of
which fit the four-slot `(verb; t; c; b; a)` shape. So in Step 3b the subset is
enforced by the **lowering**, not by the parser: `pglower` walks a complete PG
tree and, on encountering a node it does not model (a `JoinExpr`, a
`WithClause`, a `SubLink`, a second `fromClause` entry), `die()`s with a clear
"unsupported node" message rather than emitting a wrong tree.

This is arguably *cleaner* than Step 3's "only accept what we model" — "parse
everything, lower what we model" is a tidy separation — but the subset does not
disappear. It moves from the front-end to the lowering pass.

## Honest costs

Step 3b is the one place in this project where a heavy dependency is justified,
precisely because its whole lesson is "use the real thing". But the costs are
real and cut against the identity the [`README.md`](README.md) sells:

- **Size and build time.** The README's pitch is "one C file, ~730 lines, read
  it in one sitting", and every step so far keeps `diff` as the lesson.
  `libpg_query` bundles a large slab of real PostgreSQL C and takes minutes to
  build. `pglower.c` itself stays small (~360 lines), but `diff` is no longer
  the whole story — the dependency is.
- **Two new dependencies and a codegen step.** Step 3b needs `libpg_query`,
  `protobuf-c` (runtime + the `protoc-c` compiler), and a build-time codegen
  pass that turns the shipped `pg_query.proto` into `pg_query.pb-c.{c,h}` (a
  ~46,000-line generated file, `.gitignore`d and regenerated by `make pglower`).
  The rest of the project is proudly dependency-free.
- **Version-pinned output.** The tree shape tracks a Postgres major version
  (currently 18; the `version` field reads `180004` above), and node fields
  shift across majors. Reproducibility requires pinning a `libpg_query` git tag,
  which the CI job does (`-b 18-latest`).
- **It teaches translation, not parsing.** Someone who came here to learn
  parsing learns lowering instead. That is a feature *only* when it is framed as
  the complement to Step 3, which is why this file leads with "branch, not
  successor".

## The fundamental tradeoff: coverage vs. extensibility

The build costs above are real but incidental. The *defining* tradeoff of
`pg_query` is deeper, and it decides whether the tool is right for a given job
at all: **you are not adopting "a SQL parser", you are adopting *the PostgreSQL
parser* — `gram.y` plus the real scanner, frozen at a version.** Everything
follows from that.

### What it buys

- **Correctness and coverage, for free.** Every corner of Postgres DQL/DML —
  precedence, `CASE`, window functions, CTEs, `LATERAL`, casts, operator
  classes — parses exactly as the server parses it. There is no teaching
  subset and no "we didn't get to that yet". Step 3b is the evidence: on the
  first attempt, 38 of 39 golden cases matched a completely independent parser.
- **Structure you can lean on.** You receive a fully-grouped, spec-complete
  tree, so the work left to you is a small semantic mapping — the lowering —
  not lexing or precedence.

### What it forecloses

- **You inherit Postgres's semantics whether you want them or not.** The
  case-folding divergence we hit (`A` → `a`) is the friendly version of this.
  Identifier folding, keyword reservations, string-escape rules,
  `standard_conforming_strings` — none are knobs you get to turn. If your
  dialect disagrees with Postgres on any of them, `pg_query` is simply the
  wrong tool, with no override.
- **You cannot add syntax Postgres doesn't already accept — this is a hard
  ceiling, not a soft one.** The grammar is compiled C generated from
  Postgres's `.y` file; there is no hook, callback, or "custom production" API.
  A token sequence that isn't legal PostgreSQL (a K-style
  `select a by b from t`, a bespoke `PIVOT`, a new clause keyword) is rejected
  at parse time and *you never receive a tree to lower*. Your only outs are all
  bad: fork and maintain a patched `gram.y` (re-merged on every Postgres
  release), rewrite your syntax into valid Postgres first (you've now written a
  parser anyway), or give up and hand-roll. Contrast the hand-rolled parsers
  here, where adding syntax is a localized scanner branch plus a parser
  function — which is exactly why Steps 2–5 are cheap `diff`s. With `pg_query`
  there is no `diff`: extension means forking upstream.

### The escape hatch: customize the *lowering*, not the *grammar*

The ceiling is only hit when you need new *surface syntax*. As long as your
custom concept can be **spelled in syntax Postgres already accepts**, you need
no grammar change at all — you just lower that subtree differently. Postgres's
grammar is generous, so this covers a lot:

- A **function-call shape**, `myverb(a, b)`, parses to a `FuncCall`, and *you*
  decide what `myverb` means. That is a general-purpose extension point costing
  zero grammar changes — exactly how `pglower` already handles `sum`,
  `coalesce`, `avg`.
- **Operators**: Postgres accepts operator tokens it doesn't define (`@>`,
  `<->`, custom names), so novel infix semantics ride in on an `A_Expr` you
  interpret.
- **Casts, `WITH`, set operations, `CASE`** are all structural hooks you can
  repurpose in the lowering.

So the rule of thumb is: **lean on Postgres's syntax as a carrier, and put your
customization entirely in the lowering pass.** If your extension is *semantic*
(what a construct means), `pg_query` is fine — arguably better than hand-rolling,
since you inherit precedence and nesting for free. If it is *syntactic* (a new
token arrangement), you have hit the wall.

### The one-line decision

- **Reach for `pg_query`** when the input *is* PostgreSQL (or a strict subset),
  correctness matters more than control of the surface, and any customization
  you need is semantic and expressible through the function-call / operator
  hooks above.
- **Hand-roll** (the Steps 1–3 way) when you need to *own the surface syntax* —
  new keywords, new clause shapes, a non-Postgres dialect — or when a small,
  dependency-free, extensible, version-stable parser is itself the goal.

Put most briefly: `pg_query` trades **syntactic extensibility** for
**immediate, correct coverage**. You keep semantic flexibility — it's just a
tree, lower it however you like — but you give up the ability to teach it new
grammar.

## The strongest reason to want this: speaking the Postgres wire protocol

The decision rule above weighs `pg_query` as a *parser*. But there is a bigger
motivation that reframes the whole tradeoff: **compatibility with the Postgres
wire protocol.** If a system speaks the PostgreSQL frontend/backend protocol on
the wire, then every client that already speaks it — `psql`, the JDBC and ODBC
drivers, `libpq`, ORMs, BI and dashboarding tools — can connect to it unchanged.
That is an enormous, ready-made ecosystem to inherit, and it is the reason
systems like CockroachDB, YugabyteDB, Materialize, RisingWave, QuestDB, and
DuckDB's pg-wire endpoint all chose to look like Postgres on the wire rather
than invent their own protocol and client libraries.

Here is the part that makes `pg_query` almost mandatory for that goal, rather
than merely convenient. A client does **not** open a connection and immediately
send the user's query. It first issues a barrage of traffic you did not write
and cannot control:

- **Protocol and session setup** — the startup packet, authentication, and
  `ParameterStatus`/`SET`/`SHOW` exchanges a driver expects.
- **Discovery / catalog introspection** — queries against `pg_catalog` and
  `information_schema` (`pg_type`, `pg_attribute`, `pg_class`, …) that drivers
  and tools fire automatically to learn types, tables, columns, and OIDs before
  they will run anything the user typed.

Every one of those must **parse exactly as Postgres parses it** — they *are*
Postgres SQL, emitted by Postgres's own client stack. A hand-rolled parser would
have to chase that entire surface, and keep chasing it as drivers evolve.
`pg_query` delivers it by construction: because it is Postgres's own grammar,
the discovery traffic parses for free. This is where "you inherit Postgres's
semantics whether you want them or not" flips from a cost into the *deliverable*
— bug-for-bug fidelity with Postgres is precisely what the clients are testing
for.

Be clear about scope, though, so this doesn't overclaim: `pg_query` solves only
the **parse** leg of wire compatibility. Two large pieces sit beside it:

1. **The wire protocol itself** — message framing, authentication, and the
   extended-query flow (`Parse`/`Bind`/`Describe`/`Execute`), including handing
   back a `RowDescription` with correct type OIDs (see the [PostgreSQL
   frontend/backend protocol](https://www.postgresql.org/docs/current/protocol.html)).
   `pg_query` does not speak the wire; it only turns query text into a tree.
2. **Catalog emulation** — parsing a `pg_catalog` query is not answering it. To
   satisfy discovery you must actually *return plausible rows* from an
   `information_schema`/`pg_catalog` your engine synthesizes. That is a
   substantial modelling effort in its own right.

So `pg_query` is necessary-but-not-sufficient here: it is the component that lets
the ecosystem's SQL — user queries *and* the invisible discovery calls — through
the front door, while a wire codec and a synthetic catalog are what keep the
client happy once inside. For this class of goal, the extensibility ceiling
stops mattering almost entirely: you are not trying to invent syntax, you are
trying to be indistinguishable from Postgres, and inheriting its exact grammar
is the fastest honest way there.

## Why protobuf, not JSON

`libpg_query` offers the parse tree as both JSON and protobuf, and either works.
We chose protobuf on a simple argument: **once you have taken on the heavyweight
PG-parser dependency, the marginal cost of the protobuf-c glue is small, and it
makes the lowering code simpler.** Concretely:

- Protobuf decodes into **typed C structs** (`PgQuery__SelectStmt`,
  `PgQuery__AExpr`, …), so the lowering is straight-line field access —
  `s->from_clause`, `e->lexpr`, `n->node_case` — checked by the C compiler. The
  JSON path would need a hand-rolled JSON reader plus string-keyed lookups and
  its own escape handling (Postgres emits operators like `>` as `\u003e`).
- The one proto3 wrinkle to know: a zero-valued scalar is *omitted*, so an
  integer literal `0` arrives as an `A_Const` whose `ival` message is absent —
  `lower_expr` treats a missing `ival` as `0`. (The JSON shows the same thing as
  `"ival":{}`, so neither form escapes it.)

The tradeoff against the project's ethos is real — the generated `.pb-c.c` is
enormous — but it is *generated*, not authored and not committed, and it is
walled off in the optional Step 3b build. The hand-written part, `pglower.c`,
stays small and readable, which is the part that carries the lesson.

## Why keep it optional and out of the default build

The core repo stays small, fast, and dependency-free; Step 3b is opt-in:

- it lives in its own `pglower.c` behind `make pglower` / `make test3b`. A probe
  (`tests/pgenv.sh`) locates `libpg_query`, `protobuf-c`, and `protoc-c`; if any
  is missing it prints why and the target **skips (exit 0) rather than fails**,
  so a machine without the optional dependency still builds and tests green. It
  is deliberately absent from `make all`.
- `pglower.c` reuses `sqlparser.c`'s K value core — the `K` type,
  `ki`/`ks`/`ktn`/`klist`, `dec_ref`, `print_k`, and the shared `emit_query` —
  **verbatim**. Only the lowering pass is new. That reuse is itself the point:
  the AST is a stable *target* that a completely different front-end can aim at
  without touching anything downstream.

## Code layout

The Step 3b lowering lives under the `===== STEP 3b: lower a Postgres parse
tree into the K AST =====` banner in `pglower.c`, on top of the reused K core:

- `run`, which calls `pg_query_parse_protobuf`, `..._unpack`s the buffer into a
  `PgQuery__ParseResult`, requires exactly one statement, lowers it, prints, and
  frees. A genuine PostgreSQL syntax error (`result.error`) aborts here — so
  malformed SQL is rejected by the real parser, not by us.
- `lower_stmt`, dispatching on `node_case`
  (`SELECT_STMT` / `UPDATE_STMT` / `DELETE_STMT`); anything else `die()`s.
- `lower_select`, which reads `from_clause` / `where_clause` / `group_clause` /
  `target_list` into `(t, c, b, a)`, calls the shared `emit_query`, then applies
  the `DISTINCT` / `ORDER BY` / `LIMIT` wrappers from `distinct_clause` /
  `sort_clause` / `limit_count` / `limit_offset`;
- `lower_expr` (with `lower_aexpr` / `lower_bool` / `lower_call`), mapping
  `A_Expr` / `BoolExpr` / `FuncCall` / `CoalesceExpr` / `ColumnRef` / `A_Const`
  to K nodes via `op_head` (the operator mapping) and `flatten_and` (both the
  same rules as Step 3), with unmodelled node kinds routed to `die()`;
- `is_star`, the one-line `*`-→-`()` collapse, and `lower_target`, which turns a
  `ResTarget.name` alias into the `:` assignment shape;
- reused verbatim from `sqlparser.c`: the `K` type, `ki`/`ks`/`ktn`/`klist`,
  `dec_ref`, `print_k`, `node2`/`node3`, `flatten_and`, and `emit_query`.

## What's deferred

Same spirit as [`SQL.md`](SQL.md)'s deferrals — each is "more of the same
lowering":

- **String and float literals** — `A_Const` with an `sval` / `fval`; only
  integer `ival` is lowered today (matching the README's int-only type surface),
  so anything else `die()`s.
- **`IN` / `LIKE` / `BETWEEN` / `IS NULL`** — these are distinct PG nodes
  (`A_Expr` with `AEXPR_IN`, `NullTest`, etc.); each is one more `lower_expr`
  arm mapping to a sym head.
- **`HAVING`**, **joins / multi-table `FROM`**, **`INSERT`**, mixed `ORDER BY`
  directions, **`WITH`**, and **set operations** (`UNION`/…) — present in the PG
  tree, simply not lowered yet; each `die()`s as unsupported until added.
