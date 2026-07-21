/* strdup is POSIX, not ISO C; request it explicitly so a strict
 * -std=c99/-std=c11 build on glibc still declares it. Must precede any
 * system header include. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>   /* isatty -- name the mode in the prompt only when
                       * interactive, so the piped golden tests see the plain
                       * two-space prompt they strip. */

#include <pg_query.h>
#include "pg_query.pb-c.h"

/* STEP 3b (pglower): a follow-on to SQL.md / SQL_PG.md.
 *
 * Steps 1-3 hand-write parsers. This file does the OPPOSITE: it does not parse
 * SQL at all. It hands the text to libpg_query -- the ACTUAL PostgreSQL parser,
 * extracted as a standalone C library -- and then LOWERS the resulting Postgres
 * parse tree into the very same (`verb; t; c; b; a) AST that ksql (Step 2) and
 * the hand-rolled SQL front-end (Step 3) produce. So
 *   SELECT * FROM t   -->   (`select;`t;();();())
 * is the exact tree sqlparser --sql gives, only its source is Postgres's own
 * tree rather than a scanner we wrote.
 *
 * Step 3 owns a FRONT-END (text -> AST) and teaches scanning + precedence.
 * Step 3b owns a BACK-END (PG tree -> AST) and teaches LOWERING: precedence,
 * clause completeness and the whole SQL grammar arrive already solved, so what
 * is left is a small tree-to-tree translation whose rules are the ones SQL.md
 * already names. The K value core below (struct k0, the constructors, ref
 * counting, print_k) and emit_query are reused VERBATIM from sqlparser.c -- the
 * AST is a stable TARGET a completely different front-end can aim at without
 * touching anything downstream. See SQL_PG.md.
 *
 * We use the PROTOBUF output (pg_query_parse_protobuf) rather than the JSON
 * output: once we depend on the heavyweight PG parser, the protobuf-c glue is a
 * small marginal cost and buys us TYPED node structs, which keeps the lowering
 * code below straight-line field access instead of a hand-rolled JSON walk. */

/* ===== the K value core (reused verbatim from sqlparser.c) ===== */

static _Noreturn void die(const char *msg) {
    fprintf(stderr, "pglower: %s\n", msg);
    exit(1);
}

static void *xcalloc(size_t n, size_t sz)  { void *p = calloc(n, sz); if (!p) die("out of memory"); return p; }
static char *xstrdup(const char *s)        { char *p = strdup(s);     if (!p) die("out of memory"); return p; }

typedef signed char G;
typedef int         I;
typedef long long   J;
typedef char       *S;

#define KL  0
#define KI  6
#define KS  11
#define KV1 101
#define KV2 102

typedef struct k0 {
    signed char m, a, t;
    G u;
    I r;
    union {
        I i;
        S s;
        struct k0 *k;
        struct { J n; G G0[1]; };
    };
} *K;

#define kI(x) ((I*)((x)->G0))
#define kS(x) ((S*)((x)->G0))
#define kK(x) ((K*)((x)->G0))

static K ka(signed char t) {
    K x = xcalloc(1, sizeof(struct k0));
    x->t = t;
    return x;
}
static K ki(I v)           { K x = ka(-KI); x->i = v; return x; }
static K ks(const char *s) { K x = ka(-KS); x->s = xstrdup(s); return x; }

static K ktn(signed char t, J n) {
    size_t elt = (t == KI) ? sizeof(I) :
                 (t == KS) ? sizeof(S) :
                 (t == KL) ? sizeof(K) : sizeof(G);
    K x = xcalloc(1, sizeof(struct k0) + (size_t)n * elt);
    x->t = t; x->n = n;
    return x;
}

static K klist(K *xs, J n) {
    K x = ktn(KL, n);
    for (J i = 0; i < n; i++) kK(x)[i] = xs[i];
    return x;
}

static void dec_ref(K x) {
    if (!x) return;
    if (x->r > 0) { x->r--; return; }
    switch (x->t) {
    case KL:
        for (J i = 0; i < x->n; i++) dec_ref(kK(x)[i]);
        break;
    case KS:
        for (J i = 0; i < x->n; i++) free(kS(x)[i]);
        break;
    case -KS:
        free(x->s);
        break;
    }
    free(x);
}

static const char VERB_CHARS[] = ":+-*%!&|<>=~,^#_$?@.";
#define NVERBS (sizeof(VERB_CHARS) - 1)

static int verb_index(char c) {
    const char *p = strchr(VERB_CHARS, c);
    return p ? (int)(p - VERB_CHARS) : -1;
}

static K kverb(int monadic, int idx) {
    K x = ka(monadic ? KV1 : KV2);
    x->i = idx;
    return x;
}

#define V_ELIDED 2

static void print_k(K x) {
    if (!x) { printf("?[null]"); return; }
    switch (x->t) {
    case -KI:
        printf("%d", x->i);
        break;
    case  KI:
        for (J i = 0; i < x->n; i++) printf("%s%d", i ? " " : "", kI(x)[i]);
        break;
    case -KS:
        printf("`%s", x->s);
        break;
    case  KS:
        if (x->n == 1) putchar(',');
        for (J i = 0; i < x->n; i++) printf("`%s", kS(x)[i]);
        break;
    case  KL:
        putchar('(');
        for (J i = 0; i < x->n; i++) {
            if (i) putchar(';');
            print_k(kK(x)[i]);
        }
        putchar(')');
        break;
    case KV1: case KV2:
        if (x->u & V_ELIDED) break;
        if (x->i >= 0 && x->i < (int)NVERBS) {
            putchar(VERB_CHARS[x->i]);
            if (x->t == KV1) putchar(':');
        } else {
            printf("?[t=%d,i=%d]", x->t, x->i);
        }
        break;
    default:
        printf("?[t=%d]", x->t);
    }
}

/* Small AST constructors and the shared query-node builder, reused from
 * sqlparser.c. emit_query is the single point where the AST becomes
 * target-independent: sqlparser's parse_select and this file's lower_select
 * both end here, which is what makes `SELECT * FROM t` and `select from t`
 * literally the same tree. */
static K node2(K a, K b)      { K w = ktn(KL, 2); kK(w)[0]=a; kK(w)[1]=b;              return w; }
static K node3(K a, K b, K c) { K w = ktn(KL, 3); kK(w)[0]=a; kK(w)[1]=b; kK(w)[2]=c; return w; }

static K emit_query(K head, K t, K c, K b, K a) {
    K w = ktn(KL, 5);
    kK(w)[0] = head; kK(w)[1] = t; kK(w)[2] = c; kK(w)[3] = b; kK(w)[4] = a;
    return w;
}

/* Flatten a top-level &-chain into a flat list, so a WHERE of pure ANDs lands
 * in the c-list exactly like ksql's comma-separated `where d>0,e<5`. Reused
 * verbatim from sqlparser.c. A non-AND top (e.g. |) is kept as one phrase. */
#define MAX_VEC 4096
static void flatten_and(K e, K *buf, int *n) {
    if (e->t == KL && e->n == 3 && kK(e)[0]
        && kK(e)[0]->t == KV2 && kK(e)[0]->i == verb_index('&')) {
        K l = kK(e)[1], r = kK(e)[2];
        kK(e)[1] = NULL; kK(e)[2] = NULL;
        dec_ref(e);
        flatten_and(l, buf, n);
        flatten_and(r, buf, n);
    } else {
        if (*n >= MAX_VEC) die("where clause too complex");
        buf[(*n)++] = e;
    }
}

/* ===== STEP 3b: lower a Postgres parse tree into the K AST =====
 *
 * Everything above is shared with sqlparser.c. Everything below reads the
 * protobuf node structs generated from libpg_query's pg_query.proto and maps
 * each node to the K shape SQL.md already documents. Node kinds outside the
 * teaching subset (joins, subqueries, CTEs, ...) are not lowered -- they
 * die() as unsupported rather than emit a wrong tree, so the subset is
 * enforced by the LOWERING, not by a parser. */

typedef PgQuery__Node       Node;
typedef PgQuery__SelectStmt Select;

static K lower_expr(Node *n);

/* The single-String name inside a ColumnRef / funcname list. */
static const char *node_str(Node *n) {
    if (n->node_case != PG_QUERY__NODE__NODE_STRING) die("expected a name");
    return n->string->sval;
}

/* Map a SQL operator glyph to its K head. Single glyphs reuse the K verb
 * (with SQL `/` -> K `%` divide); two-char comparisons have no K glyph, so
 * they become sym heads -- identical to sqlparser's operator mapping. */
static K op_head(const char *op) {
    if (op[1] == '\0') {
        char c = op[0];
        if (c == '/') return kverb(0, verb_index('%'));
        if (strchr("=<>+-*", c)) return kverb(0, verb_index(c));
    }
    if (!strcmp(op, "<=") || !strcmp(op, ">=") || !strcmp(op, "<>")) return ks(op);
    die("unsupported operator");
}

/* A_Expr: an infix operator, or a prefix operator when lexpr is absent
 * (unary minus -> K's monadic negate -:). */
static K lower_aexpr(PgQuery__AExpr *e) {
    if (e->kind != PG_QUERY__A__EXPR__KIND__AEXPR_OP || e->n_name != 1)
        die("unsupported operator expression");
    const char *op = node_str(e->name[0]);
    if (!e->lexpr) {                               /* prefix operator */
        if (strcmp(op, "-") == 0)
            return node2(kverb(1, verb_index('-')), lower_expr(e->rexpr));
        die("unsupported prefix operator");
    }
    return node3(op_head(op), lower_expr(e->lexpr), lower_expr(e->rexpr));
}

/* BoolExpr: AND/OR fold their args left-associatively into K's &/| (matching
 * sqlparser's left-associative precedence climb); NOT is K's monadic ~:.
 * A top-level AND is later re-flattened by flatten_and in lower_where. */
static K lower_bool(PgQuery__BoolExpr *b) {
    if (b->boolop == PG_QUERY__BOOL_EXPR_TYPE__NOT_EXPR)
        return node2(kverb(1, verb_index('~')), lower_expr(b->args[0]));
    int g = (b->boolop == PG_QUERY__BOOL_EXPR_TYPE__AND_EXPR) ? '&' : '|';
    K acc = lower_expr(b->args[0]);
    for (size_t i = 1; i < b->n_args; i++)
        acc = node3(kverb(0, verb_index(g)), acc, lower_expr(b->args[i]));
    return acc;
}

/* FuncCall / CoalesceExpr: the K application shape (`fn; arg; ...) -- the very
 * (f;x;y) tree the parser already builds for f[x;y], so sum(amt) is (`sum;`amt),
 * identical to ksql's `sum amt`. (coalesce is a distinct PG node but lowers the
 * same way.) */
static K lower_call(const char *fn, Node **args, size_t nargs) {
    K buf[MAX_VEC]; int m = 0;
    buf[m++] = ks(fn);
    for (size_t i = 0; i < nargs; i++) {
        if (m >= MAX_VEC) die("too many arguments");
        buf[m++] = lower_expr(args[i]);
    }
    return klist(buf, m);
}

static K lower_expr(Node *n) {
    if (!n) die("missing expression");
    switch (n->node_case) {
    case PG_QUERY__NODE__NODE_COLUMN_REF: {
        PgQuery__ColumnRef *c = n->column_ref;
        if (c->n_fields != 1) die("qualified column names are not supported");
        return ks(node_str(c->fields[0]));         /* `name */
    }
    case PG_QUERY__NODE__NODE_A_CONST: {
        PgQuery__AConst *k = n->a_const;
        if (k->val_case == PG_QUERY__A__CONST__VAL_IVAL)
            return ki(k->ival ? k->ival->ival : 0); /* proto3 omits a zero ival */
        die("only integer literals are supported");
    }
    case PG_QUERY__NODE__NODE_A_EXPR:
        return lower_aexpr(n->a_expr);
    case PG_QUERY__NODE__NODE_BOOL_EXPR:
        return lower_bool(n->bool_expr);
    case PG_QUERY__NODE__NODE_FUNC_CALL: {
        PgQuery__FuncCall *f = n->func_call;
        if (f->n_funcname == 0) die("function has no name");
        return lower_call(node_str(f->funcname[f->n_funcname - 1]), f->args, f->n_args);
    }
    case PG_QUERY__NODE__NODE_COALESCE_EXPR: {
        PgQuery__CoalesceExpr *e = n->coalesce_expr;
        return lower_call("coalesce", e->args, e->n_args);
    }
    default:
        die("unsupported expression node");
    }
}

/* Is this target list exactly `*`? Postgres spells "all columns" as a real
 * node -- one ResTarget whose value is a ColumnRef of a single A_Star -- while
 * this AST spells it as ABSENCE (an empty phrase list). Collapsing the one to
 * the other is the SELECT * <-> `select from t` equivalence. */
static int is_star(Select *s) {
    if (s->n_target_list != 1) return 0;
    if (s->target_list[0]->node_case != PG_QUERY__NODE__NODE_RES_TARGET) return 0;
    Node *v = s->target_list[0]->res_target->val;
    if (!v || v->node_case != PG_QUERY__NODE__NODE_COLUMN_REF) return 0;
    PgQuery__ColumnRef *c = v->column_ref;
    return c->n_fields == 1 && c->fields[0]->node_case == PG_QUERY__NODE__NODE_A_STAR;
}

/* A ResTarget with a name is an alias: the existing (:;`name;expr) assignment
 * shape, so SELECT sum(amt) AS qty yields (:;`qty;(`sum;`amt)) -- ksql's
 * qty:sum amt written the other way round. SET assignments share this. */
static K lower_target(PgQuery__ResTarget *rt) {
    K e = lower_expr(rt->val);
    if (rt->name && rt->name[0])
        return node3(kverb(0, verb_index(':')), ks(rt->name), e);
    return e;
}

static K lower_list(Node **items, size_t n, K (*f)(Node *)) {
    K buf[MAX_VEC]; int m = 0;
    for (size_t i = 0; i < n; i++) {
        if (m >= MAX_VEC) die("list too long");
        buf[m++] = f(items[i]);
    }
    return klist(buf, m);
}

static K lower_target_node(Node *n) {
    if (n->node_case != PG_QUERY__NODE__NODE_RES_TARGET) die("expected a target");
    return lower_target(n->res_target);
}

/* WHERE lowers the whole expression, then flattens a top-level &-chain into the
 * c-list -- so `WHERE d>0 AND e<5` yields two phrases, exactly like ksql's
 * `where d>0,e<5`, while a top-level OR stays one combined phrase. */
static K lower_where(Node *w) {
    K e = lower_expr(w);
    K buf[MAX_VEC]; int n = 0;
    flatten_and(e, buf, &n);
    return klist(buf, n);
}

/* The single FROM table. Joins / multi-table FROM are deferred (as in Step 3),
 * so anything but one RangeVar is unsupported. */
static K lower_from(Node **from, size_t n) {
    if (n != 1) die("only a single FROM table is supported");
    if (from[0]->node_case != PG_QUERY__NODE__NODE_RANGE_VAR)
        die("only a plain table in FROM is supported");
    return ks(from[0]->range_var->relname);
}

/* An A_Const integer used as a LIMIT / OFFSET count. */
static I lower_int(Node *n) {
    if (!n || n->node_case != PG_QUERY__NODE__NODE_A_CONST
        || n->a_const->val_case != PG_QUERY__A__CONST__VAL_IVAL)
        die("expected an integer");
    return n->a_const->ival ? n->a_const->ival->ival : 0;
}

/* SELECT [DISTINCT] (*|items) FROM t [WHERE ..] [GROUP BY ..]
 *        [ORDER BY cols [ASC|DESC]] [LIMIT n [OFFSET m]]
 * The relational core fills (`select; t; c; b; a); DISTINCT/ORDER BY/LIMIT wrap
 * the result as ordinary K verbs in SQL's logical processing order -- exactly
 * as sqlparser's parse_select does, only here the clauses are read off the PG
 * tree rather than parsed:
 *   SELECT a FROM t ORDER BY x LIMIT 10  ->  10 # `x asc select a from t  ==
 *     (#;10;(`asc;`x;(`select;`t;();();(`a)))). */
static K lower_select(Select *s) {
    if (s->n_from_clause == 0) die("SELECT without FROM is not supported");
    if (s->n_target_list == 0) die("SELECT needs a select list or '*'");
    if (s->having_clause)      die("HAVING is not supported (deferred)");
    if (s->with_clause)        die("WITH is not supported (deferred)");
    if (s->op != PG_QUERY__SET_OPERATION__SETOP_NONE)
        die("set operations are not supported (deferred)");

    K a = is_star(s) ? ktn(KL, 0)
                     : lower_list(s->target_list, s->n_target_list, lower_target_node);
    K t = lower_from(s->from_clause, s->n_from_clause);
    K c = s->where_clause ? lower_where(s->where_clause) : ktn(KL, 0);
    K b = s->n_group_clause ? lower_list(s->group_clause, s->n_group_clause, lower_expr)
                            : ktn(KL, 0);

    K r = emit_query(ks("select"), t, c, b, a);

    if (s->n_distinct_clause)                          /* DISTINCT -> ?: unique */
        r = node2(kverb(1, verb_index('?')), r);

    if (s->n_sort_clause) {                            /* ORDER BY -> `asc/`dsc */
        K cols[MAX_VEC]; int m = 0; int desc = -1;
        for (size_t i = 0; i < s->n_sort_clause; i++) {
            PgQuery__SortBy *sb = s->sort_clause[i]->sort_by;
            int d = (sb->sortby_dir == PG_QUERY__SORT_BY_DIR__SORTBY_DESC) ? 1 : 0;
            if (desc == -1) desc = d;
            else if (desc != d) die("mixed ORDER BY directions are not supported");
            cols[m++] = lower_expr(sb->node);
        }
        K oc = (m == 1) ? cols[0] : klist(cols, m);
        r = node3(ks(desc ? "dsc" : "asc"), oc, r);
    }

    if (s->limit_count) {                              /* LIMIT -> # take */
        if (s->limit_offset)                           /* OFFSET -> _ drop */
            r = node3(kverb(0, verb_index('_')), ki(lower_int(s->limit_offset)), r);
        r = node3(kverb(0, verb_index('#')), ki(lower_int(s->limit_count)), r);
    } else if (s->limit_offset) {
        die("OFFSET without LIMIT is not supported");
    }

    return r;
}

/* UPDATE t SET col=expr,.. [WHERE ..]: SET assignments are the same
 * (:;`col;expr) shape as a SELECT alias, so they fill the a-slot; b is (). */
static K lower_update(PgQuery__UpdateStmt *u) {
    if (u->with_clause) die("WITH is not supported (deferred)");
    K t = ks(u->relation->relname);
    K a = lower_list(u->target_list, u->n_target_list, lower_target_node);
    K c = u->where_clause ? lower_where(u->where_clause) : ktn(KL, 0);
    return emit_query(ks("update"), t, c, ktn(KL, 0), a);
}

/* DELETE FROM t [WHERE ..] -> (`delete; t; c; (); ()). */
static K lower_delete(PgQuery__DeleteStmt *d) {
    if (d->with_clause) die("WITH is not supported (deferred)");
    K t = ks(d->relation->relname);
    K c = d->where_clause ? lower_where(d->where_clause) : ktn(KL, 0);
    return emit_query(ks("delete"), t, c, ktn(KL, 0), ktn(KL, 0));
}

static K lower_stmt(Node *n) {
    switch (n->node_case) {
    case PG_QUERY__NODE__NODE_SELECT_STMT: return lower_select(n->select_stmt);
    case PG_QUERY__NODE__NODE_UPDATE_STMT: return lower_update(n->update_stmt);
    case PG_QUERY__NODE__NODE_DELETE_STMT: return lower_delete(n->delete_stmt);
    default: die("only SELECT, UPDATE, and DELETE are supported");
    }
}

static void run(const char *src) {
    /* Hand the text to the real PostgreSQL parser; a genuine SQL syntax error
     * (e.g. `SELECT a FROM`) surfaces here as r.error and aborts. */
    PgQueryProtobufParseResult r = pg_query_parse_protobuf(src);
    if (r.error) {
        char msg[512];
        snprintf(msg, sizeof msg, "postgres: %s", r.error->message);
        pg_query_free_protobuf_parse_result(r);
        die(msg);
    }

    PgQuery__ParseResult *pr =
        pg_query__parse_result__unpack(NULL, r.parse_tree.len,
                                       (const uint8_t *)r.parse_tree.data);
    if (!pr) { pg_query_free_protobuf_parse_result(r); die("protobuf unpack failed"); }

    /* One statement per line, matching sqlparser (which rejects a second
     * statement after the optional trailing ';'). */
    if (pr->n_stmts != 1) {
        pg_query__parse_result__free_unpacked(pr, NULL);
        pg_query_free_protobuf_parse_result(r);
        die("expected a single statement");
    }

    K e = lower_stmt(pr->stmts[0]->stmt);
    print_k(e);
    putchar('\n');

    dec_ref(e);
    pg_query__parse_result__free_unpacked(pr, NULL);
    pg_query_free_protobuf_parse_result(r);
}

int main(void) {
    int interactive = isatty(STDIN_FILENO);
    char line[1024];
    for (;;) {
        fputs(interactive ? "pg> " : "  ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) { putchar('\n'); break; }
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;
        run(line);
    }
    pg_query_exit();
    return 0;
}
