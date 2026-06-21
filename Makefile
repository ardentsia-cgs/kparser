# kparser — minimal K parser
# Apache License 2.0

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2 -std=c99
LDFLAGS ?=

BIN := kparser
SRC := main.c

all: $(BIN)

$(BIN): $(SRC)
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

# Golden tests: every route through the scanner, parser, and printer.
test: $(BIN)
	tests/run.sh ./$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all strict debug run test clean
