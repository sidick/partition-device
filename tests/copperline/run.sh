#!/bin/sh
# run.sh -- headless Copperline on-target smoke test for partunit.device.
#
# Builds a minimal bootable ADF (xdftool) holding DEVS:partunit.device,
# C:puttest and a one-line Startup-Sequence, attaches a partition-table
# fixture disk as SCSI unit 0, and boots. puttest opens partunit.device's
# units, checks geometry, verifies it read the RIGHT blocks, and asserts the
# bounds refusals; it reports over serial, which Copperline forwards to its
# stdout. We assert on RESULT=.
#
# Modelled on sana2loop's tests/copperline/run.sh.
#
# The fixture has NO RDB anywhere, so the OS mounts nothing from it. Every
# unit puttest sees exists because partunit.device created it - which is what
# makes a pass meaningful rather than incidental.
#
# Boots the BUNDLED AROS Kickstart. puttest uses a normal crt, which
# sana2loop found can fail to launch or Guru before main under a REAL
# Kickstart 1.3; this test is therefore not evidence about 1.3. Making it so
# means going freestanding, as sana2test.c had to.
#
# Prereqs:
#   - xdftool on PATH (amitools: pip install amitools), or XDFTOOL=...
#   - copperline on PATH, or COPPERLINE=...
#   - a built device and test binary (make device && make puttest)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

COPPERLINE=${COPPERLINE:-copperline}
XDFTOOL=${XDFTOOL:-xdftool}
DEVICE=${PARTUNIT_DEVICE:-$ROOT/build/partunit.device}
BIN=${PUTTEST_M68K:-$ROOT/build/puttest}
MKFIXTURE=${MKFIXTURE:-$ROOT/build/mkfixture}
LAYOUT=${LAYOUT:-mbr}
# Emulated-second budget. Generous: AROS boots slowly and the device's own
# config read is real DOS I/O.
BENCH=${BENCH:-40}

for f in "$DEVICE" "$BIN" "$MKFIXTURE"; do
    [ -f "$f" ] || { echo "run.sh: missing $f (run 'make device puttest mkfixture')" >&2; exit 1; }
done
command -v "$XDFTOOL" >/dev/null || { echo "run.sh: xdftool not found" >&2; exit 1; }
command -v "$COPPERLINE" >/dev/null || { echo "run.sh: copperline not found" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "run.sh: building $LAYOUT fixture"
"$MKFIXTURE" "$LAYOUT" "$WORK/fixture.hdf"

# The device reads ENV:partunit/config, then S:partunit/config. Ship it on
# the floppy as the S: copy: visible in the image for debugging, and no
# quoting to get wrong at boot.
printf 'DISK scsi.device 0\n' > "$WORK/config"

# No "assign ENV:" here: a minimal ADF has no C: commands to run one with.
# The device falls back to S:partunit/config for exactly this case.
printf '; partunit.device Copperline smoke boot\nputtest\n' > "$WORK/startup"

echo "run.sh: building boot.adf"
"$XDFTOOL" "$WORK/boot.adf" format PARTUNIT ofs + \
    boot install boot1x + \
    makedir devs + makedir c + makedir s + makedir s/partunit + \
    write "$DEVICE" devs/partunit.device + \
    write "$BIN" c/puttest + \
    write "$WORK/startup" s/startup-sequence + \
    write "$WORK/config" s/partunit/config \
    >/dev/null

cp "$WORK/boot.adf" "$HERE/boot.adf"
cp "$WORK/fixture.hdf" "$HERE/fixture.hdf"

echo "run.sh: booting (budget ${BENCH}s emulated)"
LOG="$WORK/serial.log"
set +e
(cd "$HERE" && "$COPPERLINE" --config machine.toml \
    --noaudio --serial stdout --benchmark-until "$BENCH") > "$LOG" 2>&1
RC=$?
set -e

echo "----- guest serial output -----"
cat "$LOG"
echo "-------------------------------"

if ! grep -q "RESULT=" "$LOG"; then
    echo "run.sh: FAIL - guest never reported a result (copperline rc=$RC)" >&2
    echo "run.sh: the test did not reach its end; raise BENCH= or check the boot" >&2
    exit 1
fi
if grep -q "RESULT=PASS" "$LOG"; then
    echo "run.sh: PASS"
    exit 0
fi
echo "run.sh: FAIL - guest reported failure" >&2
exit 1
