/*
 * fixture.h - in-memory disk images for the ptparse tests.
 *
 * Fixtures are built in C rather than with sgdisk/parted, for two reasons:
 * they are byte-for-byte deterministic, and the malformed cases the hardening
 * tests need (an overflowing EntrySize x NumEntries, a cyclic EBR chain) are
 * not things a sane partitioning tool will emit on request.
 *
 * Cross-checking against real sgdisk/parted output is a separate, later step;
 * see docs/proposal.md's testing section.
 */

#ifndef FIXTURE_H
#define FIXTURE_H

#include "../src/ptparse.h"

typedef struct {
    pt_u8 *data;
    pt_u32 bs;
    pt_u64 blocks;
    unsigned long reads;     /* read count, so tests can assert on I/O      */
    pt_u64 fail_lba;         /* this LBA's read fails; ~0 for none          */
} img;

/* Known type GUIDs, in on-disk byte order. */
extern const pt_u8 fx_guid_amiga_spec[16];     /* what Emu68/sgdisk write   */
extern const pt_u8 fx_guid_amiga_winuae[16];   /* WinUAE's byteswapped form */
extern const pt_u8 fx_guid_linux[16];
extern const pt_u8 fx_guid_efi_system[16];

typedef struct {
    const pt_u8 *type;
    pt_u64       first;
    pt_u64       last;       /* inclusive, as GPT defines it */
    const char  *name;
} fx_gpt_part;

void   img_init(img *m, pt_u32 bs, pt_u64 blocks);
void   img_free(img *m);
int    img_read(void *user, pt_u64 lba, void *buf);
pt_u8 *img_blk(img *m, pt_u64 lba);
void   img_dev(const img *m, pt_device *dev, img *user);

void wr_le16(pt_u8 *p, pt_u16 v);
void wr_le32(pt_u8 *p, pt_u32 v);
void wr_le64(pt_u8 *p, pt_u64 v);
void wr_be32(pt_u8 *p, pt_u32 v);

/* MBR / EBR */
void img_mbr_init(img *m);
void img_mbr_entry(img *m, int slot, pt_u8 type, pt_u32 start, pt_u32 count);
void img_ebr(img *m, pt_u64 lba,
             pt_u8 t0, pt_u32 s0, pt_u32 n0,
             pt_u8 t1, pt_u32 s1, pt_u32 n1);
void img_fat_bpb(img *m);

/* GPT. Writes a protective MBR, both entry arrays and both headers, all with
 * correct CRCs. num_entries/entry_size let a test drive those fields. */
void img_gpt(img *m, const fx_gpt_part *parts, int n,
             pt_u32 num_entries, pt_u32 entry_size);

/* Recompute a header's CRC after a test has mutated one of its fields. */
void img_gpt_fix_header_crc(img *m, pt_u64 hdr_lba);
/* Break a header's CRC without touching any field it validates. */
void img_gpt_break_header_crc(img *m, pt_u64 hdr_lba);

/* RDB */
void img_rdb(img *m, pt_u64 lba, pt_u32 id_be, int valid_checksum);

#endif /* FIXTURE_H */
