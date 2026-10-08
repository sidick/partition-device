/*
 * unitmap.c - see unitmap.h.
 *
 * Every arithmetic path here is 64-bit and checked for overflow before use.
 * The hazard this module exists to prevent is a request that looks in-range
 * after a truncating or wrapping computation, which would let one unit write
 * into its neighbour - the exact failure the device is built to make
 * impossible.
 */

#include "unitmap.h"

int um_unit_init(um_unit *u)
{
    pt_u32 bs = u->block_size;
    pt_u8  shift = 0;

    if (bs < PT_MIN_BLOCK_SIZE || bs > PT_MAX_BLOCK_SIZE) {
        return UM_IOERR_BADADDRESS;
    }
    if ((bs & (bs - 1)) != 0) {
        return UM_IOERR_BADADDRESS;
    }
    while ((bs >> shift) > 1) {
        shift++;
    }
    u->block_shift = shift;
    return UM_OK;
}

int um_translate(const um_unit *u, pt_u64 byte_off, pt_u32 length,
                 int is_write, um_xfer *x)
{
    pt_u64 blocks64;
    pt_u64 off_blocks;
    pt_u64 end;
    pt_u64 abs;

    /*
     * Order matters, and matches lide: the media check comes first, before
     * any arithmetic, because block_size and block_shift are meaningless -
     * and zero - on a unit whose media has gone.
     */
    if (!u->media_present) {
        return UM_TDERR_DiskChanged;
    }
    if (is_write && !u->writable) {
        return UM_TDERR_WriteProt;
    }
    if (u->block_size == 0) {
        return UM_IOERR_BADADDRESS;
    }

    /*
     * Length must be a whole multiple of the block size. RKRM Devices states
     * the trackdisk restriction plainly (p.306): "All reads and writes must
     * use an io_Length that is an integer multiple of TD_SECTOR bytes" and
     * "The offset field must be an integer multiple of TD_SECTOR".
     *
     * Note a zero length is rejected too. lide gets there by a different
     * route - it shifts io_Length down to a block count and rejects count ==
     * 0 - which means a sub-block length silently truncates to zero and is
     * then refused. Same outcome, but checking the remainder says what we
     * mean.
     */
    if (length == 0 || (length & (u->block_size - 1)) != 0) {
        return UM_IOERR_BADLENGTH;
    }
    if ((byte_off & (pt_u64)(u->block_size - 1)) != 0) {
        return UM_IOERR_BADADDRESS;
    }

    off_blocks = byte_off >> u->block_shift;
    blocks64   = (pt_u64)length >> u->block_shift;

    /*
     * The two-part bounds test. Both halves are needed: the first catches a
     * start beyond the partition, the second a request that starts inside it
     * and runs off the end. The addition is checked for wrap first, because
     * on a 64-bit sum that is the only way the second test can be fooled.
     */
    if (off_blocks >= u->block_count) {
        return UM_IOERR_BADADDRESS;
    }
    end = off_blocks + blocks64;
    if (end < off_blocks) {
        return UM_IOERR_BADADDRESS;          /* wrapped */
    }
    if (end > u->block_count) {
        return UM_IOERR_BADADDRESS;
    }

    abs = u->start_lba + off_blocks;
    if (abs < u->start_lba) {
        return UM_IOERR_BADADDRESS;          /* wrapped */
    }

    x->abs_lba      = abs;
    x->blocks       = (pt_u32)blocks64;
    x->abs_byte_off = abs << u->block_shift;

    /*
     * Whether the *underlying* request needs a 64-bit command. This is the
     * case the proposal singles out and that devtest cannot test for us:
     * devtest's 4GB sub-tests only run when the unit itself exceeds 4GB, but
     * our hazard is a small partition whose absolute start is past 4GB.
     *
     * The end of the transfer is what matters, not just the start, so a
     * request straddling the boundary is caught.
     */
    {
        pt_u64 last_byte = x->abs_byte_off + (pt_u64)length - 1;
        x->needs_64bit = (last_byte > 0xFFFFFFFFUL) ? 1 : 0;
    }

    return UM_OK;
}

int um_translate_scsi(const um_unit *u, pt_u64 lba, pt_u32 blocks,
                      int is_write, pt_u64 *abs_lba)
{
    pt_u64 end;
    pt_u64 abs;

    if (!u->media_present) {
        return UM_TDERR_DiskChanged;
    }
    if (is_write && !u->writable) {
        return UM_TDERR_WriteProt;
    }
    if (blocks == 0) {
        return UM_IOERR_BADLENGTH;
    }
    if (lba >= u->block_count) {
        return UM_IOERR_BADADDRESS;
    }
    end = lba + (pt_u64)blocks;
    if (end < lba || end > u->block_count) {
        return UM_IOERR_BADADDRESS;
    }
    abs = u->start_lba + lba;
    if (abs < u->start_lba) {
        return UM_IOERR_BADADDRESS;
    }
    *abs_lba = abs;
    return UM_OK;
}

void um_geometry_fill(const um_unit *u, int removable, um_geometry *g)
{
    pt_u64 total = u->block_count;

    g->sector_size   = u->block_size;
    g->heads         = 1;
    g->track_sectors = 1;
    g->cyl_sectors   = 1;             /* heads * track_sectors */
    g->removable     = (pt_u8)(removable ? 1 : 0);

    if (total > 0xFFFFFFFFUL) {
        g->total_sectors = 0xFFFFFFFFUL;
        g->cylinders     = 0xFFFFFFFFUL;
    } else {
        g->total_sectors = (pt_u32)total;
        g->cylinders     = (pt_u32)total;
    }
}
