/*
 * test_ptparse.c - host tests for the partunit.device partition parser.
 *
 * Every hardening requirement in docs/proposal.md has a case here, named
 * after it. The malformed fixtures matter more than the well-formed ones:
 * Phase 0 found real, exploitable defects in AROS's shipping GPT and EBR
 * code, and this file exists to prove we do not repeat them.
 *
 * NOTE ON ENDIANNESS: the host is little-endian and the target (m68k) is not,
 * so these tests cannot catch a byte-order bug in ptparse.c. That contract is
 * upheld by construction - every disk field goes through the rd_le/rd_be
 * helpers and no struct is overlaid on a buffer. test_crc32_known_answer and
 * test_byte_readers below pin down the helpers themselves; the rest is code
 * review, not test coverage.
 */

#include "fixture.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;
static const char *current;

#define CHECK(cond) do {                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define CHECK_EQ(got, want) do {                                             \
        unsigned long long g = (unsigned long long)(got);                     \
        unsigned long long w = (unsigned long long)(want);                    \
        checks++;                                                            \
        if (g != w) {                                                        \
            failures++;                                                      \
            printf("  FAIL %s:%d  %s: got %llu, want %llu\n",                \
                   __FILE__, __LINE__, #got, g, w);                          \
        }                                                                    \
    } while (0)

#define TEST(name) do { current = name; printf("%s\n", name); } while (0)

/* Convenience: parse an image and return the result. */
static pt_result run(img *m, pt_partition *parts, unsigned int max,
                     pt_table *tbl)
{
    pt_device  dev;
    static pt_scratch scratch;

    img_dev(m, &dev, m);
    return pt_parse(&dev, &scratch, parts, max, tbl);
}

/* ------------------------------------------------------------------ *
 * Helpers themselves
 * ------------------------------------------------------------------ */

static void test_crc32_known_answer(void)
{
    /* The canonical CRC-32/ISO-HDLC check value for "123456789". */
    const char *s = "123456789";
    TEST("crc32 known answer");
    CHECK_EQ(pt_crc32(0, (const pt_u8 *)s, 9), 0xCBF43926UL);
    CHECK_EQ(pt_crc32(0, (const pt_u8 *)"", 0), 0UL);
}

/* ------------------------------------------------------------------ *
 * GPT: the happy paths
 * ------------------------------------------------------------------ */

static void test_gpt_spec_guid(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[1];

    TEST("GPT: spec-order Amiga GUID becomes a unit");
    img_init(&m, 512, 4096);
    parts[0].type  = fx_guid_amiga_spec;
    parts[0].first = 2048;
    parts[0].last  = 3000;
    parts[0].name  = "DOS3:Work";
    img_gpt(&m, parts, 1, 128, 128);

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.scheme, PT_SCHEME_GPT);
    CHECK_EQ(t.count, 1);
    CHECK_EQ(t.amiga_count, 1);
    CHECK_EQ(p[0].is_amiga, 1);
    CHECK_EQ(p[0].guid_byteswapped, 0);
    CHECK_EQ(p[0].start_lba, 2048);
    CHECK_EQ(p[0].block_count, 953);      /* inclusive end: 3000-2048+1 */
    CHECK(strcmp(p[0].name, "DOS3:Work") == 0);
    img_free(&m);
}

static void test_gpt_winuae_guid(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[1];

    TEST("GPT: WinUAE byte order is also accepted, and flagged");
    img_init(&m, 512, 4096);
    parts[0].type  = fx_guid_amiga_winuae;
    parts[0].first = 2048;
    parts[0].last  = 3000;
    parts[0].name  = "Swapped";
    img_gpt(&m, parts, 1, 128, 128);

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.amiga_count, 1);
    CHECK_EQ(p[0].guid_byteswapped, 1);
    img_free(&m);
}

static void test_gpt_foreign_types_are_not_units(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[3];

    TEST("GPT: FAT/Linux partitions are seen but never become units");
    img_init(&m, 512, 8192);
    parts[0].type = fx_guid_efi_system; parts[0].first = 2048;
    parts[0].last = 4095;               parts[0].name  = "EFI";
    parts[1].type = fx_guid_linux;      parts[1].first = 4096;
    parts[1].last = 6143;               parts[1].name  = "rootfs";
    parts[2].type = fx_guid_amiga_spec; parts[2].first = 6144;
    parts[2].last = 7000;               parts[2].name  = "DOS3:Amiga";
    img_gpt(&m, parts, 3, 128, 128);

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.count, 3);
    CHECK_EQ(t.amiga_count, 1);
    CHECK_EQ(p[0].is_amiga, 0);
    CHECK_EQ(p[1].is_amiga, 0);
    CHECK_EQ(p[2].is_amiga, 1);
    img_free(&m);
}

static void test_gpt_gaps_in_entry_array(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[3];

    TEST("GPT: gaps between used entries are tolerated");
    /*
     * NumEntries is the *preallocated* count, not the used count, and nothing
     * requires used entries to be contiguous. AROS's comment notes that real
     * editors tend to squeeze the table but that gaps must still be handled;
     * a parser that stops at the first all-zero entry would miss partition B.
     */
    img_init(&m, 512, 8192);
    parts[0].type = fx_guid_amiga_spec; parts[0].first = 2048;
    parts[0].last = 3000;               parts[0].name  = "A";
    parts[1].type = NULL;               /* hole: left as an unused entry */
    parts[2].type = fx_guid_amiga_spec; parts[2].first = 4001;
    parts[2].last = 5000;               parts[2].name  = "B";
    img_gpt(&m, parts, 3, 128, 128);

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.scheme, PT_SCHEME_GPT);
    CHECK_EQ(t.count, 2);
    CHECK_EQ(t.amiga_count, 2);
    CHECK_EQ(p[0].start_lba, 2048);
    CHECK_EQ(p[1].start_lba, 4001);
    CHECK_EQ(p[1].table_index, 2);   /* entry 2, not entry 1 */
    img_free(&m);
}

static void test_gpt_4kn(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[1];

    TEST("GPT: 4Kn block size");
    img_init(&m, 4096, 1024);
    parts[0].type  = fx_guid_amiga_spec;
    parts[0].first = 64;
    parts[0].last  = 500;
    parts[0].name  = "FourK";
    img_gpt(&m, parts, 1, 128, 128);

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.scheme, PT_SCHEME_GPT);
    CHECK_EQ(t.amiga_count, 1);
    CHECK_EQ(p[0].start_lba, 64);
    CHECK_EQ(p[0].block_count, 437);
    img_free(&m);
}

static void test_gpt_large_entry_size(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[1];

    TEST("GPT: EntrySize is honoured, not assumed to be 128");
    img_init(&m, 512, 8192);
    parts[0].type  = fx_guid_amiga_spec;
    parts[0].first = 2048;
    parts[0].last  = 3000;
    parts[0].name  = "Big";
    /* 256-byte entries: entries no longer sit 4-to-a-block. */
    img_gpt(&m, parts, 1, 128, 256);

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.amiga_count, 1);
    CHECK_EQ(p[0].start_lba, 2048);
    img_free(&m);
}

/* ------------------------------------------------------------------ *
 * GPT: hardening
 * ------------------------------------------------------------------ */

static void test_gpt_oversized_header_size(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[1];

    TEST("HARDENING: HeaderSize larger than a block is rejected, not CRC'd");
    img_init(&m, 512, 4096);
    parts[0].type  = fx_guid_amiga_spec;
    parts[0].first = 2048;
    parts[0].last  = 3000;
    parts[0].name  = "A";
    img_gpt(&m, parts, 1, 128, 128);

    /* Claim a 64 KB header. AROS would CRC over all of it, far past its
     * one-block buffer. We must refuse before reading a byte of it. */
    wr_le32(img_blk(&m, 1) + 12, 65536);
    /* Break the backup too, so there is no valid GPT left at all. */
    wr_le32(img_blk(&m, m.blocks - 1) + 12, 65536);

    /* No usable GPT; the protective MBR has no Amiga partitions. */
    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.scheme, PT_SCHEME_MBR);
    CHECK_EQ(t.amiga_count, 0);
    img_free(&m);
}

static void test_gpt_undersized_header_size(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[1];

    TEST("HARDENING: HeaderSize below the 92-byte minimum is rejected");
    img_init(&m, 512, 4096);
    parts[0].type = fx_guid_amiga_spec; parts[0].first = 2048;
    parts[0].last = 3000;               parts[0].name  = "A";
    img_gpt(&m, parts, 1, 128, 128);
    wr_le32(img_blk(&m, 1) + 12, 64);
    wr_le32(img_blk(&m, m.blocks - 1) + 12, 64);

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.scheme, PT_SCHEME_MBR);
    img_free(&m);
}

static void test_gpt_backup_used_when_primary_crc_bad(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[1];

    TEST("HARDENING: backup header is used when the primary CRC fails");
    img_init(&m, 512, 4096);
    parts[0].type  = fx_guid_amiga_spec;
    parts[0].first = 2048;
    parts[0].last  = 3000;
    parts[0].name  = "Recovered";
    img_gpt(&m, parts, 1, 128, 128);
    img_gpt_break_header_crc(&m, 1);

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.scheme, PT_SCHEME_GPT);
    CHECK_EQ(t.note, PT_NOTE_GPT_BACKUP_USED);
    CHECK_EQ(t.amiga_count, 1);
    CHECK_EQ(p[0].start_lba, 2048);
    img_free(&m);
}

static void test_gpt_backup_used_when_primary_signature_gone(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[1];

    TEST("HARDENING: backup is tried on ANY primary failure, not only bad CRC");
    img_init(&m, 512, 4096);
    parts[0].type  = fx_guid_amiga_spec;
    parts[0].first = 2048;
    parts[0].last  = 3000;
    parts[0].name  = "Recovered";
    img_gpt(&m, parts, 1, 128, 128);
    /* Obliterate the primary signature entirely. AROS gives up here. */
    memset(img_blk(&m, 1), 0, 8);

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.scheme, PT_SCHEME_GPT);
    CHECK_EQ(t.note, PT_NOTE_GPT_BACKUP_USED);
    CHECK_EQ(t.amiga_count, 1);
    img_free(&m);
}

static void test_gpt_backup_used_when_primary_unreadable(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[1];

    TEST("HARDENING: backup is tried when LBA 1 cannot be read at all");
    img_init(&m, 512, 4096);
    parts[0].type  = fx_guid_amiga_spec;
    parts[0].first = 2048;
    parts[0].last  = 3000;
    parts[0].name  = "Recovered";
    img_gpt(&m, parts, 1, 128, 128);
    m.fail_lba = 1;

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.scheme, PT_SCHEME_GPT);
    CHECK_EQ(t.note, PT_NOTE_GPT_BACKUP_USED);
    CHECK_EQ(t.amiga_count, 1);
    img_free(&m);
}

static void test_gpt_both_headers_bad(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[1];

    TEST("HARDENING: both headers bad means no GPT, and no partial results");
    img_init(&m, 512, 4096);
    parts[0].type = fx_guid_amiga_spec; parts[0].first = 2048;
    parts[0].last = 3000;               parts[0].name  = "A";
    img_gpt(&m, parts, 1, 128, 128);
    img_gpt_break_header_crc(&m, 1);
    img_gpt_break_header_crc(&m, m.blocks - 1);

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.scheme, PT_SCHEME_MBR);   /* fell through to the protective MBR */
    CHECK_EQ(t.amiga_count, 0);
    img_free(&m);
}

static void test_gpt_entry_array_crc_bad(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[1];

    TEST("HARDENING: a bad entry-array CRC invalidates the table");
    img_init(&m, 512, 4096);
    parts[0].type = fx_guid_amiga_spec; parts[0].first = 2048;
    parts[0].last = 3000;               parts[0].name  = "A";
    img_gpt(&m, parts, 1, 128, 128);
    /* Corrupt a byte the CRC covers but no field validates. */
    img_blk(&m, 2)[16] ^= 0xFF;
    img_blk(&m, m.blocks - 1 - 32)[16] ^= 0xFF;

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.scheme, PT_SCHEME_MBR);
    CHECK_EQ(t.amiga_count, 0);
    img_free(&m);
}

static void test_gpt_absurd_num_entries(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[1];

    TEST("HARDENING: an absurd NumEntries is rejected before any arithmetic");
    img_init(&m, 512, 4096);
    parts[0].type = fx_guid_amiga_spec; parts[0].first = 2048;
    parts[0].last = 3000;               parts[0].name  = "A";
    img_gpt(&m, parts, 1, 128, 128);

    /* 0x02000000 * 128 overflows 32 bits exactly. AROS would allocate a
     * small buffer and then CRC the full extent. */
    wr_le32(img_blk(&m, 1) + 80, 0x02000000UL);
    img_gpt_fix_header_crc(&m, 1);
    wr_le32(img_blk(&m, m.blocks - 1) + 80, 0x02000000UL);
    img_gpt_fix_header_crc(&m, m.blocks - 1);

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.scheme, PT_SCHEME_MBR);
    CHECK_EQ(t.amiga_count, 0);
    img_free(&m);
}

static void test_gpt_bad_entry_size(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[1];

    TEST("HARDENING: EntrySize not a multiple of 128 is rejected");
    img_init(&m, 512, 4096);
    parts[0].type = fx_guid_amiga_spec; parts[0].first = 2048;
    parts[0].last = 3000;               parts[0].name  = "A";
    img_gpt(&m, parts, 1, 128, 128);
    wr_le32(img_blk(&m, 1) + 84, 100);
    img_gpt_fix_header_crc(&m, 1);
    wr_le32(img_blk(&m, m.blocks - 1) + 84, 100);
    img_gpt_fix_header_crc(&m, m.blocks - 1);

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.scheme, PT_SCHEME_MBR);
    img_free(&m);
}

static void test_gpt_entry_end_before_start(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[2];

    TEST("HARDENING: an entry with LastLBA < FirstLBA is skipped, not wrapped");
    img_init(&m, 512, 8192);
    parts[0].type = fx_guid_amiga_spec; parts[0].first = 5000;
    parts[0].last = 2048;               parts[0].name  = "Backwards";
    parts[1].type = fx_guid_amiga_spec; parts[1].first = 6000;
    parts[1].last = 7000;               parts[1].name  = "Good";
    img_gpt(&m, parts, 2, 128, 128);

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.scheme, PT_SCHEME_GPT);
    CHECK_EQ(t.amiga_count, 1);
    CHECK_EQ(p[0].start_lba, 6000);
    img_free(&m);
}

static void test_gpt_entry_past_end_of_device(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[2];

    TEST("HARDENING: an entry extending past the device is skipped");
    img_init(&m, 512, 4096);
    parts[0].type = fx_guid_amiga_spec; parts[0].first = 2048;
    parts[0].last = 99999;              parts[0].name  = "TooBig";
    parts[1].type = fx_guid_amiga_spec; parts[1].first = 2500;
    parts[1].last = 3000;               parts[1].name  = "Good";
    img_gpt(&m, parts, 2, 128, 128);

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.amiga_count, 1);
    CHECK_EQ(p[0].start_lba, 2500);
    img_free(&m);
}

static void test_gpt_entry_at_lba_zero(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[1];

    TEST("HARDENING: an entry starting at LBA 0 is skipped");
    img_init(&m, 512, 4096);
    parts[0].type = fx_guid_amiga_spec; parts[0].first = 0;
    parts[0].last = 3000;               parts[0].name  = "OverTable";
    img_gpt(&m, parts, 1, 128, 128);

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.amiga_count, 0);
    img_free(&m);
}

static void test_truncation_is_reported(void)
{
    img          m;
    pt_partition p[2];
    pt_table     t;
    fx_gpt_part  parts[5];
    int          i;

    TEST("HARDENING: truncation is reported, never silent");
    img_init(&m, 512, 16384);
    for (i = 0; i < 5; i++) {
        parts[i].type  = fx_guid_amiga_spec;
        parts[i].first = 2048 + (pt_u64)i * 1000;
        parts[i].last  = 2048 + (pt_u64)i * 1000 + 499;
        parts[i].name  = "P";
    }
    img_gpt(&m, parts, 5, 128, 128);

    CHECK_EQ(run(&m, p, 2, &t), PT_OK);
    CHECK_EQ(t.count, 2);
    CHECK_EQ(t.seen, 5);
    CHECK_EQ(t.note, PT_NOTE_ENTRIES_TRUNCATED);
    img_free(&m);
}

/* ------------------------------------------------------------------ *
 * MBR and EBR
 * ------------------------------------------------------------------ */

static void test_mbr_amiga_types(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;

    TEST("MBR: 0x76 and 0x30 become units, 0x78 and 0x0C do not");
    img_init(&m, 512, 8192);
    img_mbr_init(&m);
    img_mbr_entry(&m, 0, 0x0C, 2048, 1024);   /* FAT32 LBA: not ours */
    img_mbr_entry(&m, 1, 0x76, 3072, 1024);   /* Amithlon/Emu68 */
    img_mbr_entry(&m, 2, 0x30, 4096, 1024);   /* WinUAE also accepts this */
    img_mbr_entry(&m, 3, 0x78, 5120, 1024);   /* the open question: not yet */

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.scheme, PT_SCHEME_MBR);
    CHECK_EQ(t.count, 4);
    CHECK_EQ(t.amiga_count, 2);
    CHECK_EQ(p[0].is_amiga, 0);
    CHECK_EQ(p[1].is_amiga, 1);
    CHECK_EQ(p[1].mbr_type, 0x76);
    CHECK_EQ(p[2].is_amiga, 1);
    CHECK_EQ(p[2].mbr_type, 0x30);
    CHECK_EQ(p[3].is_amiga, 0);   /* 0x78 deliberately not accepted yet */
    img_free(&m);
}

static void test_mbr_no_signature(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;

    TEST("MBR: a missing 0x55AA signature means no table");
    img_init(&m, 512, 4096);
    img_mbr_entry(&m, 0, 0x76, 2048, 1024);   /* entries but no signature */

    CHECK_EQ(run(&m, p, 8, &t), PT_ERR_NO_TABLE);
    img_free(&m);
}

static void test_fat_superfloppy(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;

    TEST("HARDENING: a FAT superfloppy is not parsed as an MBR");
    img_init(&m, 512, 4096);
    img_fat_bpb(&m);

    CHECK_EQ(run(&m, p, 8, &t), PT_ERR_NO_TABLE);
    CHECK_EQ(t.note, PT_NOTE_FAT_SUPERFLOPPY);
    img_free(&m);
}

static void test_ebr_chain(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;

    TEST("EBR: a two-link chain yields two logical partitions");
    img_init(&m, 512, 16384);
    img_mbr_init(&m);
    img_mbr_entry(&m, 0, 0x76, 2048, 1024);     /* a primary, for contrast */
    img_mbr_entry(&m, 1, 0x05, 4096, 8192);     /* extended, base 4096 */

    /* First EBR at the extended base. Entry 0's start is relative to THIS
     * EBR; entry 1's link is relative to the extended base. */
    img_ebr(&m, 4096, 0x76, 63, 1000, 0x05, 2048, 4096);
    img_ebr(&m, 4096 + 2048, 0x83, 63, 1000, 0x00, 0, 0);

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.count, 3);
    CHECK_EQ(t.amiga_count, 2);
    /* primary */
    CHECK_EQ(p[0].start_lba, 2048);
    CHECK_EQ(p[0].is_logical, 0);
    /* first logical: 4096 + 63 */
    CHECK_EQ(p[1].start_lba, 4159);
    CHECK_EQ(p[1].is_logical, 1);
    CHECK_EQ(p[1].is_amiga, 1);
    /* second logical: (4096+2048) + 63 */
    CHECK_EQ(p[2].start_lba, 6207);
    CHECK_EQ(p[2].is_amiga, 0);   /* 0x83 Linux */
    img_free(&m);
}

static void test_ebr_zero_link_is_a_clean_terminator(void)
{
    img          m;
    pt_partition p[16];
    pt_table     t;

    TEST("EBR: a zero link offset ends the chain cleanly");
    /*
     * Worth pinning down, because it is why a two-node cycle cannot be
     * expressed at all: link offsets are relative to the extended partition's
     * base, so a link pointing back at the base IS offset zero, which is the
     * standard end-of-chain marker. The degenerate cycle is terminated by the
     * format itself. A real cycle needs three nodes - see the next test.
     */
    img_init(&m, 512, 16384);
    img_mbr_init(&m);
    img_mbr_entry(&m, 0, 0x05, 4096, 8192);
    img_ebr(&m, 4096, 0x76, 63, 1000, 0x05, 0, 4096);

    CHECK_EQ(run(&m, p, 16, &t), PT_OK);
    CHECK_EQ(t.note, PT_NOTE_NONE);     /* a normal ending, not a defect */
    CHECK_EQ(t.amiga_count, 1);
    img_free(&m);
}

static void test_ebr_cycle_terminates(void)
{
    img          m;
    pt_partition p[16];
    pt_table     t;

    TEST("HARDENING: a cyclic EBR chain terminates");
    /*
     * base -> 6144 -> 8192 -> 6144 -> ...
     * AROS loops here forever, allocating a PartitionHandle and an EBRData
     * per iteration until memory is exhausted, reachable from boot.
     */
    img_init(&m, 512, 16384);
    img_mbr_init(&m);
    img_mbr_entry(&m, 0, 0x05, 4096, 8192);      /* extent 4096..12287 */
    img_ebr(&m, 4096, 0x76, 63, 1000, 0x05, 2048, 16);   /* -> 6144 */
    img_ebr(&m, 6144, 0x76, 63, 1000, 0x05, 4096, 16);   /* -> 8192 */
    img_ebr(&m, 8192, 0x76, 63, 1000, 0x05, 2048, 16);   /* -> 6144 again */

    CHECK_EQ(run(&m, p, 16, &t), PT_OK);
    CHECK_EQ(t.note, PT_NOTE_EBR_CHAIN_BROKEN);
    /* The three real logicals seen before the cycle was detected are kept. */
    CHECK_EQ(t.amiga_count, 3);
    img_free(&m);
}

static void test_ebr_backward_link_terminates(void)
{
    img          m;
    pt_partition p[16];
    pt_table     t;

    TEST("HARDENING: a non-increasing EBR link terminates the walk");
    img_init(&m, 512, 16384);
    img_mbr_init(&m);
    img_mbr_entry(&m, 0, 0x05, 4096, 8192);
    img_ebr(&m, 4096, 0x76, 63, 1000, 0x05, 4000, 4096);
    img_ebr(&m, 8096, 0x76, 63, 1000, 0x05, 100, 4096);

    CHECK_EQ(run(&m, p, 16, &t), PT_OK);
    CHECK(t.note == PT_NOTE_EBR_CHAIN_BROKEN);
    img_free(&m);
}

static void test_ebr_long_chain_capped(void)
{
    img          m;
    pt_partition p[128];
    pt_table     t;
    pt_u32       i;

    TEST("HARDENING: an over-long EBR chain is capped and reported");
    img_init(&m, 512, 65536);
    img_mbr_init(&m);
    img_mbr_entry(&m, 0, 0x05, 1024, 60000);

    /* A chain of PT_MAX_EBR_LINKS + 10 links, each 16 blocks further on. */
    for (i = 0; i < PT_MAX_EBR_LINKS + 10; i++) {
        pt_u64 here = 1024 + (pt_u64)i * 16;
        img_ebr(&m, here, 0x76, 1, 8, 0x05, (pt_u32)((i + 1) * 16), 16);
    }

    CHECK_EQ(run(&m, p, 128, &t), PT_OK);
    CHECK_EQ(t.note, PT_NOTE_EBR_CHAIN_CAPPED);
    CHECK_EQ(t.count, PT_MAX_EBR_LINKS);
    img_free(&m);
}

static void test_ebr_link_out_of_bounds(void)
{
    img          m;
    pt_partition p[16];
    pt_table     t;

    TEST("HARDENING: an EBR link outside the extended partition is refused");
    img_init(&m, 512, 16384);
    img_mbr_init(&m);
    img_mbr_entry(&m, 0, 0x05, 4096, 1024);   /* extent is 4096..5119 */
    img_ebr(&m, 4096, 0x76, 63, 500, 0x05, 9000, 16);  /* link to 13096 */

    CHECK_EQ(run(&m, p, 16, &t), PT_OK);
    CHECK_EQ(t.note, PT_NOTE_EBR_CHAIN_BROKEN);
    CHECK_EQ(t.amiga_count, 1);
    img_free(&m);
}

/* ------------------------------------------------------------------ *
 * Hybrid MBR
 * ------------------------------------------------------------------ */

static void test_hybrid_mbr_gpt_wins(void)
{
    img          m;
    pt_partition p[8];
    pt_table     t;
    fx_gpt_part  parts[1];

    TEST("HARDENING: with a valid GPT the MBR is ignored entirely");
    img_init(&m, 512, 8192);
    parts[0].type  = fx_guid_amiga_spec;
    parts[0].first = 2048;
    parts[0].last  = 3000;
    parts[0].name  = "FromGPT";
    img_gpt(&m, parts, 1, 128, 128);

    /* Now make it a hybrid: real 0x76 entries alongside the protective one.
     * brcm-emmc.device emits BOTH sets of units from a card like this. */
    img_mbr_entry(&m, 1, 0x76, 4096, 1024);
    img_mbr_entry(&m, 2, 0x76, 5120, 1024);

    CHECK_EQ(run(&m, p, 8, &t), PT_OK);
    CHECK_EQ(t.scheme, PT_SCHEME_GPT);
    CHECK_EQ(t.note, PT_NOTE_MBR_IGNORED_HYBRID);
    CHECK_EQ(t.count, 1);
    CHECK_EQ(t.amiga_count, 1);
    CHECK_EQ(p[0].start_lba, 2048);
    img_free(&m);
}

/* ------------------------------------------------------------------ *
 * RDB sniff
 * ------------------------------------------------------------------ */

static void sniff(img *m, int *found, pt_u32 *at)
{
    pt_device         dev;
    static pt_scratch scratch;

    img_dev(m, &dev, m);
    CHECK_EQ(pt_sniff_rdb(&dev, &scratch, found, at), PT_OK);
}

static void test_rdb_at_block_zero(void)
{
    img    m;
    int    found;
    pt_u32 at;

    TEST("RDB: found at block 0");
    img_init(&m, 512, 4096);
    img_rdb(&m, 0, 0x5244534BUL, 1);
    sniff(&m, &found, &at);
    CHECK_EQ(found, 1);
    CHECK_EQ(at, 0);
    img_free(&m);
}

static void test_rdb_behind_mbr(void)
{
    img    m;
    int    found;
    pt_u32 at;

    TEST("RDB: an MBR at block 0 does not hide an RDB at block 1");
    img_init(&m, 512, 4096);
    img_mbr_init(&m);
    img_mbr_entry(&m, 0, 0x0C, 2048, 1024);
    img_rdb(&m, 1, 0x5244534BUL, 1);
    sniff(&m, &found, &at);
    CHECK_EQ(found, 1);
    CHECK_EQ(at, 1);
    img_free(&m);
}

static void test_rdb_at_block_20(void)
{
    img    m;
    int    found;
    pt_u32 at;

    TEST("HARDENING: the scan reaches block 20, not just the documented 16");
    img_init(&m, 512, 4096);
    img_mbr_init(&m);
    img_rdb(&m, 20, 0x5244534BUL, 1);
    sniff(&m, &found, &at);
    CHECK_EQ(found, 1);
    CHECK_EQ(at, 20);
    img_free(&m);
}

static void test_rdb_at_block_62(void)
{
    img    m;
    int    found;
    pt_u32 at;

    TEST("RDB: block 62 is in range, block 63 is not");
    img_init(&m, 512, 4096);
    img_rdb(&m, 62, 0x5244534BUL, 1);
    sniff(&m, &found, &at);
    CHECK_EQ(found, 1);
    CHECK_EQ(at, 62);
    img_free(&m);

    img_init(&m, 512, 4096);
    img_rdb(&m, 63, 0x5244534BUL, 1);
    sniff(&m, &found, &at);
    CHECK_EQ(found, 0);
    img_free(&m);
}

static void test_rdb_bad_checksum_is_not_found(void)
{
    img    m;
    int    found;
    pt_u32 at;

    TEST("HARDENING: RDSK magic with a bad checksum does not trigger back-off");
    img_init(&m, 512, 4096);
    img_rdb(&m, 1, 0x5244534BUL, 0);
    sniff(&m, &found, &at);
    CHECK_EQ(found, 0);
    img_free(&m);
}

static void test_rdb_scrambled_forms(void)
{
    img    m;
    int    found;
    pt_u32 at;

    TEST("RDB: byteswapped DRKS and ADIDE CPRM also trigger back-off");

    img_init(&m, 512, 4096);
    img_rdb(&m, 2, 0x44524B53UL, 0);     /* DRKS: no valid checksum to have */
    sniff(&m, &found, &at);
    CHECK_EQ(found, 1);
    CHECK_EQ(at, 2);
    img_free(&m);

    img_init(&m, 512, 4096);
    {
        pt_u8 *b = img_blk(&m, 3);
        b[0] = 0x39; b[1] = 0x10; b[2] = 0xD3; b[3] = 0x12;
    }
    sniff(&m, &found, &at);
    CHECK_EQ(found, 1);
    CHECK_EQ(at, 3);
    img_free(&m);
}

static void test_rdb_summedlongs_clamped(void)
{
    img    m;
    int    found;
    pt_u32 at;
    pt_u8 *b;

    TEST("HARDENING: an absurd rdb_SummedLongs cannot drive an over-read");
    img_init(&m, 512, 4096);
    img_rdb(&m, 1, 0x5244534BUL, 1);
    b = img_blk(&m, 1);
    /* brcm-emmc.device uses this unchecked as a loop bound over 512 bytes. */
    wr_be32(b + 4, 0x40000000UL);
    sniff(&m, &found, &at);
    CHECK_EQ(found, 0);        /* rejected, and no crash under a sanitiser */
    img_free(&m);
}

static void test_rdb_absent(void)
{
    img    m;
    int    found;
    pt_u32 at;

    TEST("RDB: a plain PC-partitioned disk has none");
    img_init(&m, 512, 4096);
    img_mbr_init(&m);
    img_mbr_entry(&m, 0, 0x0C, 2048, 1024);
    sniff(&m, &found, &at);
    CHECK_EQ(found, 0);
    img_free(&m);
}

/* ------------------------------------------------------------------ *
 * Parameter validation
 * ------------------------------------------------------------------ */

static void test_bad_params(void)
{
    img          m;
    pt_partition p[4];
    pt_table     t;
    pt_device    dev;
    pt_scratch   scratch;

    TEST("bad parameters are refused");
    img_init(&m, 512, 4096);
    img_dev(&m, &dev, &m);

    CHECK_EQ(pt_parse(NULL, &scratch, p, 4, &t), PT_ERR_PARAM);
    CHECK_EQ(pt_parse(&dev, NULL, p, 4, &t), PT_ERR_PARAM);
    CHECK_EQ(pt_parse(&dev, &scratch, NULL, 4, &t), PT_ERR_PARAM);
    CHECK_EQ(pt_parse(&dev, &scratch, p, 0, &t), PT_ERR_PARAM);
    CHECK_EQ(pt_parse(&dev, &scratch, p, 4, NULL), PT_ERR_PARAM);

    dev.block_size = 256;       /* below the minimum */
    CHECK_EQ(pt_parse(&dev, &scratch, p, 4, &t), PT_ERR_PARAM);
    dev.block_size = 8192;      /* above PT_MAX_BLOCK_SIZE */
    CHECK_EQ(pt_parse(&dev, &scratch, p, 4, &t), PT_ERR_PARAM);
    dev.block_size = 1536;      /* not a power of two */
    CHECK_EQ(pt_parse(&dev, &scratch, p, 4, &t), PT_ERR_PARAM);

    img_free(&m);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("ptparse tests\n\n");

    test_crc32_known_answer();

    test_gpt_spec_guid();
    test_gpt_winuae_guid();
    test_gpt_foreign_types_are_not_units();
    test_gpt_gaps_in_entry_array();
    test_gpt_4kn();
    test_gpt_large_entry_size();

    test_gpt_oversized_header_size();
    test_gpt_undersized_header_size();
    test_gpt_backup_used_when_primary_crc_bad();
    test_gpt_backup_used_when_primary_signature_gone();
    test_gpt_backup_used_when_primary_unreadable();
    test_gpt_both_headers_bad();
    test_gpt_entry_array_crc_bad();
    test_gpt_absurd_num_entries();
    test_gpt_bad_entry_size();
    test_gpt_entry_end_before_start();
    test_gpt_entry_past_end_of_device();
    test_gpt_entry_at_lba_zero();
    test_truncation_is_reported();

    test_mbr_amiga_types();
    test_mbr_no_signature();
    test_fat_superfloppy();
    test_ebr_chain();
    test_ebr_zero_link_is_a_clean_terminator();
    test_ebr_cycle_terminates();
    test_ebr_backward_link_terminates();
    test_ebr_long_chain_capped();
    test_ebr_link_out_of_bounds();

    test_hybrid_mbr_gpt_wins();

    test_rdb_at_block_zero();
    test_rdb_behind_mbr();
    test_rdb_at_block_20();
    test_rdb_at_block_62();
    test_rdb_bad_checksum_is_not_found();
    test_rdb_scrambled_forms();
    test_rdb_summedlongs_clamped();
    test_rdb_absent();

    test_bad_params();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
