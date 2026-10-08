/*
 * unitio.c - request processing, running on the owning disk's task.
 *
 * BeginIO answers the immediate commands itself (see device.c) and queues
 * everything else here. Every path ends by setting io_Error and replying.
 *
 * No address arithmetic happens in this file: um_translate owns that, so
 * there is exactly one place where bounds are decided. In particular the
 * HD_SCSICMD path below rewrites its LBA through um_translate_scsi rather
 * than computing anything itself - bounds enforcement must not have a
 * SCSI-direct back door, and the way that guarantee gets lost is by having
 * two implementations that drift apart.
 */

#include "device.h"

#include <devices/newstyle.h>
#include <devices/scsidisk.h>
#include <exec/errors.h>

#include <proto/exec.h>

#include <string.h>

#ifndef NSCMD_ETD_READ64
# define NSCMD_ETD_READ64   0xe000
# define NSCMD_ETD_WRITE64  0xe001
# define NSCMD_ETD_SEEK64   0xe002
# define NSCMD_ETD_FORMAT64 0xe003
#endif

/* SCSI opcodes we are prepared to deal with. */
#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_READ_6           0x08
#define SCSI_WRITE_6          0x0A
#define SCSI_INQUIRY          0x12
#define SCSI_MODE_SENSE_6     0x1A
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10          0x28
#define SCSI_WRITE_10         0x2A
#define SCSI_READ_12          0xA8
#define SCSI_WRITE_12         0xAA
#define SCSI_READ_16          0x88
#define SCSI_WRITE_16         0x8A
#define SCSI_READ_CAPACITY_16 0x9E

/* ------------------------------------------------------------------ *
 * Data transfer
 * ------------------------------------------------------------------ */

/*
 * Assemble the 64-bit byte offset from the request.
 *
 * io_Offset is the low 32 bits, io_Actual the high 32 - the NDK defines
 * io_HighOffset as a literal alias for io_Actual. The 32-bit commands have
 * had io_Actual zeroed by BeginIO before they reach us, because callers leave
 * it dirty; lide's own mounter is one such caller.
 */
static pt_u64 req_offset(const struct IOStdReq *ior, int is_64)
{
    if (is_64) {
        return ((pt_u64)ior->io_Actual << 32) | (pt_u64)ior->io_Offset;
    }
    return (pt_u64)ior->io_Offset;
}

static LONG do_transfer(struct PUDisk *pd, struct PUUnit *pu,
                        struct IOStdReq *ior, int is_write, int is_64)
{
    um_xfer x;
    pt_u64  off = req_offset(ior, is_64);
    LONG    err;
    ULONG   actual = 0;

    err = (LONG)um_translate(&pu->pu_Map, off, ior->io_Length, is_write, &x);
    if (err != UM_OK) {
        ior->io_Actual = 0;
        return err;
    }

    err = pu_child_rw(pd, is_write, x.abs_byte_off, ior->io_Length,
                      ior->io_Data, &actual, x.needs_64bit);
    if (err != 0) {
        ior->io_Actual = 0;
        return err;
    }
    ior->io_Actual = actual != 0 ? actual : ior->io_Length;
    return 0;
}

/* ------------------------------------------------------------------ *
 * SCSI-direct
 * ------------------------------------------------------------------ */

static pt_u64 cdb_lba(const UBYTE *c, UBYTE op, ULONG *blocks)
{
    switch (op) {
    case SCSI_READ_6:
    case SCSI_WRITE_6:
        *blocks = c[4] != 0 ? (ULONG)c[4] : 256;   /* 0 means 256 */
        return (pt_u64)(((ULONG)(c[1] & 0x1F) << 16) |
                        ((ULONG)c[2] << 8) | (ULONG)c[3]);
    case SCSI_READ_10:
    case SCSI_WRITE_10:
        *blocks = ((ULONG)c[7] << 8) | (ULONG)c[8];
        return (pt_u64)(((ULONG)c[2] << 24) | ((ULONG)c[3] << 16) |
                        ((ULONG)c[4] << 8) | (ULONG)c[5]);
    case SCSI_READ_12:
    case SCSI_WRITE_12:
        *blocks = ((ULONG)c[6] << 24) | ((ULONG)c[7] << 16) |
                  ((ULONG)c[8] << 8) | (ULONG)c[9];
        return (pt_u64)(((ULONG)c[2] << 24) | ((ULONG)c[3] << 16) |
                        ((ULONG)c[4] << 8) | (ULONG)c[5]);
    case SCSI_READ_16:
    case SCSI_WRITE_16:
        *blocks = ((ULONG)c[10] << 24) | ((ULONG)c[11] << 16) |
                  ((ULONG)c[12] << 8) | (ULONG)c[13];
        return ((pt_u64)c[2] << 56) | ((pt_u64)c[3] << 48) |
               ((pt_u64)c[4] << 40) | ((pt_u64)c[5] << 32) |
               ((pt_u64)c[6] << 24) | ((pt_u64)c[7] << 16) |
               ((pt_u64)c[8] << 8)  |  (pt_u64)c[9];
    default:
        *blocks = 0;
        return 0;
    }
}

static void cdb_set_lba(UBYTE *c, UBYTE op, pt_u64 lba)
{
    switch (op) {
    case SCSI_READ_6:
    case SCSI_WRITE_6:
        c[1] = (UBYTE)((c[1] & 0xE0) | ((lba >> 16) & 0x1F));
        c[2] = (UBYTE)((lba >> 8) & 0xFF);
        c[3] = (UBYTE)(lba & 0xFF);
        break;
    case SCSI_READ_10:
    case SCSI_WRITE_10:
    case SCSI_READ_12:
    case SCSI_WRITE_12:
        c[2] = (UBYTE)((lba >> 24) & 0xFF);
        c[3] = (UBYTE)((lba >> 16) & 0xFF);
        c[4] = (UBYTE)((lba >> 8) & 0xFF);
        c[5] = (UBYTE)(lba & 0xFF);
        break;
    case SCSI_READ_16:
    case SCSI_WRITE_16:
        c[2] = (UBYTE)((lba >> 56) & 0xFF);
        c[3] = (UBYTE)((lba >> 48) & 0xFF);
        c[4] = (UBYTE)((lba >> 40) & 0xFF);
        c[5] = (UBYTE)((lba >> 32) & 0xFF);
        c[6] = (UBYTE)((lba >> 24) & 0xFF);
        c[7] = (UBYTE)((lba >> 16) & 0xFF);
        c[8] = (UBYTE)((lba >> 8) & 0xFF);
        c[9] = (UBYTE)(lba & 0xFF);
        break;
    default:
        break;
    }
}

/* Synthesise READ CAPACITY so it reports the PARTITION's size.
 *
 * Not cosmetic: devtest does not offset SCSI LBAs by its own partition
 * notion, so a drive-sized capacity would disagree wildly with
 * TD_GETGEOMETRY and make its "Read-to capacity" probe stop at our partition
 * end. Emu68 reports su_BlockCount - 1 for the same reason. */
static LONG scsi_read_capacity(struct PUUnit *pu, struct SCSICmd *cmd,
                               int is_16)
{
    UBYTE *d = (UBYTE *)cmd->scsi_Data;
    pt_u64 last = pu->pu_Map.block_count - 1;
    ULONG  bs   = pu->pu_Map.block_size;

    if (d == NULL) {
        return IOERR_BADADDRESS;
    }

    if (is_16) {
        if (cmd->scsi_Length < 12) {
            return IOERR_BADLENGTH;
        }
        memset(d, 0, 12);
        d[0] = (UBYTE)((last >> 56) & 0xFF);
        d[1] = (UBYTE)((last >> 48) & 0xFF);
        d[2] = (UBYTE)((last >> 40) & 0xFF);
        d[3] = (UBYTE)((last >> 32) & 0xFF);
        d[4] = (UBYTE)((last >> 24) & 0xFF);
        d[5] = (UBYTE)((last >> 16) & 0xFF);
        d[6] = (UBYTE)((last >> 8) & 0xFF);
        d[7] = (UBYTE)(last & 0xFF);
        d[8]  = (UBYTE)((bs >> 24) & 0xFF);
        d[9]  = (UBYTE)((bs >> 16) & 0xFF);
        d[10] = (UBYTE)((bs >> 8) & 0xFF);
        d[11] = (UBYTE)(bs & 0xFF);
        cmd->scsi_Actual = 12;
    } else {
        ULONG l32 = last > 0xFFFFFFFFUL ? 0xFFFFFFFFUL : (ULONG)last;
        if (cmd->scsi_Length < 8) {
            return IOERR_BADLENGTH;
        }
        memset(d, 0, 8);
        d[0] = (UBYTE)((l32 >> 24) & 0xFF);
        d[1] = (UBYTE)((l32 >> 16) & 0xFF);
        d[2] = (UBYTE)((l32 >> 8) & 0xFF);
        d[3] = (UBYTE)(l32 & 0xFF);
        d[4] = (UBYTE)((bs >> 24) & 0xFF);
        d[5] = (UBYTE)((bs >> 16) & 0xFF);
        d[6] = (UBYTE)((bs >> 8) & 0xFF);
        d[7] = (UBYTE)(bs & 0xFF);
        cmd->scsi_Actual = 8;
    }
    return 0;
}

/*
 * Synthesise INQUIRY so the unit identifies as itself.
 *
 * Forwarding INQUIRY untouched made a unit report the underlying drive's
 * vendor and product - devtest showed "COPPERLN SCSI DISK" for a partition.
 * That is wrong in the same way the geometry mode pages were: the caller
 * asked what THIS unit is, and a partition is not the drive it lives on.
 * lide synthesises INQUIRY from IDENTIFY for the same reason.
 */
static LONG scsi_inquiry(struct PUDisk *pd, struct PUUnit *pu,
                         struct SCSICmd *cmd, const UBYTE *cdb)
{
    UBYTE *d = (UBYTE *)cmd->scsi_Data;
    ULONG  len;

    if (d == NULL) {
        return IOERR_BADADDRESS;
    }
    /* EVPD: we publish no vital-product-data pages. */
    if (cdb[1] & 0x01) {
        return IOERR_NOCMD;
    }
    if (cmd->scsi_Length < 36) {
        return IOERR_BADLENGTH;
    }

    len = cmd->scsi_Length < 36 ? cmd->scsi_Length : 36;
    memset(d, 0, len);

    d[0] = 0x00;        /* direct-access block device */
    d[1] = (UBYTE)(pd->pd_Removable ? 0x80 : 0x00);
    d[2] = 0x02;        /* claims SCSI-2, as lide does: some software
                         * expects 2 and refuses 0 */
    d[3] = 0x02;        /* response data format */
    d[4] = 36 - 5;      /* additional length */

    memcpy(d + 8,  "PARTUNIT", 8);
    memcpy(d + 16, "Partition Unit  ", 16);
    memcpy(d + 32, "0001", 4);

    cmd->scsi_Actual = len;
    (void)pu;
    return 0;
}

/*
 * Synthesise MODE SENSE(6) pages 0x03 and 0x04.
 *
 * These were previously forwarded as "non-addressing" commands, which was a
 * mistake: page 0x03 (Format Parameters) and page 0x04 (Rigid Drive
 * Geometry) ARE geometry. devtest -g showed a 1024-block unit reporting 32
 * sectors per track and 32 cylinders / 16 heads - the underlying drive's
 * numbers, flatly contradicting what TD_GETGEOMETRY says about the same
 * unit. A caller that believes them computes addresses for the wrong disk.
 *
 * Pages are reported consistently with the synthetic geometry: one head, one
 * sector per track, cylinders = blocks, block size inherited. Only 0x03,
 * 0x04 and 0x3F (all) are answered; any other page is refused rather than
 * forwarded, so nothing about the child can leak through this command.
 */
#define MS_PAGE_LEN 24      /* page code + length + 22 bytes of payload */

static UBYTE *mode_page_03(UBYTE *p, const um_unit *u)
{
    memset(p, 0, MS_PAGE_LEN);
    p[0] = 0x03;
    p[1] = MS_PAGE_LEN - 2;
    p[10] = 0;                                  /* sectors per track, hi */
    p[11] = 1;                                  /* ... = 1, as we report */
    p[12] = (UBYTE)((u->block_size >> 8) & 0xFF);
    p[13] = (UBYTE)(u->block_size & 0xFF);
    return p + MS_PAGE_LEN;
}

static UBYTE *mode_page_04(UBYTE *p, const um_unit *u)
{
    /* The cylinder field is 3 bytes. Our cylinders == block count, so a
     * partition over 16777215 blocks cannot be expressed here; clamp rather
     * than wrap, as TD_GETGEOMETRY clamps to ULONG_MAX. */
    pt_u64 cyl = u->block_count;

    if (cyl > 0xFFFFFFUL) {
        cyl = 0xFFFFFFUL;
    }
    memset(p, 0, MS_PAGE_LEN);
    p[0] = 0x04;
    p[1] = MS_PAGE_LEN - 2;
    p[2] = (UBYTE)((cyl >> 16) & 0xFF);
    p[3] = (UBYTE)((cyl >> 8) & 0xFF);
    p[4] = (UBYTE)(cyl & 0xFF);
    p[5] = 1;                                   /* heads */
    return p + MS_PAGE_LEN;
}

static LONG scsi_mode_sense(struct PUUnit *pu, struct SCSICmd *cmd,
                            const UBYTE *cdb)
{
    UBYTE *d    = (UBYTE *)cmd->scsi_Data;
    UBYTE  page = (UBYTE)(cdb[2] & 0x3F);
    UBYTE  sub  = cdb[3];
    int    dbd  = (cdb[1] & 0x08) ? 1 : 0;
    UBYTE *p;
    ULONG  need;

    if (d == NULL) {
        return IOERR_BADADDRESS;
    }
    if (sub != 0) {
        return IOERR_NOCMD;         /* no subpages */
    }
    if (page != 0x03 && page != 0x04 && page != 0x3F) {
        return IOERR_NOCMD;
    }

    need = 4 + (dbd ? 0 : 8)
             + (page == 0x3F ? MS_PAGE_LEN * 2 : MS_PAGE_LEN);
    if (cmd->scsi_Length < need) {
        return IOERR_BADLENGTH;
    }

    memset(d, 0, need);
    d[0] = (UBYTE)(need - 1);       /* mode data length */
    d[1] = 0;                       /* medium type */
    d[2] = (UBYTE)(pu->pu_Map.writable ? 0x00 : 0x80);
    d[3] = (UBYTE)(dbd ? 0 : 8);    /* block descriptor length */

    p = d + 4;
    if (!dbd) {
        pt_u64 blocks = pu->pu_Map.block_count;
        ULONG  bs     = pu->pu_Map.block_size;

        if (blocks > 0xFFFFFFUL) {
            blocks = 0xFFFFFFUL;    /* 3-byte field */
        }
        p[0] = 0;                               /* density code */
        p[1] = (UBYTE)((blocks >> 16) & 0xFF);
        p[2] = (UBYTE)((blocks >> 8) & 0xFF);
        p[3] = (UBYTE)(blocks & 0xFF);
        p[4] = 0;
        p[5] = (UBYTE)((bs >> 16) & 0xFF);
        p[6] = (UBYTE)((bs >> 8) & 0xFF);
        p[7] = (UBYTE)(bs & 0xFF);
        p += 8;
    }

    if (page == 0x03 || page == 0x3F) {
        p = mode_page_03(p, &pu->pu_Map);
    }
    if (page == 0x04 || page == 0x3F) {
        p = mode_page_04(p, &pu->pu_Map);
    }

    cmd->scsi_Actual = need;
    return 0;
}

/*
 * HD_SCSICMD policy: non-addressing commands are forwarded untouched,
 * READ/WRITE have their LBA rewritten and bounds-checked, READ CAPACITY is
 * synthesised for the partition, and everything else is refused. The
 * allowlist is deliberately narrow - this is where a bounds bypass would
 * hide. lide arrived at almost exactly this set independently.
 */
static LONG do_scsicmd(struct PUDisk *pd, struct PUUnit *pu,
                       struct IOStdReq *ior)
{
    struct SCSICmd *cmd = (struct SCSICmd *)ior->io_Data;
    UBYTE  *cdb;
    UBYTE   op;
    LONG    err;

    if (cmd == NULL || cmd->scsi_Command == NULL || cmd->scsi_CmdLength == 0) {
        return IOERR_BADADDRESS;
    }
    if (!pu->pu_Map.media_present) {
        return TDERR_DiskChanged;
    }

    cdb = (UBYTE *)cmd->scsi_Command;
    op  = cdb[0];

    switch (op) {
    case SCSI_TEST_UNIT_READY:
        /*
         * The one command genuinely answered by the child: "is the medium
         * there and ready" is a property of the drive, and forwarding it is
         * how a unit learns its media went away. No identity or geometry
         * leaks through it.
         */
        break;

    case SCSI_INQUIRY:
        cmd->scsi_CmdActual = cmd->scsi_CmdLength;
        err = scsi_inquiry(pd, pu, cmd, cdb);
        cmd->scsi_Status = err == 0 ? 0 : 2;
        return err == 0 ? 0 : HFERR_BadStatus;

    case SCSI_MODE_SENSE_6:
        /*
         * Synthesised, not forwarded. MODE SENSE is not optional - devtest
         * -g ends by calling scsi_read_mode_pages() and returns its result
         * unchanged, so refusing it makes "devtest -g" exit nonzero even
         * when every printed line is correct - but answering it with the
         * child's geometry pages is worse than refusing, because the caller
         * believes them.
         */
        cmd->scsi_CmdActual = cmd->scsi_CmdLength;
        err = scsi_mode_sense(pu, cmd, cdb);
        cmd->scsi_Status = err == 0 ? 0 : 2;
        return err == 0 ? 0 : HFERR_BadStatus;

    case SCSI_READ_CAPACITY_10:
        cmd->scsi_CmdActual = cmd->scsi_CmdLength;
        err = scsi_read_capacity(pu, cmd, 0);
        cmd->scsi_Status = err == 0 ? 0 : 2;
        return err == 0 ? 0 : HFERR_BadStatus;

    case SCSI_READ_CAPACITY_16:
        cmd->scsi_CmdActual = cmd->scsi_CmdLength;
        err = scsi_read_capacity(pu, cmd, 1);
        cmd->scsi_Status = err == 0 ? 0 : 2;
        return err == 0 ? 0 : HFERR_BadStatus;

    case SCSI_READ_6:
    case SCSI_WRITE_6:
    case SCSI_READ_10:
    case SCSI_WRITE_10:
    case SCSI_READ_12:
    case SCSI_WRITE_12:
    case SCSI_READ_16:
    case SCSI_WRITE_16: {
        ULONG  blocks = 0;
        pt_u64 lba    = cdb_lba(cdb, op, &blocks);
        pt_u64 abs    = 0;
        int    is_write = (op == SCSI_WRITE_6  || op == SCSI_WRITE_10 ||
                           op == SCSI_WRITE_12 || op == SCSI_WRITE_16);

        err = (LONG)um_translate_scsi(&pu->pu_Map, lba, blocks,
                                      is_write, &abs);
        if (err != UM_OK) {
            cmd->scsi_CmdActual = cmd->scsi_CmdLength;
            cmd->scsi_Status    = 2;    /* CHECK CONDITION */
            cmd->scsi_SenseActual = 0;
            return HFERR_BadStatus;
        }
        cdb_set_lba(cdb, op, abs);
        break;
    }

    default:
        /*
         * Refused. Anything we do not understand could be an addressing
         * command we failed to recognise, and forwarding it would be exactly
         * the back door the allowlist exists to close.
         */
        return IOERR_NOCMD;
    }

    /* Forward whatever survived, CDB possibly rewritten. */
    pd->pd_ChildStd->io_Command = HD_SCSICMD;
    pd->pd_ChildStd->io_Data    = cmd;
    pd->pd_ChildStd->io_Length  = sizeof(struct SCSICmd);
    pd->pd_ChildStd->io_Offset  = 0;
    pd->pd_ChildStd->io_Actual  = 0;
    err = DoIO((struct IORequest *)pd->pd_ChildIO);
    ior->io_Actual = pd->pd_ChildStd->io_Actual;
    return err;
}

/* ------------------------------------------------------------------ *
 * The dispatcher
 * ------------------------------------------------------------------ */

void pu_process_ioreq(struct PUDisk *pd, struct IOStdReq *ior)
{
    struct PUUnit  *pu   = (struct PUUnit *)ior->io_Unit;
    struct IOExtTD *iotd = (struct IOExtTD *)ior;
    LONG            err  = 0;

    switch (ior->io_Command) {

    /* ---- plain transfers ---- */
    case CMD_READ:
        err = do_transfer(pd, pu, ior, 0, 0);
        break;
    case CMD_WRITE:
        err = do_transfer(pd, pu, ior, 1, 0);
        break;
    case TD_READ64:
    case NSCMD_TD_READ64:
        err = do_transfer(pd, pu, ior, 0, 1);
        break;
    case TD_WRITE64:
    case NSCMD_TD_WRITE64:
        err = do_transfer(pd, pu, ior, 1, 1);
        break;

    /*
     * ---- the ETD_* family ----
     *
     * The strictest requirement devtest imposes, and confirmed against RKRM
     * Devices (trackdisk, pp.305-306): "Any request found with an iotd_Count
     * less than the current change counter value will be returned with a
     * characteristic error (TDERR_DiskChange)".
     *
     * The comparison is "<", not "!=". A count AHEAD of ours is accepted -
     * and the spec names 0xFFFFFFFF as a documented "don't care" value that
     * must succeed, which "!=" would wrongly reject. That is the intuitive
     * mistake to avoid here.
     *
     * The check precedes the media and range checks, so a stale count always
     * reports TDERR_DiskChanged even when the offset is also out of range.
     */
    case ETD_READ:
    case ETD_WRITE:
    case ETD_FORMAT:
    case ETD_UPDATE:
    case ETD_CLEAR:
    case NSCMD_ETD_READ64:
    case NSCMD_ETD_WRITE64:
    case NSCMD_ETD_FORMAT64:
        if (iotd->iotd_Count < pu->pu_ChangeNum) {
            err = TDERR_DiskChanged;
            break;
        }
        switch (ior->io_Command) {
        case ETD_READ:
            err = do_transfer(pd, pu, ior, 0, 0);
            break;
        case ETD_WRITE:
        case ETD_FORMAT:
            err = do_transfer(pd, pu, ior, 1, 0);
            break;
        case NSCMD_ETD_READ64:
            err = do_transfer(pd, pu, ior, 0, 1);
            break;
        case NSCMD_ETD_WRITE64:
        case NSCMD_ETD_FORMAT64:
            err = do_transfer(pd, pu, ior, 1, 1);
            break;
        default:
            /* ETD_UPDATE / ETD_CLEAR: nothing cached, so nothing to do. */
            ior->io_Actual = 0;
            err = 0;
            break;
        }
        break;

    case TD_FORMAT:
        err = do_transfer(pd, pu, ior, 1, 0);
        break;
    case TD_FORMAT64:
    case NSCMD_TD_FORMAT64:
        err = do_transfer(pd, pu, ior, 1, 1);
        break;

    /* ---- status ---- */
    /*
     * TD_CHANGESTATE reports presence in io_Actual (0 = present) and
     * io_Error is ALWAYS 0: media state is not an error.
     */
    case TD_CHANGESTATE:
        ior->io_Actual = pu->pu_Map.media_present ? 0 : 1;
        err = 0;
        break;

    /*
     * TD_PROTSTATUS likewise: write protection is a status, so a
     * write-protected unit reports io_Error = 0 with io_Actual = 1, as lide
     * does by deliberately swallowing the underlying TDERR_WriteProt.
     */
    case TD_PROTSTATUS:
        ior->io_Actual = pu->pu_Map.writable ? 0 : 1;
        err = 0;
        break;

    /* Nothing is cached here, so these are honest no-ops - but they must
     * still answer io_Actual = 0 with no error, as lide does. If a buffering
     * layer is ever added, these become real. */
    case CMD_UPDATE:
    case CMD_CLEAR:
        ior->io_Actual = 0;
        err = 0;
        break;

    case HD_SCSICMD:
        err = do_scsicmd(pd, pu, ior);
        break;

    case PU_CMD_DIE:
        /* Handled by the task loop, not here. */
        err = 0;
        break;

    default:
        ior->io_Actual = 0;   /* may still hold a caller's high offset */
        err = IOERR_NOCMD;
        break;
    }

    ior->io_Error = (BYTE)err;
    ReplyMsg(&ior->io_Message);
}
