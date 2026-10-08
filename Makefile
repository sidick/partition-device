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
#   make cross      compile the parser for m68k against the real Amiga types,
#                   and report its size
#   make check      all of the above
#   make clean

CC       ?= cc
M68KCC   ?= /opt/amiga/bin/m68k-amigaos-gcc
M68KSIZE ?= /opt/amiga/bin/m68k-amigaos-size
WARN     = -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual \
           -Wstrict-prototypes -Wmissing-prototypes -Wwrite-strings
CFLAGS  ?= -O1 -g $(WARN)

BUILD    = build
SRC      = src/ptparse.c src/unitmap.c
HDRS     = src/ptparse.h src/unitmap.h

PT_TEST  = $(BUILD)/test_ptparse
UM_TEST  = $(BUILD)/test_unitmap
TESTBINS = $(PT_TEST) $(UM_TEST)

.PHONY: all test asan strict cross device check clean

all: test

$(BUILD):
	@mkdir -p $(BUILD)

$(PT_TEST): src/ptparse.c tests/test_ptparse.c tests/fixture.c \
            $(HDRS) tests/fixture.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/ptparse.c tests/test_ptparse.c tests/fixture.c

$(UM_TEST): src/unitmap.c tests/test_unitmap.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/unitmap.c tests/test_unitmap.c

test: $(TESTBINS)
	@$(PT_TEST)
	@echo
	@$(UM_TEST)

# The parser handles untrusted on-disk data, so the malformed fixtures are
# only meaningful under a sanitiser: "it didn't crash" is not evidence that a
# bounds check works. Treat an asan failure as a real defect.
asan: | $(BUILD)
	$(CC) $(WARN) -O1 -g -fsanitize=address,undefined \
	    -fno-omit-frame-pointer -o $(BUILD)/pt_asan \
	    src/ptparse.c tests/test_ptparse.c tests/fixture.c
	@$(BUILD)/pt_asan
	$(CC) $(WARN) -O1 -g -fsanitize=address,undefined \
	    -fno-omit-frame-pointer -o $(BUILD)/um_asan \
	    src/unitmap.c tests/test_unitmap.c
	@$(BUILD)/um_asan

# The Amiga cross-compilers in use are older than the host compiler, so build
# the parser alone as C89 to catch portability problems early. The tests
# themselves are host-only and not held to this.
strict: | $(BUILD)
	$(CC) -std=c89 -pedantic $(WARN) -O1 -c \
	    -o $(BUILD)/ptparse_c89.o src/ptparse.c
	$(CC) -std=c89 -pedantic $(WARN) -O1 -c \
	    -o $(BUILD)/unitmap_c89.o src/unitmap.c
	@echo "parser and unitmap compile clean as C89"

# Build the parser the way the device will: 68000, size-optimised, and with
# PTPARSE_AMIGA so it uses exec/types.h rather than stdint.h. This is the only
# build that exercises that path, and it is also where a big-endian target
# would reveal an endianness assumption the host tests cannot see.
cross: | $(BUILD)
	$(M68KCC) -mcpu=68000 -Os -fomit-frame-pointer -DPTPARSE_AMIGA \
	    $(WARN) -c -o $(BUILD)/ptparse_m68k.o src/ptparse.c
	$(M68KCC) -mcpu=68000 -Os -fomit-frame-pointer -DPTPARSE_AMIGA \
	    $(WARN) -c -o $(BUILD)/unitmap_m68k.o src/unitmap.c
	@$(M68KSIZE) $(BUILD)/ptparse_m68k.o $(BUILD)/unitmap_m68k.o

# The device itself. Linked -nostartfiles -nostdlib: a device has no startup
# code, which is also why device.c must define and set SysBase by hand.
DEVSRC = src/device.c src/config.c src/child.c src/unitio.c src/iotask.c \
         src/ptparse.c src/unitmap.c
DEVOBJ = $(DEVSRC:src/%.c=$(BUILD)/dev_%.o) $(BUILD)/endskip.o
DEVCFLAGS = -mcpu=68000 -Os -fomit-frame-pointer -DPTPARSE_AMIGA $(WARN)

$(BUILD)/dev_%.o: src/%.c $(HDRS) src/device.h | $(BUILD)
	$(M68KCC) $(DEVCFLAGS) -c -o $@ $<

$(BUILD)/endskip.o: src/endskip.S | $(BUILD)
	$(M68KCC) -c -o $@ $<

device: $(BUILD)/partunit.device

$(BUILD)/partunit.device: $(DEVOBJ)
	$(M68KCC) -nostartfiles -nostdlib -o $@ $(DEVOBJ) -lgcc -lc
	@$(M68KSIZE) $@

check: test asan strict cross device
	@echo "all checks passed"

clean:
	rm -rf $(BUILD)
