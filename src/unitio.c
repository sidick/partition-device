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
    case SCSI_INQUIRY:
    case SCSI_MODE_SENSE_6:
        /*
         * Forwarded untouched: they carry no address. MODE SENSE in
         * particular is not optional - devtest -g ends by calling
         * scsi_read_mode_pages() and returns its result unchanged, so
         * refusing it makes "devtest -g" exit nonzero even when every
         * printed line is correct.
         */
        break;

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
