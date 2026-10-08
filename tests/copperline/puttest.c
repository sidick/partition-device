/*
 * puttest.c - on-target conformance smoke test for partunit.device.
 *
 * Cross-built for m68k, booted under Copperline from a minimal ADF, and
 * reports PASS/FAIL over serial (Copperline forwards serial to its stdout,
 * so run.sh can assert on it). Modelled on sana2loop's sana2test.c.
 *
 * This is the first thing that can catch a class of defect no host test can:
 * everything about the device's behaviour as an Exec device rather than as
 * arithmetic. The checks are therefore weighted towards "did the right
 * blocks come back" and "is the refusal real", not towards parser corners
 * already covered by 276 host checks.
 *
 * Output goes over serial via exec RawPutChar - the ROM debug path, which
 * needs no serial.device handler, no Mount and no Workbench files, so it
 * works in the most minimal boot. Copperline captures it with
 * "--serial stdout". Technique from sana2loop's sana2test.c.
 *
 * NOTE: this build uses a normal crt and so needs the bundled AROS Kickstart
 * or KS2.0+. sana2loop found that under a REAL Kickstart 1.3 a normally
 * linked program can fail to launch or Guru before reaching main, and had to
 * go fully freestanding. If partunit.device ever needs to prove 1.3 claims,
 * this harness has to go freestanding too - it is not evidence about 1.3 as
 * it stands.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <devices/trackdisk.h>
#include <devices/newstyle.h>
#include <devices/scsidisk.h>

#include <proto/exec.h>

#include <string.h>

/* ------------------------------------------------------------------ *
 * Serial output
 * ------------------------------------------------------------------ */

static void raw_putchar(UBYTE c)
{
    register UBYTE  d0 asm("d0") = c;
    register APTR   a6 asm("a6") = (APTR)SysBase;
    __asm volatile (
        "jsr     -516(%%a6)\n"      /* RawPutChar, LVO -516 */
        : : "r"(d0), "r"(a6)
        : "d1", "a0", "a1", "cc", "memory");
}

static void puts_(const char *s)
{
    while (*s != '\0') {
        raw_putchar((UBYTE)*s++);
    }
}

static void put_u32(ULONG v)
{
    char  buf[12];
    int   i = 0;

    if (v == 0) {
        raw_putchar('0');
        return;
    }
    while (v != 0 && i < 11) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i-- > 0) {
        raw_putchar((UBYTE)buf[i]);
    }
}

static void put_i32(LONG v)
{
    if (v < 0) {
        raw_putchar('-');
        put_u32((ULONG)(-v));
    } else {
        put_u32((ULONG)v);
    }
}

/* ------------------------------------------------------------------ *
 * Test bookkeeping
 * ------------------------------------------------------------------ */

static int checks;
static int failures;

static void ok(const char *name)
{
    checks++;
    puts_("  ok   ");
    puts_(name);
    puts_("\n");
}

static void fail(const char *name, LONG got, LONG want)
{
    checks++;
    failures++;
    puts_("  FAIL ");
    puts_(name);
    puts_(": got ");
    put_i32(got);
    puts_(", want ");
    put_i32(want);
    puts_("\n");
}

static void check_eq(const char *name, LONG got, LONG want)
{
    if (got == want) {
        ok(name);
    } else {
        fail(name, got, want);
    }
}

/* ------------------------------------------------------------------ *
 * The fixture pattern, matching tools/mkfixture.c exactly.
 * ------------------------------------------------------------------ */

#define FIX_BS 512

static int pattern_matches(const UBYTE *b, int part, ULONG blockno)
{
    ULONG i;

    if (b[0] != 'P' || b[1] != 'A' || b[2] != 'R' || b[3] != 'T' ||
        b[4] != 'U' || b[5] != 'N' || b[6] != 'I' || b[7] != 'T') {
        return 0;
    }
    if (b[8] != (UBYTE)part) {
        return 0;
    }
    if (b[14] != 0xA5 || b[15] != 0x5A) {
        return 0;
    }
    if ((ULONG)b[10] != (blockno & 0xFF) ||
        (ULONG)b[11] != ((blockno >> 8) & 0xFF)) {
        return 0;
    }
    for (i = 16; i < FIX_BS; i++) {
        UBYTE want = (UBYTE)((i + blockno * 7u + (ULONG)part * 31u) & 0xFF);
        if (b[i] != want) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ *
 * The tests
 * ------------------------------------------------------------------ */

static struct MsgPort  *port;
static struct IOExtTD  *io;
static struct IOStdReq *ior;
static UBYTE           *buf;

static LONG do_cmd(UWORD cmd, ULONG offset, ULONG length, APTR data)
{
    ior->io_Command = cmd;
    ior->io_Offset  = offset;
    ior->io_Actual  = 0;
    ior->io_Length  = length;
    ior->io_Data    = data;
    return (LONG)DoIO((struct IORequest *)io);
}

static void test_unit(ULONG unitnum, int part, ULONG expect_blocks)
{
    struct DriveGeometry dg;
    LONG                 err;

    puts_("unit ");
    put_u32(unitnum);
    puts_(":\n");

    if (OpenDevice((STRPTR)"partunit.device", unitnum,
                   (struct IORequest *)io, 0) != 0) {
        checks++;
        failures++;
        puts_("  FAIL open: io_Error ");
        put_i32(ior->io_Error);
        puts_("\n");
        return;
    }
    ok("open");

    /* --- geometry: the whole unit IS the partition --- */
    err = do_cmd(TD_GETGEOMETRY, 0, sizeof(dg), &dg);
    check_eq("TD_GETGEOMETRY", err, 0);
    if (err == 0) {
        check_eq("  dg_SectorSize", (LONG)dg.dg_SectorSize, FIX_BS);
        check_eq("  dg_TotalSectors", (LONG)dg.dg_TotalSectors,
                 (LONG)expect_blocks);
        check_eq("  dg_Heads", (LONG)dg.dg_Heads, 1);
        check_eq("  dg_TrackSectors", (LONG)dg.dg_TrackSectors, 1);
        check_eq("  dg_Cylinders", (LONG)dg.dg_Cylinders, (LONG)expect_blocks);
    }

    /* --- TD_CHANGENUM must be non-zero, or every ETD_* request fails --- */
    err = do_cmd(TD_CHANGENUM, 0, 0, NULL);
    check_eq("TD_CHANGENUM ok", err, 0);
    if (ior->io_Actual == 0) {
        fail("TD_CHANGENUM nonzero", 0, 1);
    } else {
        ok("TD_CHANGENUM nonzero");
    }

    /* --- the real question: did we read the RIGHT blocks? --- */
    err = do_cmd(CMD_READ, 0, FIX_BS, buf);
    check_eq("CMD_READ block 0", err, 0);
    if (err == 0) {
        check_eq("  io_Actual", (LONG)ior->io_Actual, FIX_BS);
        if (pattern_matches(buf, part, 0)) {
            ok("  block 0 pattern");
        } else {
            fail("  block 0 pattern", 0, 1);
        }
    }

    /* A block well inside the partition, to catch a constant offset error
     * that block 0 alone would not. */
    err = do_cmd(CMD_READ, 7 * FIX_BS, FIX_BS, buf);
    check_eq("CMD_READ block 7", err, 0);
    if (err == 0 && pattern_matches(buf, part, 7)) {
        ok("  block 7 pattern");
    } else if (err == 0) {
        fail("  block 7 pattern", 0, 1);
    }

    /* The last block: proves the length, and that we are not one short. */
    err = do_cmd(CMD_READ, (expect_blocks - 1) * FIX_BS, FIX_BS, buf);
    check_eq("CMD_READ last block", err, 0);
    if (err == 0 && pattern_matches(buf, part, expect_blocks - 1)) {
        ok("  last block pattern");
    } else if (err == 0) {
        fail("  last block pattern", 0, 1);
    }

    /* --- bounds: the defining safety property --- */
    err = do_cmd(CMD_READ, expect_blocks * FIX_BS, FIX_BS, buf);
    check_eq("read one past end refused", err, IOERR_BADADDRESS);

    err = do_cmd(CMD_READ, (expect_blocks - 1) * FIX_BS, 2 * FIX_BS, buf);
    check_eq("read straddling end refused", err, IOERR_BADADDRESS);

    /* A gross overshoot, but with a length the BUFFER can hold: if the
     * device wrongly accepts it we must get a clean failure, not memory
     * corruption. An earlier version asked for (blocks+1) * 512 bytes into a
     * 2KB buffer and, when a wrong expectation made the device accept it,
     * scribbled over the heap and hung the machine - the test must not be
     * able to do more damage than the bug it is looking for. */
    err = do_cmd(CMD_READ, (expect_blocks + 1000) * FIX_BS, FIX_BS, buf);
    check_eq("read far past end refused", err, IOERR_BADADDRESS);

    err = do_cmd(CMD_READ, 0, FIX_BS - 1, buf);
    check_eq("part-block length refused", err, IOERR_BADLENGTH);

    /* --- ETD_*: devtest's strictest requirement --- */
    {
        ULONG change;

        do_cmd(TD_CHANGENUM, 0, 0, NULL);
        change = ior->io_Actual;

        ior->io_Command = ETD_READ;
        ior->io_Offset  = 0;
        ior->io_Actual  = 0;
        ior->io_Length  = FIX_BS;
        ior->io_Data    = buf;
        io->iotd_Count  = 0;            /* stale: must be rejected */
        err = (LONG)DoIO((struct IORequest *)io);
        check_eq("ETD_READ count 0 rejected", err, TDERR_DiskChanged);

        ior->io_Command = ETD_READ;
        ior->io_Offset  = 0;
        ior->io_Actual  = 0;
        ior->io_Length  = FIX_BS;
        ior->io_Data    = buf;
        io->iotd_Count  = change;       /* current: must succeed */
        err = (LONG)DoIO((struct IORequest *)io);
        check_eq("ETD_READ current count ok", err, 0);
        if (err == 0 && pattern_matches(buf, part, 0)) {
            ok("  ETD_READ pattern");
        } else if (err == 0) {
            fail("  ETD_READ pattern", 0, 1);
        }

        ior->io_Command = ETD_READ;
        ior->io_Offset  = 0;
        ior->io_Actual  = 0;
        ior->io_Length  = FIX_BS;
        ior->io_Data    = buf;
        io->iotd_Count  = 0xFFFFFFFFUL; /* documented "don't care" */
        err = (LONG)DoIO((struct IORequest *)io);
        check_eq("ETD_READ 0xFFFFFFFF ok", err, 0);
    }

    /* --- NSD --- */
    {
        struct NSDeviceQueryResult *q =
            AllocMem(256, MEMF_PUBLIC | MEMF_CLEAR);
        if (q != NULL) {
            q->nsdqr_DevQueryFormat = 0;
            q->nsdqr_SizeAvailable  = 0;
            err = do_cmd(NSCMD_DEVICEQUERY, 0, 256, q);
            check_eq("NSCMD_DEVICEQUERY", err, 0);
            if (err == 0) {
                check_eq("  DevQueryFormat", (LONG)q->nsdqr_DevQueryFormat, 0);
                check_eq("  DeviceType", (LONG)q->nsdqr_DeviceType,
                         NSDEVTYPE_TRACKDISK);
                /* Must be the bytes written, not our io_Length. */
                check_eq("  SizeAvailable", (LONG)q->nsdqr_SizeAvailable,
                         (LONG)sizeof(struct NSDeviceQueryResult));
            }
            FreeMem(q, 256);
        }
    }

    /*
     * --- HD_SCSICMD READ CAPACITY(10) ---
     *
     * Must report the PARTITION's size, not the drive's. devtest's -g run
     * showed this coming back as 0 sectors, and devtest -i then failed with
     * "Invalid transfer size" - so this is checked directly here, where the
     * expected numbers are known exactly.
     */
    {
        struct SCSICmd  sc;
        UBYTE           cdb[10];
        UBYTE           cap[8];
        UBYTE           sense[32];
        ULONG           last;
        ULONG           bs;

        memset(&sc, 0, sizeof(sc));
        memset(cdb, 0, sizeof(cdb));
        memset(cap, 0xCC, sizeof(cap));     /* poison, so 0 means written */
        cdb[0] = 0x25;                      /* READ CAPACITY(10) */

        sc.scsi_Data        = (UWORD *)cap;
        sc.scsi_Length      = sizeof(cap);
        sc.scsi_Command     = cdb;
        sc.scsi_CmdLength   = sizeof(cdb);
        sc.scsi_Flags       = SCSIF_READ | SCSIF_AUTOSENSE;
        sc.scsi_SenseData   = sense;
        sc.scsi_SenseLength = sizeof(sense);

        err = do_cmd(HD_SCSICMD, 0, sizeof(struct SCSICmd), &sc);
        check_eq("HD_SCSICMD READ CAPACITY(10)", err, 0);
        if (err == 0) {
            last = ((ULONG)cap[0] << 24) | ((ULONG)cap[1] << 16) |
                   ((ULONG)cap[2] << 8)  |  (ULONG)cap[3];
            bs   = ((ULONG)cap[4] << 24) | ((ULONG)cap[5] << 16) |
                   ((ULONG)cap[6] << 8)  |  (ULONG)cap[7];
            puts_("    capacity last=");
            put_u32(last);
            puts_(" bs=");
            put_u32(bs);
            puts_(" scsi_Actual=");
            put_u32(sc.scsi_Actual);
            puts_("\n");
            check_eq("  capacity last LBA", (LONG)last,
                     (LONG)(expect_blocks - 1));
            check_eq("  capacity block size", (LONG)bs, FIX_BS);
        }
    }

    /*
     * --- HD_SCSICMD INQUIRY and MODE SENSE must describe THIS unit ---
     *
     * Both were originally forwarded to the child as "non-addressing"
     * commands, and both then described the underlying drive: devtest showed
     * a partition reporting the drive's vendor string, and mode pages 0x03
     * and 0x04 reporting the drive's sectors-per-track and cylinder/head
     * counts - flatly contradicting TD_GETGEOMETRY for the same unit. A
     * caller that believes those pages computes addresses for the wrong
     * disk, so this is checked here rather than left to inspection.
     */
    {
        struct SCSICmd  sc;
        UBYTE           cdb[10];
        UBYTE           rep[64];
        UBYTE           sense[32];

        /* INQUIRY: our identity, not the child's. */
        memset(&sc, 0, sizeof(sc));
        memset(cdb, 0, sizeof(cdb));
        memset(rep, 0xCC, sizeof(rep));
        cdb[0] = 0x12;
        cdb[4] = 36;
        sc.scsi_Data        = (UWORD *)rep;
        sc.scsi_Length      = 36;
        sc.scsi_Command     = cdb;
        sc.scsi_CmdLength   = 6;
        sc.scsi_Flags       = SCSIF_READ | SCSIF_AUTOSENSE;
        sc.scsi_SenseData   = sense;
        sc.scsi_SenseLength = sizeof(sense);

        err = do_cmd(HD_SCSICMD, 0, sizeof(struct SCSICmd), &sc);
        check_eq("HD_SCSICMD INQUIRY", err, 0);
        if (err == 0) {
            check_eq("  peripheral type", (LONG)rep[0], 0);
            if (memcmp(rep + 8, "PARTUNIT", 8) == 0) {
                ok("  vendor is ours, not the child's");
            } else {
                fail("  vendor is ours, not the child's", 0, 1);
            }
        }

        /* MODE SENSE page 0x04: geometry must match TD_GETGEOMETRY. */
        memset(&sc, 0, sizeof(sc));
        memset(cdb, 0, sizeof(cdb));
        memset(rep, 0xCC, sizeof(rep));
        cdb[0] = 0x1A;
        cdb[1] = 0x08;          /* DBD: no block descriptor */
        cdb[2] = 0x04;          /* Rigid Drive Geometry */
        cdb[4] = sizeof(rep);
        sc.scsi_Data        = (UWORD *)rep;
        sc.scsi_Length      = sizeof(rep);
        sc.scsi_Command     = cdb;
        sc.scsi_CmdLength   = 6;
        sc.scsi_Flags       = SCSIF_READ | SCSIF_AUTOSENSE;
        sc.scsi_SenseData   = sense;
        sc.scsi_SenseLength = sizeof(sense);

        err = do_cmd(HD_SCSICMD, 0, sizeof(struct SCSICmd), &sc);
        check_eq("HD_SCSICMD MODE SENSE 0x04", err, 0);
        if (err == 0) {
            ULONG cyl;
            check_eq("  block desc len", (LONG)rep[3], 0);
            check_eq("  page code", (LONG)(rep[4] & 0x3F), 0x04);
            cyl = ((ULONG)rep[6] << 16) | ((ULONG)rep[7] << 8) |
                   (ULONG)rep[8];
            check_eq("  cylinders match geometry", (LONG)cyl,
                     (LONG)expect_blocks);
            check_eq("  heads match geometry", (LONG)rep[9], 1);
        }

        /* A page we do not synthesise must be refused, not forwarded -
         * otherwise the child's answer leaks through this command. */
        memset(&sc, 0, sizeof(sc));
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x1A;
        cdb[1] = 0x08;
        cdb[2] = 0x01;          /* Read-Write Error Recovery: not ours */
        cdb[4] = sizeof(rep);
        sc.scsi_Data        = (UWORD *)rep;
        sc.scsi_Length      = sizeof(rep);
        sc.scsi_Command     = cdb;
        sc.scsi_CmdLength   = 6;
        sc.scsi_Flags       = SCSIF_READ | SCSIF_AUTOSENSE;
        sc.scsi_SenseData   = sense;
        sc.scsi_SenseLength = sizeof(sense);

        err = do_cmd(HD_SCSICMD, 0, sizeof(struct SCSICmd), &sc);
        check_eq("unsynthesised mode page refused", err, HFERR_BadStatus);
    }

    /* --- status commands report in io_Actual, never io_Error --- */
    err = do_cmd(TD_CHANGESTATE, 0, 0, NULL);
    check_eq("TD_CHANGESTATE ok", err, 0);
    check_eq("  media present", (LONG)ior->io_Actual, 0);

    err = do_cmd(TD_PROTSTATUS, 0, 0, NULL);
    check_eq("TD_PROTSTATUS ok", err, 0);

    err = do_cmd(CMD_UPDATE, 0, 0, NULL);
    check_eq("CMD_UPDATE ok", err, 0);

    CloseDevice((struct IORequest *)io);
    ok("close");
}

/*
 * A unit number that must not exist. The answer matters: anything other
 * than TDERR_BadUnitNum makes a unit-walking scanner (HDToolbox, and our
 * own future tool) stop at the first gap, and our x100 numbering is full of
 * gaps by construction.
 */
static void test_absent_unit(ULONG unitnum)
{
    LONG err;

    puts_("absent unit ");
    put_u32(unitnum);
    puts_(":\n");

    err = (LONG)OpenDevice((STRPTR)"partunit.device", unitnum,
                           (struct IORequest *)io, 0);
    if (err == 0) {
        fail("  should not have opened", 0, 1);
        CloseDevice((struct IORequest *)io);
        return;
    }
    check_eq("  TDERR_BadUnitNum", (LONG)ior->io_Error, TDERR_BadUnitNum);
}

int main(void)
{
    puts_("\npartunit.device on-target test\n\n");

    port = CreateMsgPort();
    if (port == NULL) {
        puts_("RESULT=FAIL (no msgport)\n");
        return 20;
    }
    io = (struct IOExtTD *)CreateIORequest(port, sizeof(struct IOExtTD));
    if (io == NULL) {
        puts_("RESULT=FAIL (no ioreq)\n");
        return 20;
    }
    ior = &io->iotd_Req;

    buf = AllocMem(FIX_BS * 4, MEMF_PUBLIC | MEMF_CLEAR);
    if (buf == NULL) {
        puts_("RESULT=FAIL (no buffer)\n");
        return 20;
    }

    /*
     * Disk 0, partitions 0 and 1 - the two Amiga partitions of the fixture.
     * The FAT/EFI decoy must NOT have become a unit, which is what makes
     * these the first two and not the second and third.
     */
    /*
     * Units are numbered by TABLE SLOT, not by "nth Amiga partition":
     * um_unit_number(disk, i) uses the partition's index within its own
     * table. So the fixture's MBR slot 1 (0x76, 1024 blocks) is unit 1 and
     * slot 2 (0x30, 512 blocks) is unit 2 - and slot 0, the FAT decoy, has
     * no unit at all.
     *
     * That is the more stable choice: a slot index does not shift when a
     * neighbouring partition changes type, whereas "nth Amiga partition"
     * would renumber everything after it.
     */
    test_unit(1, 1, 1024);
    test_unit(2, 2, 512);

    /* Unit 0 is the FAT decoy's slot: it must NOT have become a unit. That
     * is the whole protect-the-PC-side property, checked directly. */
    test_absent_unit(0);

    /* Gaps in our own numbering, and a disk that does not exist. */
    test_absent_unit(3);
    test_absent_unit(50);
    test_absent_unit(100);

    puts_("\n");
    put_u32((ULONG)checks);
    puts_(" checks, ");
    put_u32((ULONG)failures);
    puts_(" failures\n");
    puts_(failures == 0 ? "RESULT=PASS\n" : "RESULT=FAIL\n");

    FreeMem(buf, FIX_BS * 4);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    return failures == 0 ? 0 : 20;
}
