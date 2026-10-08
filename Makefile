# partunit.device
#
# The default target builds and runs the host tests. The parser is written to
# be host-testable precisely so this needs no Amiga, no emulator and no
# cross-compiler - see docs/proposal.md Phase 1.
#
#   make            build and run the tests
#   make asan       same, under AddressSanitizer + UBSan
#   make strict     same, with pedantic C89 warnings (the m68k toolchains are
#                   older than the host compiler, so this catches portability
#                   problems before they reach a cross build)
#   make clean

CC      ?= cc
WARN     = -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual \
           -Wstrict-prototypes -Wmissing-prototypes -Wwrite-strings
CFLAGS  ?= -O1 -g $(WARN)

BUILD    = build
SRC      = src/ptparse.c
TESTSRC  = tests/test_ptparse.c tests/fixture.c
TESTBIN  = $(BUILD)/test_ptparse

.PHONY: all test asan strict clean

all: test

$(BUILD):
	@mkdir -p $(BUILD)

$(TESTBIN): $(SRC) $(TESTSRC) src/ptparse.h tests/fixture.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(TESTSRC)

test: $(TESTBIN)
	@$(TESTBIN)

# The parser handles untrusted on-disk data, so the malformed fixtures are
# only meaningful under a sanitiser: "it didn't crash" is not evidence that a
# bounds check works. Treat an asan failure as a real defect.
asan: $(SRC) $(TESTSRC) | $(BUILD)
	$(CC) $(WARN) -O1 -g -fsanitize=address,undefined \
	    -fno-omit-frame-pointer -o $(BUILD)/test_asan $(SRC) $(TESTSRC)
	@$(BUILD)/test_asan

# The Amiga cross-compilers in use are older than the host compiler, so build
# the parser alone as C89 to catch portability problems early. The tests
# themselves are host-only and not held to this.
strict: | $(BUILD)
	$(CC) -std=c89 -pedantic $(WARN) -O1 -c -o $(BUILD)/ptparse_c89.o $(SRC)
	@echo "parser compiles clean as C89"

clean:
	rm -rf $(BUILD)
