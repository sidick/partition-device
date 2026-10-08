/*
 * unitmap.h - unit numbering, offset translation, bounds and geometry for
 *             partunit.device.
 *
 * This is the arithmetic the device's safety guarantee rests on, so it lives
 * in its own module with no AmigaOS dependencies and is tested on the host.
 * The Exec glue in device.c does no address arithmetic of its own.
 *
 * The error codes are the real exec/errors.h and devices/trackdisk.h values,
 * verified against NDK 3.2 headers, so callers can compare directly against
 * the system constants.
 */

#ifndef UNITMAP_H
#define UNITMAP_H

#include "ptparse.h"

/* From exec/errors.h. */
#define UM_OK                0
#define UM_IOERR_NOCMD     (-3)
#define UM_IOERR_BADLENGTH (-4)
#define UM_IOERR_BADADDRESS (-5)

/* From devices/trackdisk.h. */
#define UM_TDERR_WriteProt    28
#define UM_TDERR_DiskChanged  29

/*
 * Unit numbering: unit = disk * UM_UNITS_PER_DISK + partition.
 *
 * This mirrors the scsi.device board*100 + LUN*10 + target idiom and is
 * stable across boots, since the disk index comes from config order and the
 * partition index from table order.
 *
 * Two consequences of the sparseness, both deliberate and both costly enough
 * to be worth stating:
 *
 *  - open() MUST return TDERR_BadUnitNum, never IOERR_OPENFAIL, for every
 *    unit number that does not exist. lide's comment is emphatic about why:
 *    "HDToolbox scans each LUN of a unit and stops searching if it sees an
 *    error other than TDERR_BadUnitNum. So if this is not returned, only one
 *    drive will ever be detected." A scanner walking 0..99 before reaching
 *    disk 1 must see TDERR_BadUnitNum for all 97 gaps. Note lide itself is
 *    asymmetric here - it returns IOERR_OPENFAIL for unitnum > highestUnit -
 *    and we must not copy that.
 *
 *  - devtest -p probes units as target + lun*10 and stops at the first open
 *    failure per target, so it will never list disks past the first. Cosmetic:
 *    -t, -g and -i all work on an explicitly named unit.
 */
#define UM_UNITS_PER_DISK   100
#define UM_MAX_DISKS        8
#define UM_MAX_PARTS_PER_DISK 32

#define um_unit_number(disk, part) \
    ((pt_u32)(disk) * UM_UNITS_PER_DISK + (pt_u32)(part))
#define um_unit_disk(unit)  ((pt_u32)(unit) / UM_UNITS_PER_DISK)
#define um_unit_part(unit)  ((pt_u32)(unit) % UM_UNITS_PER_DISK)

/*
 * What a unit needs to know to translate and bound a request. Deliberately
 * not the whole unit struct - this module sees only the geometry.
 */
typedef struct {
    pt_u64 start_lba;      /* absolute start on the underlying unit      */
    pt_u64 block_count;    /* length of the partition, in blocks         */
    pt_u32 block_size;     /* inherited from the underlying unit         */
    pt_u8  block_shift;    /* log2(block_size), precomputed              */
    pt_u8  writable;       /* 0 => writes fail with TDERR_WriteProt      */
    pt_u8  media_present;  /* 0 => data commands fail TDERR_DiskChanged  */
} um_unit;

/* A translated request, ready to hand to the underlying device. */
typedef struct {
    pt_u64 abs_lba;        /* absolute block on the underlying unit      */
    pt_u32 blocks;         /* length in blocks                           */
    pt_u64 abs_byte_off;   /* abs_lba << block_shift, for TD64 callers    */
    int    needs_64bit;    /* 1 if the byte offset or its end crosses 4GB */
} um_xfer;

/* Fill block_shift from block_size. Returns UM_OK, or BADADDRESS if the
 * block size is not a supported power of two. */
int um_unit_init(um_unit *u);

/*
 * Translate and bounds-check a data request.
 *
 * byte_off is the caller's offset *within the partition*, assembled by the
 * caller from io_Offset (low 32 bits) and io_Actual (high 32 bits) - see the
 * TD64 convention note in device.c. length is io_Length in bytes.
 *
 * is_write selects the write-protect check. Returns UM_OK and fills *x, or a
 * negative/positive system error code and leaves *x untouched.
 *
 * This is the device's defining safety property: an out-of-range request
 * fails rather than touching the neighbouring partition. Emu68 applies the
 * same two-part test (offset >= count || offset + len/bs > count) and returns
 * IOERR_BADADDRESS, which is what we match.
 */
int um_translate(const um_unit *u, pt_u64 byte_off, pt_u32 length,
                 int is_write, um_xfer *x);

/*
 * Rewrite a SCSI READ/WRITE LBA into the underlying unit's address space,
 * bounds-checked. Returns UM_OK and fills *abs_lba, or an error.
 *
 * Bounds enforcement must not have a SCSI-direct back door, so this shares
 * the partition extent with um_translate rather than reimplementing it.
 */
int um_translate_scsi(const um_unit *u, pt_u64 lba, pt_u32 blocks,
                      int is_write, pt_u64 *abs_lba);

/*
 * The synthetic geometry: the whole unit *is* the partition. One surface, one
 * block per track, cylinders = blocks, block size inherited.
 *
 * This is exact, loses nothing, and makes a mount entry trivially
 * LowCyl 0 / HighCyl blocks-1. It deliberately differs from Emu68, which
 * reports a fixed 128 heads x 64 sectors with dg_Cylinders = TotalSectors /
 * 8192 - that truncates, so up to 8191 blocks fall off the end of any mount
 * entry derived from its cylinder fields. AROS independently arrives at our
 * answer, falling back to "one block per cylinder (flat LBA)" whenever a
 * partition is not cylinder-aligned, which for modern 1MiB-aligned tables is
 * always. devtest checks none of this (it never verifies TotalSectors ==
 * C*H*S, and does not object to Heads == 1).
 *
 * Fields are written through a callback-free plain struct so this stays
 * host-testable; device.c copies them into the real struct DriveGeometry.
 */
typedef struct {
    pt_u32 total_sectors;  /* clamped to 0xFFFFFFFF; see below */
    pt_u32 sector_size;
    pt_u32 cylinders;
    pt_u32 cyl_sectors;
    pt_u32 track_sectors;
    pt_u32 heads;
    pt_u8  removable;
} um_geometry;

/*
 * dg_TotalSectors is a ULONG while our units are 64-bit addressable, so a
 * partition longer than 2^32 blocks must clamp rather than wrap - lide does
 * the same for drives over 2TB.
 */
void um_geometry_fill(const um_unit *u, int removable, um_geometry *g);

#endif /* UNITMAP_H */
