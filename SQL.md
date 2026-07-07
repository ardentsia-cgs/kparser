# sql — Step 3

A follow-on to [`README.md`](README.md) and [`KSQL.md`](KSQL.md). Step 1
(`kparser.c`) parses core K. Step 2 (`ksqlparser.c`) adds **ksql**, K's own SQL
dialect. Step 3 (`sqlparser.c`) adds a **standard-SQL query** front-end (a small
slice of SQL — see [Scope](#scope-this-is-one-small-corner-of-sql)) — and the
whole point is that it targets the *same AST* as ksql:

```
  SELECT * FROM t
(`select;`t;();();())
```

is the exact tree ksql produces for `select from t`. `sqlparser.c` is
`ksqlparser.c` plus a ~480-line sql layer and a runtime **mode switch**; as
before, `diff ksqlparser.c sqlparser.c` *is* the lesson.

```sh
make sqlparser     # build it
make test3         # STEP 1 + STEP 2 suites (default ksql mode) + sql cases
./sqlparser --sql  # start in SQL mode
```

```
  SELECT a, b FROM t WHERE d > 0 AND e < 5 GROUP BY c
(`select;`t;((>;`d;0);(<;`e;5));(`c);(`a;`b))
```

## Scope: this is one small corner of SQL

Be clear about what "SQL" means here. The SQL standard is large, and divides
into several sub-languages; Step 3 implements a slice of just **one** of them:

| Sub-language | Examples | Step 3 |
| --- | --- | --- |
| **DQL** — queries | `SELECT … FROM … WHERE … GROUP BY …` | **yes** (the subset below) |
| **DML** — data manipulation | `INSERT`, `UPDATE`, `DELETE`, `MERGE` | partial (`UPDATE`, `DELETE`; no `INSERT`/`MERGE`) |
| **DDL** — schema | `CREATE`/`ALTER`/`DROP TABLE`, indexes, views | no |
| **DCL** — permissions | `GRANT`, `REVOKE` | no |
| **TCL** — transactions | `BEGIN`, `COMMIT`, `ROLLBACK`, `SAVEPOINT` | no |

And even within the query family, Step 3 is a teaching subset. It does **not**
cover, among much else: joins (`JOIN … ON`, multi-table `FROM`), subqueries and
common table expressions (`WITH`), set operations (`UNION`/`INTERSECT`/`EXCEPT`),
window functions (`OVER`), `HAVING`, `CASE`, `IN`/`LIKE`/`BETWEEN`/`IS NULL`,
and the full literal/type surface (only integers and a sym-shortcut for strings
exist). See [What's deferred](#whats-deferred) for the near-term omissions.

That narrowness is deliberate, and it's the point. This is the same shape of
claim the rest of the project makes — Step 1 parses core K, not all of K; Step
2 parses the four `[t;c;b;a]` query verbs, not all of ksql. The slice chosen
here is exactly the part of SQL that the `(verb; t; c; b; a)` tree already
models, because that is what makes the "two surfaces, one AST" lesson land.
DDL/DCL/TCL aren't "missing features" so much as a *different shape* — `CREATE
TABLE` describes a schema, not a `[t;c;b;a]` query — and folding them in would
be a separate exercise, not a bigger version of this one.

## The one idea: SQL and ksql are two surfaces over one AST

ksql showed that a query is sugar for a four-argument application
`(verb; t; c; b; a)` — `t`=from, `c`=where, `b`=by, `a`=phrases. That tree is
**target-independent**: it doesn't care whether the surface syntax was K's
`select … by … from … where …` or ANSI SQL's `SELECT … FROM … WHERE … GROUP BY
…`. Step 3 simply adds a second front-end that fills the same four slots, so
the two languages *converge on one tree*:

| SQL | ksql | shared AST |
| --- | --- | --- |
| `SELECT * FROM t` | `select from t` | `` (`select;`t;();();()) `` |
| `SELECT a FROM t WHERE d>0` | `select a from t where d>0` | `` (`select;`t;((>;`d;0));();(`a)) `` |
| `SELECT sum(amt) AS qty FROM t` | `select qty:sum amt from t` | `` (`select;`t;();();((:;`qty;(`sum;`amt)))) `` |

In code this is literally one function: both `parse_query` (ksql) and
`parse_sql_query` (sql) end at the shared `emit_query(head, t, c, b, a)`.

## `SELECT *` is `all columns` — i.e. nothing

The headline equivalence works because `*` desugars to *absence*. ksql writes
"all columns" by simply omitting the phrase list (`select from t`); SQL spells
it `*`. So in SQL mode a lone `*` becomes the **empty phrase list** `a = ()`,
and the two trees coincide. (A bare `SELECT FROM t` with neither `*` nor a
column list is a SQL error here — SQL requires you to write `*`.)

## A mode, not a blend

A given line is *either* K/ksql *or* SQL — never both. They disagree at the
token level (`,` joins in K but separates in SQL; `=` is a verb vs. equality;
`'abc'` is a string vs. nothing), and at the grammar level (K is
precedence-free, SQL is not). So the surface is chosen **before** scanning by a
`Mode`:

- `--sql` / `--ksql` on the command line set the initial mode (default
  `ksql`, which keeps Steps 1–2 byte-for-byte intact);
- `\sql` / `\ksql` typed at the REPL flip it live.

Getting the mode wrong is quiet, not loud: SQL typed into `ksql` mode parses as
*legal-but-wrong K* (`as`, `GROUP`, `BY` are just names there, so
`select sum(x) as mysum from t GROUP BY w` mis-parses rather than erroring).
To make the active surface obvious, an **interactive** REPL prompt names the
mode (`ksql> ` / `sql> `); when stdin is piped (the golden test runner) the
prompt stays the plain two spaces the runner strips, so the indicator never
leaks into tests.

The mode branches only the scanner (`scan` vs. `scan_sql`) and the top-level
parser (`parse_E` vs. `parse_sql_query`). Downstream — the K types, the
printer, ref-counting — is shared. This is why `make test3` re-runs the entire
Step 1 and Step 2 golden suites against `sqlparser` in its default mode: the
mode switch is a strict superset.

And in `sql` mode the SQL surface yields the *same* tree as the matching ksql
query — e.g. `SELECT sum(x) AS mysum FROM t GROUP BY w` and
`select mysum:sum(x) by w from t` both parse to
`` (`select;`t;();(`w);((:;`mysum;(`sum;`x)))) ``.

## What maps slot-for-slot

The relational core fills the four slots directly:

| SQL clause | slot | note |
| --- | --- | --- |
| `FROM t` | `t` | single table (joins deferred) |
| `WHERE …` | `c` | top-level `AND` chain flattens into the list |
| `GROUP BY …` | `b` | |
| select list / `SET …` | `a` | |

**Aliases are still assignment.** `sum(amt) AS qty` builds `(:;\`qty;(\`sum;\`amt))`
— the same `:` shape ksql gets from `qty:sum amt`, just written the other way
round. `AS` is optional (`sum(amt) qty` works too).

**Function calls are the application shape.** `sum(amt)` →
`(\`sum;\`amt)`, `coalesce(a,b)` → `(\`coalesce;\`a;\`b)` — the very `(f;arg;…)`
tree the parser already builds for `f[x;y]`.

**`AND` is the phrase separator.** ksql separates `where` conditions with a
top-level comma, whose implicit meaning is AND. SQL writes that AND. So a
top-level `AND`-chain is flattened into the `c`-list, making
`WHERE d>0 AND e<5` produce `((>;\`d;0);(<;\`e;5))` — identical to ksql's
`where d>0,e<5`. A top-level `OR` (or anything else) stays one combined phrase.

## What wraps the query instead of extending it

SQL's post-relational clauses — `DISTINCT`, `ORDER BY`, `LIMIT` — don't belong
in `[t;c;b;a]`. Rather than widen the tuple, they **wrap the query noun as
ordinary K verbs**, following SQL's own logical processing order
(FROM→WHERE→GROUP→SELECT→DISTINCT→ORDER→LIMIT, with the outermost wrapper
applied last):

```
SELECT a FROM t ORDER BY x LIMIT 10
```

desugars to the ordinary K expression `` 10 # `x asc select a from t ``:

```
(#;10;(`asc;`x;(`select;`t;();();(`a))))
```

The wrapper heads are all ordinary K: `asc`/`dsc` are the dyadic sort verbs
(kparc's `[f]asc`/`[f]dsc`), `#` is take, `_` is drop, and `?:` is monadic
unique. Writing `query` for the inner `(`select; t; c; b; a)` node:

| clause | wrapper |
| --- | --- |
| `DISTINCT` | `(?:; query)` (unique) |
| `ORDER BY c [ASC]` | `` (`asc; c; query) `` |
| `ORDER BY c DESC` | `` (`dsc; c; query) `` |
| `LIMIT n` | `(#; n; query)` (take) |
| `LIMIT n OFFSET m` | `(#; n; (_; m; query))` (take of drop) |

This keeps the query node itself identical between SQL and ksql — which is the
whole equivalence — and reuses the "queries are composable nouns" idea
[`KSQL.md`](KSQL.md) already relies on (`count select a from t`).

### Why composition rather than a variadic `[t;c;b;a;…]`

A tempting alternative is to make the query node variadic and carry sort/limit
as extra slots. We deliberately don't, for three reasons:

1. **It keeps the equivalence exact.** If SQL needed slots ksql never emits,
   the two surfaces would stop producing the same tree — and that sameness is
   the entire lesson.
2. **`[t;c;b;a]` is a cited convention** (nsl `ksql.k`, kparc's
   `#[t;c;b;a]` select) — exactly four arguments. Widening it invents a shape
   that exists nowhere else in K, against KSQL.md's "no new AST shape".
3. **The wrappers are ordinary K verbs.** `asc`, `dsc`, `#`, `_`, and unique
   `?:` are existing verbs, so the output is runnable K, not a parser artifact.

The honest counterpoint: some K dialects fold a sort/limit parameter into the
select brackets themselves, so a variadic form isn't unreasonable — it's just a
different lineage than the `[t;c;b;a]` one this project follows.

## The hard part: precedence

This is the one place SQL genuinely needs machinery K does not. K is
**precedence-free and right-to-left**; SQL has full operator precedence and is
left-associative. Reusing `parse_e` would mis-group SQL, so SQL gets its own
**precedence-climbing** parser (`parse_sql_expr`). The trees it builds are
ordinary K nodes — only the *grouping* differs:

```
WHERE x - y - z > 0
  SQL (left-assoc):  (>;(-;(-;`x;`y);`z);0)
  K parse_e:         would group `-` right-associatively
```

Binding levels (loosest → tightest): `OR` < `AND` < `NOT` (prefix) <
comparisons (`= <> < <= > >=`) < `+ -` < `* /` < unary `-`. `NOT` maps to K's
monadic `~:`, unary minus to `-:`.

## Operator mapping

Where K has a glyph, SQL uses it, so the output matches ksql:

| SQL | K head | prints |
| --- | --- | --- |
| `= < > + - *` | the K verb | `=` `<` `>` `+` `-` `*` |
| `/` (divide) | K `%` (divide) | `%` |
| `AND` / `OR` | K `&` / `|` | `&` / `|` |
| `NOT` | K `~:` | `~:` |
| `<= >= <>` (and `!=`) | sym head | `` `<= `` `` `>= `` `` `<> `` |

K has no two-glyph comparison, so `<=`/`>=`/`<>` become sym heads (`!=`
normalises to `<>`). SQL `/` is division, which in K is `%`.

## AST shapes

| Source | AST |
| --- | --- |
| `SELECT * FROM t` | `` (`select;`t;();();()) `` |
| `SELECT a,b FROM t` | `` (`select;`t;();();(`a;`b)) `` |
| `SELECT a FROM t WHERE d>0 AND e<5` | `` (`select;`t;((>;`d;0);(<;`e;5));();(`a)) `` |
| `SELECT a FROM t WHERE d>0 OR e<5` | `` (`select;`t;((\|;(>;`d;0);(<;`e;5)));();(`a)) `` |
| `SELECT sum(amt) AS qty FROM t` | `` (`select;`t;();();((:;`qty;(`sum;`amt)))) `` |
| `SELECT a,b FROM t GROUP BY c` | `` (`select;`t;();(`c);(`a;`b)) `` |
| `SELECT a FROM t ORDER BY x` | `` (`asc;`x;(`select;`t;();();(`a))) `` |
| `SELECT a FROM t LIMIT 10` | `` (#;10;(`select;`t;();();(`a))) `` |
| `SELECT DISTINCT a FROM t` | `` (?:;(`select;`t;();();(`a))) `` |
| `UPDATE t SET a=x+1 WHERE d>0` | `` (`update;`t;((>;`d;0));();((:;`a;(+;`x;1)))) `` |
| `DELETE FROM t WHERE d>0` | `` (`delete;`t;((>;`d;0));();()) `` |

## Design notes

### Keywords are reserved only positionally, and case-insensitively

`select`/`update`/`delete` start a statement; `from`/`where`/`group`/`by`/… are
recognised as clause keywords inside a statement. All are matched
case-insensitively (`SELECT` == `select`), but identifiers keep their original
case (`SELECT A FROM T` → `` (`select;`T;();();(`A)) ``).

### A clause keyword must carry its operand; clauses must be in order

As in ksql, the parser checks *structural* well-formedness only and otherwise
`die()`s: `SELECT a` (no `FROM`), `SELECT FROM t` (no `*`/columns),
`SELECT a FROM` (no table), `SELECT a FROM t WHERE` (empty condition),
`SELECT a FROM t GROUP BY` (empty group). And because a query is a complete
noun, anything dangling after its clauses (a clause out of order, e.g.
`GROUP BY … WHERE …`) is rejected as a trailing token.

### What's deferred

Kept out to stay focused (each is "more of the same"):

- **String literals** are represented as syms (a shortcut, matching the
  README's "strings are out of scope"); a string whose text equals a keyword
  is therefore an unsupported corner case.
- **`IN` / `LIKE` / `BETWEEN` / `IS NULL`** — more comparison forms; they would
  add sym-head operators (`` `in ``, `` `like ``) and, for `BETWEEN`, a second
  use of `AND`.
- **`HAVING`** — a post-group filter; naturally a `where` over the grouped
  result.
- **Joins / multi-table `FROM`**, mixed `ORDER BY` directions, and
  **`INSERT` / `UPSERT`** (the `[t;d]` shape, deferred in ksql too).

## Code layout

The Step 3 additions are tagged `STEP3` or grouped under the
`===== STEP 3: sql =====` banner in `sqlparser.c`:

- a `Mode` enum and the `--sql`/`--ksql` + `\sql`/`\ksql` plumbing in `main`
  and `run`;
- `emit_query`, factored out of `parse_query` so both front-ends share the one
  node builder;
- `scan_sql`, a second single-pass scanner (case-insensitive keywords,
  `'strings'`, two-char operators, `,` as a separator);
- `parse_sql_expr`, the precedence-climbing expression parser (with
  `parse_sql_prefix` / `parse_sql_primary` and the `sql_binop_prec` table);
- `parse_select` / `parse_update` / `parse_delete` / `parse_sql_query`, which
  collect the clauses, reorder them into `emit_query`, and wrap the result for
  `DISTINCT` / `ORDER BY` / `LIMIT`;
- small helpers: `ci_eq`, `sql_kw`, `sql_is_reserved` / `sql_is_name`,
  `flatten_and`, and the `node2` / `node3` constructors.
