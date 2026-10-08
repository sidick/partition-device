/*
 * ptparse.c - MBR / EBR / GPT partition table parser for partunit.device
 *
 * See ptparse.h for the contract. Two rules govern this file:
 *
 *   1. No struct is ever overlaid on a disk buffer, and no integer is ever
 *      memcpy'd out of one. Every field goes through rd_le16/32/64 or
 *      rd_be32. The target is big-endian m68k and the test host is not, so a
 *      violation here is invisible to the tests.
 *
 *   2. Every loop is bounded and every length is clamped before use. The
 *      hardening list in docs/proposal.md is derived from real defects in
 *      AROS's shipping GPT and EBR code; each item has a numbered comment
 *      below and a test in tests/test_ptparse.c.
 */

#include "ptparse.h"

#include <string.h>

/* ------------------------------------------------------------------ *
 * Byte readers. The whole endianness contract lives here.
 * ------------------------------------------------------------------ */

static pt_u16 rd_le16(const pt_u8 *p)
{
    return (pt_u16)((pt_u16)p[0] | ((pt_u16)p[1] << 8));
}

static pt_u32 rd_le32(const pt_u8 *p)
{
    return (pt_u32)p[0]
         | ((pt_u32)p[1] << 8)
         | ((pt_u32)p[2] << 16)
         | ((pt_u32)p[3] << 24);
}

static pt_u64 rd_le64(const pt_u8 *p)
{
    return (pt_u64)rd_le32(p) | ((pt_u64)rd_le32(p + 4) << 32);
}

static pt_u32 rd_be32(const pt_u8 *p)
{
    return ((pt_u32)p[0] << 24)
         | ((pt_u32)p[1] << 16)
         | ((pt_u32)p[2] << 8)
         |  (pt_u32)p[3];
}

/* ------------------------------------------------------------------ *
 * CRC-32/ISO-HDLC, as GPT requires: reflected, poly 0xEDB88320,
 * init 0xFFFFFFFF, final complement. Computed bytewise and therefore
 * endian-independent. No table, to keep the device small; the arrays
 * involved are at most a few tens of KB and this runs once per disk.
 * ------------------------------------------------------------------ */

pt_u32 pt_crc32(pt_u32 seed, const pt_u8 *buf, size_t len)
{
    pt_u32 crc = seed ^ 0xFFFFFFFFUL;
    size_t i;
    int    b;

    for (i = 0; i < len; i++) {
        crc ^= (pt_u32)buf[i];
        for (b = 0; b < 8; b++) {
            if (crc & 1UL) {
                crc = (crc >> 1) ^ 0xEDB88320UL;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

/*
 * Streaming variant: feed successive chunks, seed with 0 on the first call
 * and pass the previous return value thereafter. Needed because the GPT entry
 * array is CRC'd a block at a time rather than being held whole in memory.
 */
static pt_u32 crc32_feed(pt_u32 running, const pt_u8 *buf, size_t len)
{
    pt_u32 crc = running ^ 0xFFFFFFFFUL;
    size_t i;
    int    b;

    for (i = 0; i < len; i++) {
        crc ^= (pt_u32)buf[i];
        for (b = 0; b < 8; b++) {
            if (crc & 1UL) {
                crc = (crc >> 1) ^ 0xEDB88320UL;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

/* ------------------------------------------------------------------ *
 * Amiga partition types.
 * ------------------------------------------------------------------ */

/*
 * MBR/EBR types that mean "Amiga-owned".
 *
 * 0x76 is Amithlon's convention, adopted by Emu68 and WinUAE. 0x30 is
 * accepted by WinUAE, whose ChangeLog calls it "another Amithlon like RDB
 * drive inside real PC partition".
 *
 * 0x78 is deliberately NOT here: WinUAE's ChangeLog mentions "Amithlon
 * partition type (0x78/0x30)" but no current code tests for it, and we have
 * not established whether it is a historical type, a typo for 0x76, or
 * something still in the wild. Adding it is a one-line change once that is
 * settled - see the open question in docs/proposal.md.
 */
static const pt_u8 amiga_mbr_types[] = { 0x76, 0x30 };

/* MBR types whose contents are an extended partition holding an EBR chain. */
static int is_extended_type(pt_u8 t)
{
    return t == 0x05 || t == 0x0F || t == 0x85;
}

static int is_amiga_mbr_type(pt_u8 t)
{
    size_t i;
    for (i = 0; i < sizeof(amiga_mbr_types); i++) {
        if (amiga_mbr_types[i] == t) {
            return 1;
        }
    }
    return 0;
}

/*
 * The Amiga "RDB inside a GPT partition" type GUID, canonically
 * 3F82EEBC-87C9-4097-8165-89D6540557C0.
 *
 * Both byte orders are accepted on read. These two sequences were established
 * by compiling each implementation's constant and dumping the bytes; see
 * docs/phase0-notes.md.
 *
 *   SPEC   - what Emu68 matches and what sgdisk writes from the canonical
 *            string. Correct per UEFI 2.10 Appendix A, which stores the first
 *            three fields little-endian.
 *   WINUAE - what WinUAE's constant matches. Byteswapped within the first
 *            three fields; almost certainly a transcription slip, since
 *            WinUAE's own ChangeLog writes the GUID in canonical order.
 *
 * Accepting both is safe rather than lax: the WinUAE spelling is the
 * byteswapped form of an Amiga-specific GUID and cannot plausibly collide
 * with another vendor's registered type, so we are not widening the match in
 * a way that could claim a non-Amiga partition as a unit. We never write a
 * type GUID, so there is no corresponding ambiguity on the write side.
 */
static const pt_u8 guid_amiga_spec[16] = {
    0xBC, 0xEE, 0x82, 0x3F, 0xC9, 0x87, 0x97, 0x40,
    0x81, 0x65, 0x89, 0xD6, 0x54, 0x05, 0x57, 0xC0
};

static const pt_u8 guid_amiga_winuae[16] = {
    0x3F, 0x82, 0xEE, 0xBC, 0x87, 0xC9, 0x40, 0x97,
    0x81, 0x65, 0x89, 0xD6, 0x54, 0x05, 0x57, 0xC0
};

static const pt_u8 guid_unused[16] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* ------------------------------------------------------------------ *
 * Block access with a one-block cache.
 * ------------------------------------------------------------------ */

static pt_result fetch_block(const pt_device *dev, pt_scratch *s, pt_u64 lba)
{
    if (s->cache_valid && s->cached_lba == lba) {
        return PT_OK;
    }
    if (dev->total_blocks != 0 && lba >= dev->total_blocks) {
        return PT_ERR_IO;
    }
    if (dev->read(dev->user, lba, s->block) != 0) {
        s->cache_valid = 0;
        return PT_ERR_IO;
    }
    s->cached_lba  = lba;
    s->cache_valid = 1;
    return PT_OK;
}

/*
 * Copy len bytes from byte offset `off` within the device into dst, crossing
 * block boundaries as needed. Used for GPT entries, which need not be
 * block-aligned: EntrySize must be a multiple of 128 but may exceed the block
 * size, so an entry can straddle. We only ever need an entry's first 128
 * bytes (type GUID, unique GUID, first/last LBA, attributes, name), which is
 * why scratch->entry is that size.
 */
static pt_result fetch_bytes(const pt_device *dev, pt_scratch *s,
                             pt_u64 off, pt_u32 len, pt_u8 *dst)
{
    pt_u32 done = 0;

    while (done < len) {
        pt_u64    lba    = (off + done) / dev->block_size;
        pt_u32    within = (pt_u32)((off + done) % dev->block_size);
        pt_u32    chunk  = dev->block_size - within;
        pt_result r;

        if (chunk > len - done) {
            chunk = len - done;
        }
        r = fetch_block(dev, s, lba);
        if (r != PT_OK) {
            return r;
        }
        memcpy(dst + done, s->block + within, chunk);
        done += chunk;
    }
    return PT_OK;
}

/* ------------------------------------------------------------------ *
 * Partition collection, with honest truncation.
 * ------------------------------------------------------------------ */

typedef struct {
    pt_partition *parts;
    unsigned int  max;
    pt_table     *tbl;
} collector;

static void collect(collector *c, const pt_partition *p)
{
    c->tbl->seen++;
    if (c->tbl->count >= c->max || c->tbl->count >= PT_MAX_PARTITIONS) {
        /* Hardening: never silently drop. The caller learns via the note. */
        c->tbl->note = PT_NOTE_ENTRIES_TRUNCATED;
        return;
    }
    c->parts[c->tbl->count] = *p;
    c->tbl->count++;
    if (p->is_amiga) {
        c->tbl->amiga_count++;
    }
}

/*
 * Sanity-check a partition's extent before accepting it. AROS checks none of
 * this and produces wrapped lengths and garbage de_HighCyl from a malformed
 * table (hardening items: EndBlock >= StartBlock, containment, start != 0).
 */
static int extent_is_sane(const pt_device *dev, pt_u64 start, pt_u64 count)
{
    if (count == 0) {
        return 0;
    }
    if (start == 0) {
        /* A partition starting at the table itself is always wrong. */
        return 0;
    }
    if (start + count < start) {
        return 0;               /* 64-bit overflow */
    }
    if (dev->total_blocks != 0 && start + count > dev->total_blocks) {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ *
 * FAT superfloppy detection.
 *
 * A FAT BPB can end in 0xAA55, so block 0 of an unpartitioned FAT volume
 * looks like an MBR. AROS guards against this with FAT_IsBPBPlausible before
 * treating block 0 as a table; without it, a PC-formatted stick with no
 * partition table parses as four garbage partitions.
 * ------------------------------------------------------------------ */

static int looks_like_fat_bpb(const pt_u8 *b)
{
    pt_u16 bytes_per_sector = rd_le16(b + 11);
    pt_u8  sectors_per_cluster = b[13];
    pt_u16 reserved_sectors = rd_le16(b + 14);
    pt_u8  media = b[21];

    /* A jump instruction opens every real BPB. */
    if (!((b[0] == 0xEB && b[2] == 0x90) || b[0] == 0xE9)) {
        return 0;
    }
    switch (bytes_per_sector) {
    case 512: case 1024: case 2048: case 4096: break;
    default: return 0;
    }
    /* Must be a power of two, 1..128. */
    if (sectors_per_cluster == 0 ||
        (sectors_per_cluster & (pt_u8)(sectors_per_cluster - 1)) != 0) {
        return 0;
    }
    if (reserved_sectors == 0) {
        return 0;
    }
    if (media < 0xF0) {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ *
 * GPT
 * ------------------------------------------------------------------ */

#define GPT_SIG_0 0x20494645UL      /* "EFI " */
#define GPT_SIG_1 0x54524150UL      /* "PART" */
#define GPT_MIN_HEADER_SIZE 92

typedef struct {
    pt_u32 header_size;
    pt_u32 my_lba_ok;
    pt_u64 backup_lba;
    pt_u64 entries_lba;
    pt_u32 num_entries;
    pt_u32 entry_size;
    pt_u32 entries_crc;
} gpt_header;

static int gpt_header_plausible(const pt_device *dev, const gpt_header *h);

/*
 * Validate a GPT header found at `lba`.
 *
 * Hardening: HeaderSize is clamped to the block size before the CRC is
 * computed over it. AROS defines a maximum and never uses it, so a header
 * claiming a huge HeaderSize makes its CRC loop read far past the one-block
 * buffer. We also require MyLBA to equal the LBA we read it from, which is
 * what distinguishes a primary from a stale copy.
 *
 * The CRC is computed with the stored CRC field zeroed - in a local copy, so
 * the caller's buffer is not mutated. AROS zeroes it in place and never
 * restores it.
 */
static pt_result gpt_read_header(const pt_device *dev, pt_scratch *s,
                                 pt_u64 lba, gpt_header *h)
{
    pt_u8     hdr[PT_MAX_BLOCK_SIZE];
    pt_u32    hsize, stored_crc, calc_crc;
    pt_u64    my_lba;
    pt_result r;

    r = fetch_block(dev, s, lba);
    if (r != PT_OK) {
        return r;
    }

    if (rd_le32(s->block + 0) != GPT_SIG_0 ||
        rd_le32(s->block + 4) != GPT_SIG_1) {
        return PT_ERR_NO_TABLE;
    }

    hsize = rd_le32(s->block + 12);
    if (hsize < GPT_MIN_HEADER_SIZE || hsize > dev->block_size) {
        return PT_ERR_CORRUPT;      /* hardening: clamped, not trusted */
    }

    my_lba = rd_le64(s->block + 24);
    if (my_lba != lba) {
        return PT_ERR_CORRUPT;
    }

    stored_crc = rd_le32(s->block + 16);

    memcpy(hdr, s->block, hsize);
    hdr[16] = 0; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;
    calc_crc = pt_crc32(0, hdr, hsize);
    if (calc_crc != stored_crc) {
        return PT_ERR_CORRUPT;
    }

    h->header_size = hsize;
    h->my_lba_ok   = 1;
    h->backup_lba  = rd_le64(s->block + 32);
    h->entries_lba = rd_le64(s->block + 72);
    h->num_entries = rd_le32(s->block + 80);
    h->entry_size  = rd_le32(s->block + 84);
    h->entries_crc = rd_le32(s->block + 88);

    /* Absurd values are rejected here so primary and backup get the same
     * treatment, rather than only whichever one the caller checks. */
    if (!gpt_header_plausible(dev, h)) {
        return PT_ERR_CORRUPT;
    }
    return PT_OK;
}

/*
 * Validate the entry array's CRC by streaming it, then parse it.
 *
 * Hardening: EntrySize and NumEntries both come from the header and are both
 * bounded here, and their product is checked for overflow before use. AROS
 * multiplies them as plain ULONGs, so an overflowed product yields a small
 * allocation while its CRC and entry loops still run the full extent.
 */
static pt_result gpt_check_entries_crc(const pt_device *dev, pt_scratch *s,
                                       const gpt_header *h)
{
    pt_u64 total = (pt_u64)h->entry_size * (pt_u64)h->num_entries;
    pt_u64 done  = 0;
    pt_u32 crc   = 0;

    while (done < total) {
        pt_u64    lba    = h->entries_lba + (done / dev->block_size);
        pt_u32    within = (pt_u32)(done % dev->block_size);
        pt_u32    chunk  = dev->block_size - within;
        pt_result r;

        if ((pt_u64)chunk > total - done) {
            chunk = (pt_u32)(total - done);
        }
        r = fetch_block(dev, s, lba);
        if (r != PT_OK) {
            return r;
        }
        crc = crc32_feed(crc, s->block + within, chunk);
        done += chunk;
    }

    return (crc == h->entries_crc) ? PT_OK : PT_ERR_CORRUPT;
}

static pt_result gpt_parse_entries(const pt_device *dev, pt_scratch *s,
                                   const gpt_header *h, collector *c)
{
    pt_u32 i;

    for (i = 0; i < h->num_entries; i++) {
        pt_u64       off = h->entries_lba * (pt_u64)dev->block_size
                         + (pt_u64)i * (pt_u64)h->entry_size;
        pt_u32       want = h->entry_size < 128 ? h->entry_size : 128;
        pt_partition p;
        pt_u64       first, last;
        int          spec, swapped, j;
        pt_result    r;

        r = fetch_bytes(dev, s, off, want, s->entry);
        if (r != PT_OK) {
            return r;
        }

        /*
         * Unused entries are identified by an all-zero *type* GUID, which is
         * what the spec says. NumEntries is the preallocated count, not the
         * used count, and gaps between used entries are legal - AROS's
         * comment notes that partition editors generally squeeze the table
         * but that gaps must still be tolerated.
         */
        if (memcmp(s->entry, guid_unused, 16) == 0) {
            continue;
        }

        spec    = (memcmp(s->entry, guid_amiga_spec, 16) == 0);
        swapped = (memcmp(s->entry, guid_amiga_winuae, 16) == 0);

        first = rd_le64(s->entry + 32);
        last  = rd_le64(s->entry + 40);

        memset(&p, 0, sizeof(p));
        p.table_index      = (int)i;
        p.mbr_type         = 0;
        p.is_logical       = 0;
        p.guid_byteswapped = (pt_u8)(swapped ? 1 : 0);
        p.is_amiga         = (pt_u8)((spec || swapped) ? 1 : 0);

        /* Inclusive end, as GPT defines it. */
        if (last < first) {
            continue;               /* hardening: no wrapped lengths */
        }
        p.start_lba   = first;
        p.block_count = last - first + 1;

        if (!extent_is_sane(dev, p.start_lba, p.block_count)) {
            continue;
        }

        /*
         * Name: 72 bytes of UTF-16LE at offset 56. Reduced to Latin-1, which
         * is all the Amiga side can use; a non-Latin-1 codepoint becomes '?'.
         * codesets.library is not available this early, which is the same
         * reasoning AROS records.
         */
        if (h->entry_size >= 128) {
            for (j = 0; j < PT_GPT_NAME_CHARS; j++) {
                pt_u16 ch = rd_le16(s->entry + 56 + j * 2);
                if (ch == 0) {
                    break;
                }
                p.name[j] = (char)(ch < 0x100 ? (ch ? ch : '?') : '?');
            }
            p.name[j] = '\0';
        }

        collect(c, &p);
    }
    return PT_OK;
}

static pt_result gpt_try(const pt_device *dev, pt_scratch *s, collector *c)
{
    gpt_header h;
    pt_result  r, rprimary;

    memset(&h, 0, sizeof(h));

    /* The primary header is always at LBA 1. */
    rprimary = gpt_read_header(dev, s, 1, &h);
    if (rprimary == PT_OK) {
        r = gpt_check_entries_crc(dev, s, &h);
        if (r == PT_OK) {
            return gpt_parse_entries(dev, s, &h, c);
        }
    }

    /*
     * Hardening: fall back to the backup header on ANY primary failure, not
     * only a bad CRC - including an unreadable or signature-less LBA 1. AROS
     * tries the backup only on bad CRC, and its probe path cannot even do
     * that, because ERROR_BAD_CRC is 255 while the checking function returns
     * 2, making the fallback dead code.
     *
     * The backup LBA named by the primary header is not trusted: the primary
     * may be exactly what is corrupt. Prefer the device's own last block,
     * which is where the spec puts the backup, and fall back to the primary's
     * claim only when the device size is unknown. Either way the candidate is
     * bounded before use.
     */
    {
        pt_u64 cand = 0;

        if (dev->total_blocks != 0) {
            cand = dev->total_blocks - 1;
        } else if (h.backup_lba != 0) {
            cand = h.backup_lba;
        }

        if (cand <= 1) {
            return rprimary == PT_OK ? PT_ERR_CORRUPT : rprimary;
        }
        if (dev->total_blocks != 0 && cand >= dev->total_blocks) {
            return PT_ERR_CORRUPT;      /* hardening: bound the backup LBA */
        }

        memset(&h, 0, sizeof(h));
        r = gpt_read_header(dev, s, cand, &h);
        if (r != PT_OK) {
            return rprimary == PT_OK ? PT_ERR_CORRUPT : rprimary;
        }
        r = gpt_check_entries_crc(dev, s, &h);
        if (r != PT_OK) {
            return r;
        }
        c->tbl->note = PT_NOTE_GPT_BACKUP_USED;
        return gpt_parse_entries(dev, s, &h, c);
    }
}

/* Reject absurd header values before they are used for anything. */
static int gpt_header_plausible(const pt_device *dev, const gpt_header *h)
{
    pt_u64 total;

    if (h->entry_size < 128 || (h->entry_size % 128) != 0) {
        return 0;
    }
    if (h->num_entries == 0 || h->num_entries > PT_MAX_GPT_ENTRIES) {
        return 0;
    }
    /* Hardening: check the product for overflow before anyone multiplies it. */
    total = (pt_u64)h->entry_size * (pt_u64)h->num_entries;
    if (h->entry_size != 0 && total / h->entry_size != h->num_entries) {
        return 0;
    }
    if (h->entries_lba < 2) {
        return 0;
    }
    if (dev->total_blocks != 0) {
        pt_u64 blocks = (total + dev->block_size - 1) / dev->block_size;
        if (h->entries_lba + blocks > dev->total_blocks) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ *
 * MBR and the EBR chain
 * ------------------------------------------------------------------ */

#define MBR_TABLE_OFF 446
#define MBR_ENTRY_LEN 16

static void mbr_entry(const pt_u8 *blk, int slot,
                      pt_u8 *type, pt_u64 *start, pt_u64 *count)
{
    const pt_u8 *e = blk + MBR_TABLE_OFF + slot * MBR_ENTRY_LEN;
    *type  = e[4];
    *start = (pt_u64)rd_le32(e + 8);
    *count = (pt_u64)rd_le32(e + 12);
}

/*
 * Walk the EBR chain of one extended partition.
 *
 * Hardening, all three of which AROS lacks entirely - its induction variable
 * is a UBYTE that is incremented and never read, so a cyclic chain allocates
 * per iteration until memory is exhausted, reachable from boot:
 *
 *   - an iteration cap (PT_MAX_EBR_LINKS)
 *   - a strictly-increasing LBA requirement, which makes a cycle impossible
 *     and therefore subsumes a visited set
 *   - containment within the extended partition's own extent
 *
 * Also note the addressing rule, which AROS gets wrong: entry 0's start is
 * relative to the *current EBR's* LBA, while entry 1's link is relative to
 * the *extended partition's base*. Those coincide only on the first link,
 * which is why the bug is easy to miss.
 */
static pt_result ebr_walk(const pt_device *dev, pt_scratch *s, collector *c,
                          pt_u64 ext_base, pt_u64 ext_count)
{
    pt_u64       cur  = ext_base;
    pt_u64       prev = 0;
    unsigned int links = 0;

    while (links < PT_MAX_EBR_LINKS) {
        pt_u8     t0, t1;
        pt_u64    s0, n0, s1, n1, next;
        pt_result r;

        if (cur < ext_base ||
            (ext_count != 0 && cur >= ext_base + ext_count)) {
            c->tbl->note = PT_NOTE_EBR_CHAIN_BROKEN;
            return PT_OK;
        }
        if (links > 0 && cur <= prev) {
            c->tbl->note = PT_NOTE_EBR_CHAIN_BROKEN;
            return PT_OK;
        }

        r = fetch_block(dev, s, cur);
        if (r != PT_OK) {
            c->tbl->note = PT_NOTE_EBR_CHAIN_BROKEN;
            return PT_OK;       /* a short chain is not a fatal disk error */
        }
        if (rd_le16(s->block + 510) != 0xAA55) {
            c->tbl->note = PT_NOTE_EBR_CHAIN_BROKEN;
            return PT_OK;
        }

        mbr_entry(s->block, 0, &t0, &s0, &n0);
        mbr_entry(s->block, 1, &t1, &s1, &n1);

        if (t0 != 0 && n0 != 0) {
            pt_partition p;
            memset(&p, 0, sizeof(p));
            p.start_lba   = cur + s0;         /* relative to THIS EBR */
            p.block_count = n0;
            p.mbr_type    = t0;
            p.is_logical  = 1;
            p.is_amiga    = (pt_u8)(is_amiga_mbr_type(t0) ? 1 : 0);
            p.table_index = (int)links;
            if (extent_is_sane(dev, p.start_lba, p.block_count)) {
                collect(c, &p);
            }
        }

        if (s1 == 0 || !is_extended_type(t1)) {
            return PT_OK;                     /* end of chain, normally */
        }
        next = ext_base + s1;                 /* relative to the ext base */
        prev = cur;
        cur  = next;
        links++;
    }

    c->tbl->note = PT_NOTE_EBR_CHAIN_CAPPED;
    return PT_OK;
}

static pt_result mbr_try(const pt_device *dev, pt_scratch *s, collector *c)
{
    pt_u8     prim_type[4];
    pt_u64    prim_start[4], prim_count[4];
    int       slot;
    pt_result r;

    r = fetch_block(dev, s, 0);
    if (r != PT_OK) {
        return r;
    }
    if (rd_le16(s->block + 510) != 0xAA55) {
        return PT_ERR_NO_TABLE;
    }
    if (looks_like_fat_bpb(s->block)) {
        c->tbl->note = PT_NOTE_FAT_SUPERFLOPPY;
        return PT_ERR_NO_TABLE;
    }

    /* Snapshot the table: the EBR walk will reuse the block cache. */
    for (slot = 0; slot < 4; slot++) {
        mbr_entry(s->block, slot,
                  &prim_type[slot], &prim_start[slot], &prim_count[slot]);
    }

    for (slot = 0; slot < 4; slot++) {
        if (prim_type[slot] == 0 || prim_count[slot] == 0) {
            continue;
        }
        if (is_extended_type(prim_type[slot])) {
            r = ebr_walk(dev, s, c, prim_start[slot], prim_count[slot]);
            if (r != PT_OK) {
                return r;
            }
            continue;
        }
        {
            pt_partition p;
            memset(&p, 0, sizeof(p));
            p.start_lba   = prim_start[slot];
            p.block_count = prim_count[slot];
            p.mbr_type    = prim_type[slot];
            p.is_logical  = 0;
            p.is_amiga    = (pt_u8)(is_amiga_mbr_type(prim_type[slot]) ? 1 : 0);
            p.table_index = slot;
            if (extent_is_sane(dev, p.start_lba, p.block_count)) {
                collect(c, &p);
            }
        }
    }
    return PT_OK;
}

/* ------------------------------------------------------------------ *
 * Entry point
 * ------------------------------------------------------------------ */

pt_result pt_parse(const pt_device *dev, pt_scratch *s,
                   pt_partition *parts, unsigned int max_parts,
                   pt_table *tbl)
{
    collector c;
    pt_result r;

    if (dev == NULL || s == NULL || parts == NULL || tbl == NULL ||
        dev->read == NULL || max_parts == 0) {
        return PT_ERR_PARAM;
    }
    if (dev->block_size < PT_MIN_BLOCK_SIZE ||
        dev->block_size > PT_MAX_BLOCK_SIZE ||
        (dev->block_size & (dev->block_size - 1)) != 0) {
        return PT_ERR_PARAM;
    }

    memset(tbl, 0, sizeof(*tbl));
    s->cache_valid = 0;

    c.parts = parts;
    c.max   = max_parts;
    c.tbl   = tbl;

    /*
     * GPT before MBR, and a valid GPT wins outright: the MBR is then ignored
     * entirely, which is the UEFI rule for a hybrid. brcm-emmc.device gets
     * this wrong - its protective-MBR test fails *after* the GPT units have
     * already been appended, so one hybrid card yields both sets of units.
     *
     * AROS's probe order has the same shape, for a related reason recorded in
     * its source: GPT must precede MBR, and RDB must come last or an
     * MBR->EBR->RDB disk has its MBR and EBR offsets ignored.
     */
    r = gpt_try(dev, s, &c);
    if (r == PT_OK) {
        tbl->scheme = PT_SCHEME_GPT;
        if (tbl->note == PT_NOTE_NONE) {
            tbl->note = PT_NOTE_MBR_IGNORED_HYBRID;
        }
        return PT_OK;
    }
    /*
     * GPT absent or unusable: reset the collector and try the MBR. The reset
     * matters - a GPT whose entry array CRC failed partway through may have
     * left partitions in the array, and those must not be mixed with MBR
     * results.
     */
    memset(tbl, 0, sizeof(*tbl));
    c.tbl = tbl;

    r = mbr_try(dev, s, &c);
    if (r != PT_OK) {
        return r;
    }
    tbl->scheme = PT_SCHEME_MBR;
    return PT_OK;
}

/* ------------------------------------------------------------------ *
 * RDB sniff, for the whole-disk back-off decision
 * ------------------------------------------------------------------ */

#define RDB_ID_RDSK 0x5244534BUL    /* 'RDSK' */
#define RDB_ID_CDSK 0x4344534BUL    /* 'CDSK' */
#define RDB_ID_DRKS 0x44524B53UL    /* 'DRKS' - byteswapped RDSK */

/*
 * An RDB block is Amiga-native, so big-endian, and is checksummed by summing
 * rdb_SummedLongs longwords from offset 0 to zero.
 *
 * Hardening: rdb_SummedLongs is clamped to the block size before use.
 * brcm-emmc.device uses it unchecked as a loop bound over a 512-byte buffer,
 * which is a straight over-read from malformed media - and our sniff runs on
 * untrusted media by definition.
 */
static int rdb_checksum_ok(const pt_u8 *blk, pt_u32 block_size)
{
    pt_u32 summed = rd_be32(blk + 4);
    pt_u32 sum    = 0;
    pt_u32 i, max;

    max = block_size / 4;
    if (summed < 2 || summed > max) {
        return 0;               /* clamped, not trusted */
    }
    for (i = 0; i < summed; i++) {
        sum += rd_be32(blk + i * 4);
    }
    return sum == 0;
}

static int rdb_block_matches(const pt_u8 *blk, pt_u32 block_size)
{
    pt_u32 id = rd_be32(blk);

    if (id == RDB_ID_RDSK || id == RDB_ID_CDSK) {
        if (rdb_checksum_ok(blk, block_size)) {
            return 1;
        }
        /*
         * The Win9x-trashed form: magic intact, checksum broken because
         * Windows wrote over bytes 0xDC..0xDF. WinUAE zeroes them, re-checks,
         * and writes the repair back to the medium. We do the same test for
         * *detection* only - this is somebody else's metadata and repairing
         * it is not our business.
         */
        if (block_size > 0xDF) {
            pt_u8  copy[PT_MAX_BLOCK_SIZE];
            memcpy(copy, blk, block_size);
            copy[0xDC] = 0; copy[0xDD] = 0; copy[0xDE] = 0; copy[0xDF] = 0;
            if (rdb_checksum_ok(copy, block_size)) {
                return 1;
            }
        }
        return 0;
    }

    /*
     * Byteswapped and ADIDE-scrambled RDBs are still RDBs the OS may mount,
     * so they must trigger the back-off. We do not attempt to unscramble and
     * validate the checksum - recognising the signature is enough to decide
     * "the OS owns this disk", which is the only question being asked.
     */
    if (id == RDB_ID_DRKS) {
        return 1;
    }
    if (blk[0] == 0x39 && blk[1] == 0x10 && blk[2] == 0xD3 && blk[3] == 0x12) {
        return 1;               /* ADIDE 'CPRM' */
    }
    return 0;
}

pt_result pt_sniff_rdb(const pt_device *dev, pt_scratch *s,
                       int *found, pt_u32 *at_block)
{
    pt_u32 b;

    if (dev == NULL || s == NULL || found == NULL || at_block == NULL ||
        dev->read == NULL) {
        return PT_ERR_PARAM;
    }
    if (dev->block_size < PT_MIN_BLOCK_SIZE ||
        dev->block_size > PT_MAX_BLOCK_SIZE) {
        return PT_ERR_PARAM;
    }

    *found    = 0;
    *at_block = 0;
    s->cache_valid = 0;

    for (b = 0; b < PT_RDB_SCAN_BLOCKS; b++) {
        if (dev->total_blocks != 0 && (pt_u64)b >= dev->total_blocks) {
            break;
        }
        /*
         * A read failure mid-scan is not fatal: a short or flaky disk should
         * not be reported as "no RDB" on the strength of one bad block, but
         * neither should it abort the whole parse. Skip and continue.
         */
        if (fetch_block(dev, s, (pt_u64)b) != PT_OK) {
            continue;
        }
        if (rdb_block_matches(s->block, dev->block_size)) {
            *found    = 1;
            *at_block = b;
            return PT_OK;
        }
    }
    return PT_OK;
}

/* ------------------------------------------------------------------ *
 * Diagnostics
 * ------------------------------------------------------------------ */

const char *pt_note_str(pt_note n)
{
    switch (n) {
    case PT_NOTE_NONE:              return "ok";
    case PT_NOTE_RDB_WHOLE_DISK:    return "RDB on raw disk - OS owns it";
    case PT_NOTE_GPT_BACKUP_USED:   return "primary GPT bad, backup used";
    case PT_NOTE_MBR_IGNORED_HYBRID:return "GPT used, MBR ignored";
    case PT_NOTE_EBR_CHAIN_CAPPED:  return "EBR chain too long, capped";
    case PT_NOTE_EBR_CHAIN_BROKEN:  return "EBR chain broken, stopped";
    case PT_NOTE_ENTRIES_TRUNCATED: return "more partitions than slots";
    case PT_NOTE_FAT_SUPERFLOPPY:   return "FAT volume, no partition table";
    }
    return "unknown";
}

const char *pt_result_str(pt_result r)
{
    switch (r) {
    case PT_OK:            return "ok";
    case PT_ERR_IO:        return "read failed";
    case PT_ERR_PARAM:     return "bad parameter";
    case PT_ERR_NO_TABLE:  return "no partition table";
    case PT_ERR_TRUNCATED: return "too many partitions";
    case PT_ERR_CORRUPT:   return "table corrupt";
    }
    return "unknown";
}
