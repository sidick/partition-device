/*
 * child.c - talking to the underlying block device.
 *
 * Every request partunit.device serves is ultimately one or more requests to
 * a child device, issued from the owning disk's task so that our use of the
 * child is serialised. This file owns that conversation and nothing else; it
 * does no address arithmetic (see unitmap.c) and makes no policy decisions.
 */

#include "device.h"

#include <devices/newstyle.h>
#include <exec/errors.h>
#include <exec/memory.h>

#include <proto/exec.h>

#include <string.h>

/* The NDK has NSCMD_TD_* but not the ETD forms. */
#ifndef NSCMD_ETD_READ64
# define NSCMD_ETD_READ64   0xe000
# define NSCMD_ETD_WRITE64  0xe001
# define NSCMD_ETD_SEEK64   0xe002
# define NSCMD_ETD_FORMAT64 0xe003
#endif

LONG pu_child_open(struct PUDisk *pd)
{
    pd->pd_ChildPort = CreateMsgPort();
    if (pd->pd_ChildPort == NULL) {
        return IOERR_OPENFAIL;
    }

    /*
     * An IOExtTD, not an IOStdReq: we need iotd_Count to issue ETD_* to the
     * child, and IOExtTD begins with IOStdReq so the same memory serves both
     * views. CreateIORequest zeroes it.
     */
    pd->pd_ChildIO = (struct IOExtTD *)
        CreateIORequest(pd->pd_ChildPort, sizeof(struct IOExtTD));
    if (pd->pd_ChildIO == NULL) {
        DeleteMsgPort(pd->pd_ChildPort);
        pd->pd_ChildPort = NULL;
        return IOERR_OPENFAIL;
    }
    pd->pd_ChildStd = &pd->pd_ChildIO->iotd_Req;

    if (OpenDevice((STRPTR)pd->pd_Name, pd->pd_ChildUnit,
                   (struct IORequest *)pd->pd_ChildIO, 0) != 0) {
        DeleteIORequest((struct IORequest *)pd->pd_ChildIO);
        DeleteMsgPort(pd->pd_ChildPort);
        pd->pd_ChildIO   = NULL;
        pd->pd_ChildStd  = NULL;
        pd->pd_ChildPort = NULL;
        return IOERR_OPENFAIL;
    }

    pd->pd_ChildOpen = 1;
    return 0;
}

void pu_child_close(struct PUDisk *pd)
{
    if (pd->pd_ChildOpen) {
        CloseDevice((struct IORequest *)pd->pd_ChildIO);
        pd->pd_ChildOpen = 0;
    }
    if (pd->pd_ChildIO != NULL) {
        DeleteIORequest((struct IORequest *)pd->pd_ChildIO);
        pd->pd_ChildIO  = NULL;
        pd->pd_ChildStd = NULL;
    }
    if (pd->pd_ChildPort != NULL) {
        DeleteMsgPort(pd->pd_ChildPort);
        pd->pd_ChildPort = NULL;
    }
}

/*
 * Ask the child what it is: geometry, and whether it speaks NSD/TD64.
 *
 * The 64-bit question is per-child and must be asked, not assumed. We hand a
 * child a 64-bit command only if it advertised one; otherwise a partition
 * whose absolute address crosses 4GB is simply unreachable through that
 * child, which is a limitation to report rather than to paper over.
 */
LONG pu_child_probe(struct PUDisk *pd)
{
    struct DriveGeometry dg;
    struct NSDeviceQueryResult *nsd;
    UBYTE *buf;
    LONG   err;

    memset(&dg, 0, sizeof(dg));
    pd->pd_ChildStd->io_Command = TD_GETGEOMETRY;
    pd->pd_ChildStd->io_Data    = &dg;
    pd->pd_ChildStd->io_Length  = sizeof(dg);
    pd->pd_ChildStd->io_Offset  = 0;
    pd->pd_ChildStd->io_Actual  = 0;
    err = DoIO((struct IORequest *)pd->pd_ChildIO);
    if (err != 0) {
        return err;
    }

    if (dg.dg_SectorSize < PT_MIN_BLOCK_SIZE ||
        dg.dg_SectorSize > PT_MAX_BLOCK_SIZE ||
        (dg.dg_SectorSize & (dg.dg_SectorSize - 1)) != 0) {
        /*
         * We inherit the child's block size rather than assuming 512 - which
         * is what makes 4Kn media work, and is better than Amiberry manages
         * (it takes block size from config and so scans 4Kn media at a
         * 512-byte stride). But an implausible value is refused outright
         * rather than guessed at.
         */
        return TDERR_BadDriveType;
    }

    pd->pd_BlockSize   = dg.dg_SectorSize;
    pd->pd_TotalBlocks = (pt_u64)dg.dg_TotalSectors;
    pd->pd_Removable   = (UBYTE)((dg.dg_Flags & DGF_REMOVABLE) ? 1 : 0);

    /*
     * NSCMD_DEVICEQUERY. The result struct "may be extended in the future",
     * per the NDK header's own comment, so allocate generously and trust
     * nsdqr_SizeAvailable rather than our sizeof.
     */
    buf = AllocMem(256, MEMF_PUBLIC | MEMF_CLEAR);
    if (buf == NULL) {
        return TDERR_NoMem;
    }
    nsd = (struct NSDeviceQueryResult *)buf;
    nsd->nsdqr_DevQueryFormat = 0;
    nsd->nsdqr_SizeAvailable  = 0;

    pd->pd_ChildStd->io_Command = NSCMD_DEVICEQUERY;
    pd->pd_ChildStd->io_Data    = buf;
    pd->pd_ChildStd->io_Length  = 256;
    pd->pd_ChildStd->io_Offset  = 0;
    pd->pd_ChildStd->io_Actual  = 0;

    if (DoIO((struct IORequest *)pd->pd_ChildIO) == 0 &&
        nsd->nsdqr_DevQueryFormat == 0 &&
        nsd->nsdqr_SizeAvailable >= 16 &&
        nsd->nsdqr_DeviceType == NSDEVTYPE_TRACKDISK) {

        UWORD *cmd = (UWORD *)nsd->nsdqr_SupportedCommands;

        pd->pd_HasNSD = 1;
        if (cmd != NULL) {
            while (*cmd != 0) {
                if (*cmd == NSCMD_TD_READ64 || *cmd == TD_READ64) {
                    pd->pd_Has64 = 1;
                }
                cmd++;
            }
        }
    }

    FreeMem(buf, 256);

    /*
     * TD_CHANGENUM, so we have a baseline for the child's media state. Our
     * own per-unit change number is separate and starts at 1; this is only
     * used to notice that the child's has moved.
     */
    pd->pd_ChildStd->io_Command = TD_CHANGENUM;
    pd->pd_ChildStd->io_Data    = NULL;
    pd->pd_ChildStd->io_Length  = 0;
    pd->pd_ChildStd->io_Actual  = 0;
    if (DoIO((struct IORequest *)pd->pd_ChildIO) == 0) {
        pd->pd_ChildChangeNum = pd->pd_ChildStd->io_Actual;
    }

    return 0;
}

/*
 * Forward one read or write to the child.
 *
 * byte_off is absolute on the child, already translated and bounds-checked by
 * unitmap.c - this function must never be reached with an address outside the
 * partition, and does no checking of its own precisely so that there is one
 * place where bounds are decided rather than two that could disagree.
 */
LONG pu_child_rw(struct PUDisk *pd, int is_write, pt_u64 byte_off,
                 ULONG length, APTR data, ULONG *actual, int needs64)
{
    LONG err;

    if (needs64 && !pd->pd_Has64) {
        /*
         * The partition lives past 4GB on a child that cannot address it.
         * Report rather than truncate: a silently wrapped address would write
         * to the wrong place, which is the one failure this device exists to
         * prevent.
         */
        return IOERR_BADADDRESS;
    }

    pd->pd_ChildStd->io_Data   = data;
    pd->pd_ChildStd->io_Length = length;
    /*
     * The TD64 convention, confirmed by the NDK header itself, which defines
     * io_HighOffset as an alias for io_Actual: io_Offset carries the low 32
     * bits of a BYTE offset and io_Actual the high 32.
     */
    pd->pd_ChildStd->io_Offset = (ULONG)(byte_off & 0xFFFFFFFFUL);
    pd->pd_ChildStd->io_Actual = (ULONG)(byte_off >> 32);

    if (needs64) {
        if (pd->pd_HasNSD) {
            pd->pd_ChildStd->io_Command =
                is_write ? NSCMD_TD_WRITE64 : NSCMD_TD_READ64;
        } else {
            pd->pd_ChildStd->io_Command = is_write ? TD_WRITE64 : TD_READ64;
        }
    } else {
        pd->pd_ChildStd->io_Command = is_write ? CMD_WRITE : CMD_READ;
    }

    err = DoIO((struct IORequest *)pd->pd_ChildIO);
    if (actual != NULL) {
        *actual = err == 0 ? pd->pd_ChildStd->io_Actual : 0;
    }
    return err;
}

/*
 * The parser's block-read callback. Reads whole blocks at absolute LBAs on
 * the child, used only at init and on a media change.
 */
int pu_child_read_block(void *user, pt_u64 lba, void *buf)
{
    struct PUDisk *pd = (struct PUDisk *)user;
    pt_u64  off = (pt_u64)lba * (pt_u64)pd->pd_BlockSize;
    int    need64 = (off + pd->pd_BlockSize - 1) > 0xFFFFFFFFUL;

    if (pu_child_rw(pd, 0, off, pd->pd_BlockSize, buf, NULL, need64) != 0) {
        return 1;
    }
    return 0;
}
