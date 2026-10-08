#include "fixture.h"

#include <stdlib.h>
#include <string.h>

/*
 * The verified on-disk byte sequences. See docs/phase0-notes.md: these were
 * established by compiling each implementation's constant and dumping the
 * bytes, cross-checked against what sgdisk writes from the canonical string
 * 3F82EEBC-87C9-4097-8165-89D6540557C0.
 */
const pt_u8 fx_guid_amiga_spec[16] = {
    0xBC, 0xEE, 0x82, 0x3F, 0xC9, 0x87, 0x97, 0x40,
    0x81, 0x65, 0x89, 0xD6, 0x54, 0x05, 0x57, 0xC0
};
const pt_u8 fx_guid_amiga_winuae[16] = {
    0x3F, 0x82, 0xEE, 0xBC, 0x87, 0xC9, 0x40, 0x97,
    0x81, 0x65, 0x89, 0xD6, 0x54, 0x05, 0x57, 0xC0
};
/* 0FC63DAF-8483-4772-8E79-3D69D8477DE4 */
const pt_u8 fx_guid_linux[16] = {
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};
/* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
const pt_u8 fx_guid_efi_system[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

void wr_le16(pt_u8 *p, pt_u16 v)
{
    p[0] = (pt_u8)(v & 0xFF);
    p[1] = (pt_u8)((v >> 8) & 0xFF);
}

void wr_le32(pt_u8 *p, pt_u32 v)
{
    p[0] = (pt_u8)(v & 0xFF);
    p[1] = (pt_u8)((v >> 8) & 0xFF);
    p[2] = (pt_u8)((v >> 16) & 0xFF);
    p[3] = (pt_u8)((v >> 24) & 0xFF);
}

void wr_le64(pt_u8 *p, pt_u64 v)
{
    wr_le32(p, (pt_u32)(v & 0xFFFFFFFFUL));
    wr_le32(p + 4, (pt_u32)((v >> 32) & 0xFFFFFFFFUL));
}

void wr_be32(pt_u8 *p, pt_u32 v)
{
    p[0] = (pt_u8)((v >> 24) & 0xFF);
    p[1] = (pt_u8)((v >> 16) & 0xFF);
    p[2] = (pt_u8)((v >> 8) & 0xFF);
    p[3] = (pt_u8)(v & 0xFF);
}

void img_init(img *m, pt_u32 bs, pt_u64 blocks)
{
    m->bs       = bs;
    m->blocks   = blocks;
    m->reads    = 0;
    m->fail_lba = (pt_u64)-1;
    m->data     = (pt_u8 *)calloc((size_t)(bs * blocks), 1);
}

void img_free(img *m)
{
    free(m->data);
    m->data = NULL;
}

pt_u8 *img_blk(img *m, pt_u64 lba)
{
    return m->data + (size_t)(lba * m->bs);
}

int img_read(void *user, pt_u64 lba, void *buf)
{
    img *m = (img *)user;
    m->reads++;
    if (lba == m->fail_lba) {
        return 1;
    }
    if (lba >= m->blocks) {
        return 1;
    }
    memcpy(buf, m->data + (size_t)(lba * m->bs), m->bs);
    return 0;
}

void img_dev(const img *m, pt_device *dev, img *user)
{
    dev->read         = img_read;
    dev->user         = user;
    dev->block_size   = m->bs;
    dev->total_blocks = m->blocks;
}

/* ---------------- MBR / EBR ---------------- */

void img_mbr_init(img *m)
{
    pt_u8 *b = img_blk(m, 0);
    /* A plausible boot-code opening that is NOT a FAT BPB jump. */
    b[0] = 0x33; b[1] = 0xC0; b[2] = 0x8E;
    wr_le16(b + 510, 0xAA55);
}

void img_mbr_entry(img *m, int slot, pt_u8 type, pt_u32 start, pt_u32 count)
{
    pt_u8 *e = img_blk(m, 0) + 446 + slot * 16;
    e[0] = 0x00;
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;   /* junk CHS, as modern tools do */
    e[4] = type;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    wr_le32(e + 8, start);
    wr_le32(e + 12, count);
}

void img_ebr(img *m, pt_u64 lba,
             pt_u8 t0, pt_u32 s0, pt_u32 n0,
             pt_u8 t1, pt_u32 s1, pt_u32 n1)
{
    pt_u8 *b = img_blk(m, lba);
    pt_u8 *e;

    memset(b, 0, m->bs);
    wr_le16(b + 510, 0xAA55);

    e = b + 446;
    e[4] = t0;
    wr_le32(e + 8, s0);
    wr_le32(e + 12, n0);

    e = b + 446 + 16;
    e[4] = t1;
    wr_le32(e + 8, s1);
    wr_le32(e + 12, n1);
}

void img_fat_bpb(img *m)
{
    pt_u8 *b = img_blk(m, 0);

    memset(b, 0, m->bs);
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;      /* jmp short / nop */
    memcpy(b + 3, "MSDOS5.0", 8);
    wr_le16(b + 11, 512);                        /* bytes per sector */
    b[13] = 8;                                   /* sectors per cluster */
    wr_le16(b + 14, 1);                          /* reserved sectors */
    b[16] = 2;                                   /* FAT count */
    b[21] = 0xF8;                                /* media descriptor */
    wr_le16(b + 510, 0xAA55);                    /* the trap */
}

/* ---------------- GPT ---------------- */

#define GPT_HDR_SIZE 92

static pt_u32 arr_blocks(const img *m, pt_u32 num_entries, pt_u32 entry_size)
{
    pt_u64 bytes = (pt_u64)num_entries * (pt_u64)entry_size;
    return (pt_u32)((bytes + m->bs - 1) / m->bs);
}

static void write_entries(img *m, pt_u64 lba,
                          const fx_gpt_part *parts, int n,
                          pt_u32 num_entries, pt_u32 entry_size)
{
    pt_u8 *base = img_blk(m, lba);
    int    i, j;

    memset(base, 0, (size_t)arr_blocks(m, num_entries, entry_size) * m->bs);

    for (i = 0; i < n && (pt_u32)i < num_entries; i++) {
        pt_u8 *e = base + (size_t)i * entry_size;

        /* A NULL type leaves the entry zeroed, i.e. unused - which is how a
         * test asks for a gap in the middle of the array. */
        if (parts[i].type == NULL) {
            continue;
        }
        memcpy(e, parts[i].type, 16);
        /* unique partition GUID: anything non-zero and distinct */
        memset(e + 16, 0x11 + i, 16);
        wr_le64(e + 32, parts[i].first);
        wr_le64(e + 40, parts[i].last);
        wr_le64(e + 48, 0);                      /* attributes */
        if (parts[i].name != NULL) {
            for (j = 0; parts[i].name[j] != '\0' && j < 36; j++) {
                wr_le16(e + 56 + j * 2, (pt_u16)(pt_u8)parts[i].name[j]);
            }
        }
    }
}

static void write_header(img *m, pt_u64 hdr_lba, pt_u64 alt_lba,
                         pt_u64 entries_lba, pt_u32 num_entries,
                         pt_u32 entry_size, pt_u32 first_usable,
                         pt_u64 last_usable)
{
    pt_u8 *h = img_blk(m, hdr_lba);
    pt_u32 ecrc;

    memset(h, 0, m->bs);
    memcpy(h, "EFI PART", 8);
    wr_le32(h + 8, 0x00010000UL);                /* revision 1.0 */
    wr_le32(h + 12, GPT_HDR_SIZE);
    wr_le32(h + 16, 0);                          /* header CRC, filled below */
    wr_le32(h + 20, 0);                          /* reserved */
    wr_le64(h + 24, hdr_lba);                    /* MyLBA */
    wr_le64(h + 32, alt_lba);                    /* AlternateLBA */
    wr_le64(h + 40, first_usable);
    wr_le64(h + 48, last_usable);
    memset(h + 56, 0x5A, 16);                    /* DiskGUID */
    wr_le64(h + 72, entries_lba);
    wr_le32(h + 80, num_entries);
    wr_le32(h + 84, entry_size);

    ecrc = pt_crc32(0, img_blk(m, entries_lba),
                    (size_t)num_entries * entry_size);
    wr_le32(h + 88, ecrc);

    img_gpt_fix_header_crc(m, hdr_lba);
}

void img_gpt_fix_header_crc(img *m, pt_u64 hdr_lba)
{
    pt_u8 *h = img_blk(m, hdr_lba);
    pt_u8  copy[PT_MAX_BLOCK_SIZE];
    pt_u32 hsize;

    hsize = (pt_u32)h[12] | ((pt_u32)h[13] << 8) |
            ((pt_u32)h[14] << 16) | ((pt_u32)h[15] << 24);
    if (hsize > m->bs) {
        hsize = GPT_HDR_SIZE;    /* mutated beyond the block: CRC the real part */
    }
    memcpy(copy, h, hsize);
    copy[16] = 0; copy[17] = 0; copy[18] = 0; copy[19] = 0;
    wr_le32(h + 16, pt_crc32(0, copy, hsize));
}

void img_gpt_break_header_crc(img *m, pt_u64 hdr_lba)
{
    pt_u8 *h = img_blk(m, hdr_lba);
    /* DiskGUID is covered by the CRC but validated by nothing. */
    h[56] ^= 0xFF;
}

void img_gpt(img *m, const fx_gpt_part *parts, int n,
             pt_u32 num_entries, pt_u32 entry_size)
{
    pt_u32 ab          = arr_blocks(m, num_entries, entry_size);
    pt_u64 backup_hdr  = m->blocks - 1;
    pt_u64 backup_arr  = backup_hdr - ab;
    pt_u32 first_usable = 2 + ab;
    pt_u64 last_usable  = backup_arr - 1;

    /* Protective MBR: one 0xEE partition covering the rest of the disk. */
    img_mbr_init(m);
    img_mbr_entry(m, 0, 0xEE, 1,
                  (pt_u32)(m->blocks - 1 > 0xFFFFFFFFUL
                           ? 0xFFFFFFFFUL : m->blocks - 1));

    write_entries(m, 2, parts, n, num_entries, entry_size);
    write_entries(m, backup_arr, parts, n, num_entries, entry_size);

    write_header(m, 1, backup_hdr, 2, num_entries, entry_size,
                 first_usable, last_usable);
    write_header(m, backup_hdr, 1, backup_arr, num_entries, entry_size,
                 first_usable, last_usable);
}

/* ---------------- RDB ---------------- */

void img_rdb(img *m, pt_u64 lba, pt_u32 id_be, int valid_checksum)
{
    pt_u8 *b = img_blk(m, lba);
    pt_u32 summed = 64;
    pt_u32 sum = 0;
    pt_u32 i;

    memset(b, 0, m->bs);
    wr_be32(b + 0, id_be);
    wr_be32(b + 4, summed);
    wr_be32(b + 8, 0);                   /* checksum, filled below */
    wr_be32(b + 12, 0x00000007);         /* host id */
    wr_be32(b + 16, m->bs);              /* rdb_BlockBytes */

    if (!valid_checksum) {
        wr_be32(b + 8, 0xDEADBEEFUL);
        return;
    }
    for (i = 0; i < summed; i++) {
        pt_u8 *p = b + i * 4;
        sum += ((pt_u32)p[0] << 24) | ((pt_u32)p[1] << 16) |
               ((pt_u32)p[2] << 8) | (pt_u32)p[3];
    }
    /* Sum of all summed longwords must be zero. */
    wr_be32(b + 8, (pt_u32)(0UL - sum));
}
