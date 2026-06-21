#!/usr/bin/env bash
#
# Golden test runner for kparser.
#
# Each case in cases.tsv feeds one line to the REPL and compares the printed
# AST (after stripping the two-space prompt). Cases marked @ABORT must make
# the parser exit with a nonzero status (the die() paths).
#
# Usage: tests/run.sh [path-to-kparser-binary]   (default: ./kparser)
# Exit status is 0 iff every case passes.

set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="${1:-$DIR/../kparser}"
CASES="$DIR/cases.tsv"

if [ ! -x "$BIN" ]; then
    echo "tests: binary not found or not executable: $BIN" >&2
    echo "tests: build it first (e.g. 'make')." >&2
    exit 2
fi

pass=0
fail=0

# Run one input, return the printed AST with the leading prompt stripped.
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
    if [ "$expected" = "@ABORT" ]; then
        check_abort "$input"
    else
        check "$input" "$expected"
    fi
done <"$CASES"

# Generated abort case: a name longer than MAX_NAME (256) must abort.
# (Kept out of the data file because it's a 300-byte token.)
longname="$(head -c 300 /dev/zero | tr '\0' a)"
check_abort "$longname"

echo "----------------------------------------"
echo "kparser tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
