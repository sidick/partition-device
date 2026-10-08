#!/bin/sh
# run-devtest.sh -- run Chris Hooper's devtest against partunit.device under
# headless Copperline.
#
# Separate from run.sh because devtest is a third-party binary with its own
# build, and because its own semantics need explaining: "devtest green" is
# not a thing. devtest -t deliberately discards individual test failures and
# always exits 0 ("no driver passes all tests" - devtest.c:4894-4901), so the
# gate here is -p, -g and -i exiting zero, plus a reviewed -t report. See
# docs/proposal.md's testing section.
#
# devtest is NOT in this repo. Point DEVTEST_BIN at a cross-built copy:
#   git clone https://github.com/cdhooper/amiga_devtest
#   cd amiga_devtest && m68k-amigaos-gcc -Os -Wno-pointer-sign \
#       -Wno-strict-aliasing -DVER='"1.9a+"' -o devtest devtest.c -lamiga
# (its own Makefile asks for -mcrt=clib2, which this /opt/amiga toolchain
# does not have; the default crt builds it fine.)
#
# Never run devtest -d, -bd or -i -d here: they write from offset 0 with only
# partial backup, and these units are real partitions.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

COPPERLINE=${COPPERLINE:-copperline}
XDFTOOL=${XDFTOOL:-xdftool}
DEVICE=${PARTUNIT_DEVICE:-$ROOT/build/partunit.device}
RUNNER=${DEVTESTRUN_M68K:-$ROOT/build/devtestrun}
DEVTEST_BIN=${DEVTEST_BIN:-$ROOT/build/devtest}
MKFIXTURE=${MKFIXTURE:-$ROOT/build/mkfixture}
LAYOUT=${LAYOUT:-mbr}
# devtest -i reads the whole unit and -t exercises a lot of commands, so this
# needs a far larger budget than the puttest smoke run.
BENCH=${BENCH:-180}

for f in "$DEVICE" "$RUNNER" "$MKFIXTURE"; do
    [ -f "$f" ] || { echo "run-devtest.sh: missing $f (run 'make device devtestrun mkfixture')" >&2; exit 1; }
done
if [ ! -f "$DEVTEST_BIN" ]; then
    echo "run-devtest.sh: no devtest binary at $DEVTEST_BIN" >&2
    echo "run-devtest.sh: see this script's header for how to build one" >&2
    exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "run-devtest.sh: building $LAYOUT fixture"
"$MKFIXTURE" "$LAYOUT" "$WORK/fixture.hdf"

printf 'DISK scsi.device 0\n' > "$WORK/config"
printf '; partunit.device devtest run\ndevtestrun\n' > "$WORK/startup"

echo "run-devtest.sh: building boot.adf"
"$XDFTOOL" "$WORK/boot.adf" format PARTUNIT ofs + \
    boot install boot1x + \
    makedir devs + makedir c + makedir s + makedir s/partunit + \
    write "$DEVICE" devs/partunit.device + \
    write "$RUNNER" c/devtestrun + \
    write "$DEVTEST_BIN" c/devtest + \
    write "$WORK/startup" s/startup-sequence + \
    write "$WORK/config" s/partunit/config \
    >/dev/null

cp "$WORK/boot.adf" "$HERE/boot-devtest.adf"
cp "$WORK/fixture.hdf" "$HERE/fixture.hdf"

echo "run-devtest.sh: booting (budget ${BENCH}s emulated)"
LOG="$WORK/serial.log"
set +e
(cd "$HERE" && "$COPPERLINE" --config machine-devtest.toml \
    --noaudio --serial stdout --benchmark-until "$BENCH") > "$LOG" 2>&1
set -e

# Strip Copperline's own logging so what remains is the guest's report.
grep -vE '^\[[0-9]{4}-' "$LOG" > "$LOG.guest" || true

echo "----- devtest report -----"
cat "$LOG.guest"
echo "--------------------------"

if ! grep -q "RESULT=" "$LOG.guest"; then
    echo "run-devtest.sh: FAIL - guest never reported (raise BENCH=?)" >&2
    exit 1
fi
if ! grep -q "RESULT=PASS" "$LOG.guest"; then
    echo "run-devtest.sh: FAIL - one of -p/-g/-i returned nonzero" >&2
    exit 1
fi
echo "run-devtest.sh: PASS (-p, -g and -i all exited 0)"
echo "run-devtest.sh: NOTE -t's exit code proves nothing by design; review"
echo "run-devtest.sh:      its report above for unexpected 'Fail' lines."
exit 0
