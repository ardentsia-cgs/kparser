#!/usr/bin/env bash
#
# STEP 3b cross-check runner for pglower.
#
# The whole point of pglower is that it targets the SAME (`verb; t; c; b; a)
# AST as the hand-rolled sqlparser. So it is checked against the SAME golden
# file, tests/sql_cases.tsv -- the very cases sqlparser --sql is tested against
# in `make test3`. If pglower reproduces those ASTs from PostgreSQL's own parse
# tree, then two entirely independent front-ends (a ~530-line teaching parser
# and the actual PostgreSQL grammar) converge on one AST, checked against
# ground truth rather than a fixture only one of them authored.
#
# Usage: tests/run_pg.sh [path-to-pglower-binary] [path-to-cases.tsv]
#   binary defaults to ./pglower, cases default to tests/sql_cases.tsv.
# Exit status is 0 iff every (non-skipped) case passes.
#
# A SMALL set of cases legitimately differs because the real PostgreSQL parser
# is not the teaching parser -- these are SKIPPED, with the reason printed, so
# the divergence is documented rather than hidden. See SQL_PG.md.

set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="${1:-$DIR/../pglower}"
CASES="${2:-$DIR/sql_cases.tsv}"

if [ ! -x "$BIN" ]; then
    echo "tests: binary not found or not executable: $BIN" >&2
    exit 2
fi

# Inputs where PostgreSQL's parser legitimately yields a different tree than the
# hand-rolled sqlparser. Keyed by exact input line; the value is the reason.
skip_reason() {
    case "$1" in
        "SeLeCt A FROM T")
            echo "PostgreSQL case-folds unquoted identifiers (A->a, T->t); sqlparser preserves case" ;;
        *) echo "" ;;
    esac
}

pass=0
fail=0
skip=0

ast_of() {
    printf '%s\n' "$1" | "$BIN" 2>/dev/null | sed -n '1s/^  //p'
}

check() { # input  expected
    local in="$1" exp="$2" got
    got="$(ast_of "$in")"
    if [ "$got" = "$exp" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL  input : %s\n      expect: %s\n      got   : %s\n' \
            "$in" "$exp" "$got"
    fi
}

check_abort() { # input
    local in="$1"
    printf '%s\n' "$in" | "$BIN" >/dev/null 2>&1
    if [ "$?" -ne 0 ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL  input : %s\n      expect: nonzero exit (abort)\n      got   : exit 0\n' "$in"
    fi
}

while IFS=$'\t' read -r input expected || [ -n "${input:-}" ]; do
    case "$input" in '' | //*) continue ;; esac
    reason="$(skip_reason "$input")"
    if [ -n "$reason" ]; then
        skip=$((skip + 1))
        printf 'SKIP  input : %s\n      reason: %s\n' "$input" "$reason"
        continue
    fi
    if [ "$expected" = "@ABORT" ]; then
        check_abort "$input"
    else
        check "$input" "$expected"
    fi
done <"$CASES"

echo "----------------------------------------"
echo "pglower cross-check: $pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
