# kparser — minimal K parser
# Apache License 2.0

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2 -std=c99
LDFLAGS ?=

BIN := kparser
SRC := kparser.c

# STEP 2: the ksql build (kparser.c + a ksql layer).
BIN2 := ksqlparser
SRC2 := ksqlparser.c

# STEP 3: the sql build (ksqlparser.c + a sql layer + a mode switch).
BIN3 := sqlparser
SRC3 := sqlparser.c

all: $(BIN) $(BIN2) $(BIN3)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN2): $(SRC2)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN3): $(SRC3)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Stricter warnings — useful while editing the parser.
# The two C11-extension warnings (anonymous struct and anonymous union)
# are expected: we use them intentionally inside the K struct for the
# flexible-array-tail idiom.
strict: CFLAGS += -Wpedantic -Wconversion -Wshadow
strict: clean $(BIN)

# Address + UB sanitizers for tracking down memory issues. Run with:
#   make debug && ./kparser
debug: CFLAGS := -O0 -g -Wall -Wextra -std=c99 -fsanitize=address,undefined
debug: clean $(BIN)

run: $(BIN)
	./$(BIN)

run2: $(BIN2)
	./$(BIN2)

# STEP 3: the sql REPL starts in --sql mode (toggle live with \ksql / \sql).
run3: $(BIN3)
	./$(BIN3) --sql

# Golden tests: every route through the scanner, parser, and printer.
test: $(BIN)
	tests/run.sh ./$(BIN)

# STEP 2 tests: the ksql binary must pass the STEP 1 golden suite too
# (proving it is a strict superset), then the ksql-specific cases.
test2: $(BIN2)
	tests/run.sh ./$(BIN2)
	tests/run.sh ./$(BIN2) tests/ksql_cases.tsv

# STEP 3 tests: in its default (ksql) mode the sql binary must pass the STEP 1
# and STEP 2 suites (proving the mode switch is a strict superset), then the
# sql-specific cases run with the binary in --sql mode.
test3: $(BIN3)
	tests/run.sh ./$(BIN3)
	tests/run.sh ./$(BIN3) tests/ksql_cases.tsv
	tests/run.sh ./$(BIN3) tests/sql_cases.tsv --sql

# Coverage build + line/branch report (requires gcov). Compiles in two
# steps so the gcov data files are named after kparser.c on every toolchain,
# drives the parser with the golden suite, then summarizes coverage.
coverage: clean
	$(CC) -O0 -g --coverage -std=c11 -Wall -Wextra -c kparser.c -o kparser.o
	$(CC) --coverage kparser.o -o $(BIN)
	tests/run.sh ./$(BIN)
	gcov -b kparser.c
	@rm -f kparser.o

clean:
	rm -f $(BIN) $(BIN2) $(BIN3) kparser.o *.gcda *.gcno *.gcov

.PHONY: all strict debug run run2 run3 test test2 test3 coverage clean
