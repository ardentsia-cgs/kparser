#!/usr/bin/env bash
#
# STEP 3b dependency probe for pglower.
#
# Locates libpg_query (the PostgreSQL parser as a C library), its bundled
# pg_query.proto, protobuf-c, and the protoc-c compiler. On success it prints
# shell assignments for the build recipe to eval:
#
#   PG_INC=...      include dirs (for -I)
#   PG_LIB=...      library dirs (for -L)
#   PG_PROTO=...    path to pg_query.proto
#   PROTOC_C=...    protoc-c binary (or `protoc` with the c plugin)
#
# and exits 0. If any piece is missing it prints a human-readable reason to
# stderr and exits 1, so `make test3b` can SKIP (not fail) on machines without
# the optional dependency -- keeping the default build and CI green.

set -u

say()  { printf '%s\n' "$*"; }        # stdout: eval-able assignments
warn() { printf '%s\n' "$*" >&2; }    # stderr: human-readable reason

# --- locate a prefix that contains pg_query.h ---
pg_prefix=""
for cand in \
    "${LIBPG_QUERY_PREFIX:-}" \
    "$(command -v brew >/dev/null 2>&1 && brew --prefix libpg_query 2>/dev/null)" \
    /usr/local /opt/homebrew /usr
do
    [ -n "$cand" ] || continue
    if [ -f "$cand/include/pg_query.h" ]; then pg_prefix="$cand"; break; fi
done
if [ -z "$pg_prefix" ]; then
    warn "pglower: libpg_query not found (looked for include/pg_query.h)."
    warn "         install it (e.g. 'brew install libpg_query') or set LIBPG_QUERY_PREFIX."
    exit 1
fi

proto="$pg_prefix/include/pg_query/pg_query.proto"
if [ ! -f "$proto" ]; then
    warn "pglower: found libpg_query but not its pg_query.proto at $proto."
    exit 1
fi

# --- locate protobuf-c (header + lib) and the protoc-c compiler ---
pc_prefix=""
for cand in \
    "${PROTOBUF_C_PREFIX:-}" \
    "$(command -v brew >/dev/null 2>&1 && brew --prefix protobuf-c 2>/dev/null)" \
    /usr/local /opt/homebrew /usr
do
    [ -n "$cand" ] || continue
    if [ -f "$cand/include/protobuf-c/protobuf-c.h" ]; then pc_prefix="$cand"; break; fi
done
if [ -z "$pc_prefix" ]; then
    warn "pglower: protobuf-c not found (looked for include/protobuf-c/protobuf-c.h)."
    warn "         install it (e.g. 'brew install protobuf-c')."
    exit 1
fi

protoc_c="$(command -v protoc-c || true)"
if [ -z "$protoc_c" ]; then
    warn "pglower: protoc-c compiler not found on PATH (part of protobuf-c)."
    exit 1
fi

say "PG_INC='-I. -I$pg_prefix/include -I$pg_prefix/include/pg_query -I$pc_prefix/include'"
say "PG_LIB='-L$pg_prefix/lib -L$pc_prefix/lib -lpg_query -lprotobuf-c'"
say "PG_PROTO='$proto'"
say "PROTOC_C='$protoc_c'"
exit 0
