/*
 * devtestrun.c - run Chris Hooper's devtest against partunit.device under
 *                Copperline and get its output off the machine.
 *
 * devtest prints with printf, and a headless minimal boot has no console we
 * can read. So this wrapper runs each devtest mode with its output
 * redirected to a file, then reads the file back and echoes it over serial
 * via RawPutChar, which Copperline forwards to its stdout. That gives the
 * host both the full report and each mode's exit code, so run-devtest.sh can
 * assert rather than eyeball.
 *
 * The modes and why these: per docs/proposal.md, "devtest green" is NOT a
 * meaningful criterion - devtest -t deliberately discards individual test
 * failures and always exits 0 ("no driver passes all tests"). Only -p, -g
 * and -i return a usable exit code, so those three are the gate; -t is
 * captured for review but its exit code proves nothing.
 *
 * Note -t is run WITHOUT -d. devtest's destructive modes write from offset 0
 * with only partial backup, and these units are real partitions.
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

/* SYS: rather than RAM: - the boot volume is certainly there and is
 * writable, and the log survives for xdftool to extract if serial capture
 * ever disappoints. */
#define LOGFILE "SYS:devtest.log"
#define UNIT    "1"

static void putc_(UBYTE c)
{
    register UBYTE d0 asm("d0") = c;
    register APTR  a6 asm("a6") = (APTR)SysBase;
    __asm volatile ("jsr -516(%%a6)" : : "r"(d0), "r"(a6)
                    : "d1", "a0", "a1", "cc", "memory");
}

static void emit(const char *s)
{
    while (*s != '\0') {
        putc_((UBYTE)*s++);
    }
}

static void emit_i32(LONG v)
{
    char buf[12];
    int  i = 0;

    if (v < 0) {
        putc_('-');
        v = -v;
    }
    if (v == 0) {
        putc_('0');
        return;
    }
    while (v != 0 && i < 11) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i-- > 0) {
        putc_((UBYTE)buf[i]);
    }
}

/* Echo the captured log to serial, normalising line ends. */
static void dump_log(void)
{
    BPTR  fh;
    char *buf;
    LONG  got;

    fh = Open((STRPTR)LOGFILE, MODE_OLDFILE);
    if (fh == 0) {
        emit("  (no output captured)\n");
        return;
    }
    buf = AllocVec(4096, 0);
    if (buf == NULL) {
        Close(fh);
        return;
    }
    while ((got = Read(fh, buf, 4095)) > 0) {
        LONG i;
        for (i = 0; i < got; i++) {
            if (buf[i] != '\r') {
                putc_((UBYTE)buf[i]);
            }
        }
    }
    FreeVec(buf);
    Close(fh);
}

/*
 * Run one devtest mode. Returns its exit code, or -1 if it could not be
 * launched at all (which is a harness failure, not a device failure, and
 * must not be mistaken for a pass).
 */
static LONG run_mode(const char *flag)
{
    char  cmd[128];
    BPTR  out;
    LONG  rc;

    /*
     * "devtest <args> partunit.device 1".
     *
     * Note -i takes a MANDATORY transfer size: its usage is
     * "-i <tsize>[,<align>]". Passing a bare -i makes devtest parse the
     * device name as the size and fail with "Invalid transfer size
     * partunit.device" - which looks like a device failure and is not one.
     */
    cmd[0] = '\0';
    strcat(cmd, "devtest ");
    strcat(cmd, flag);
    strcat(cmd, " partunit.device " UNIT);

    emit("\n=== devtest ");
    emit(flag);
    emit(" ===\n");

    out = Open((STRPTR)LOGFILE, MODE_NEWFILE);
    if (out == 0) {
        emit("  harness: cannot open " LOGFILE "\n");
        return -1;
    }

    /*
     * SYS_Input is NULL so devtest cannot block waiting on a console that
     * is not there.
     *
     * SystemTags only closes SYS_Output/SYS_Input when SYS_Asynch is TRUE.
     * Synchronously, closing them is OUR job - and until it is closed
     * nothing is flushed to the file and the next MODE_NEWFILE on the same
     * name fails. Both symptoms showed up on the first run: an empty
     * capture for the first mode and "cannot open" for every one after it.
     */
    rc = SystemTags((STRPTR)cmd,
                    SYS_Output, (ULONG)out,
                    SYS_Input,  (ULONG)0,
                    SYS_Asynch, (ULONG)FALSE,
                    TAG_END);
    Close(out);

    if (rc != 0) {
        emit("  (devtest exited nonzero; IoErr=");
        emit_i32(IoErr());
        emit(")\n");
    }
    dump_log();

    emit("RC=");
    emit_i32(rc);
    emit("\n");
    return rc;
}

int main(void)
{
    LONG rc_p, rc_g, rc_i;
    int  bad = 0;

    emit("\ndevtest against partunit.device unit " UNIT "\n");

    /*
     * -p probe, -g geometry, -i integrity: the three that return a usable
     * exit code. -i needs an explicit transfer size (8k here). -g is the one that needs MODE SENSE(6) to work, since it
     * ends by calling scsi_read_mode_pages() and returns that result
     * unchanged. -i without -d is read/read/compare, non-destructive.
     */
    rc_p = run_mode("-p");
    rc_g = run_mode("-g");
    rc_i = run_mode("-i 8k");

    /* -t for review only: it always exits 0 by design, so its code is not
     * part of the gate. The host greps its captured output instead. */
    (void)run_mode("-t");

    emit("\nsummary: -p RC=");
    emit_i32(rc_p);
    emit(", -g RC=");
    emit_i32(rc_g);
    emit(", -i RC=");
    emit_i32(rc_i);
    emit("\n");

    if (rc_p != 0) { bad = 1; }
    if (rc_g != 0) { bad = 1; }
    if (rc_i != 0) { bad = 1; }

    emit(bad ? "RESULT=FAIL\n" : "RESULT=PASS\n");
    return bad ? 20 : 0;
}
