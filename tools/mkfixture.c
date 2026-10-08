/*
 * mkfixture.c - write a partition-table fixture disk image for the
 *               on-target Copperline test.
 *
 * Host tool. Reuses the same image builders as the host unit tests
 * (tests/fixture.c), so the bytes the Amiga sees are the bytes the unit
 * tests reason about - one definition of what a fixture is.
 *
 * Each Amiga partition is filled with a verifiable pattern rather than
 * zeroes: block N of partition P holds a 16-byte header naming P and N,
 * then a byte sequence derived from both. That lets the on-target test
 * prove it read the RIGHT blocks, not merely that a read succeeded -
 * which is the whole question for an offset-translating filter device.
 *
 *   mkfixture mbr  out.hdf    MBR, one FAT-ish decoy + two 0x76 partitions
 *   mkfixture gpt  out.hdf    GPT, one EFI decoy + two Amiga-GUID partitions
 */

#include "../tests/fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Must match the verifier in tests/copperline/puttest.c. Kept deliberately
 * simple so the Amiga side needs no arithmetic a 68000 would labour over.
 */
void fx_fill_pattern(pt_u8 *block, pt_u32 bs, int part, pt_u32 blockno)
{
    pt_u32 i;

    memset(block, 0, bs);
    memcpy(block, "PARTUNIT", 8);
    block[8]  = (pt_u8)part;
    block[9]  = 0;
    wr_le32(block + 10, blockno);
    block[14] = 0xA5;
    block[15] = 0x5A;

    for (i = 16; i < bs; i++) {
        block[i] = (pt_u8)((i + blockno * 7u + (pt_u32)part * 31u) & 0xFF);
    }
}

static void fill_partition(img *m, int part, pt_u64 start, pt_u64 count)
{
    pt_u64 b;

    for (b = 0; b < count; b++) {
        fx_fill_pattern(img_blk(m, start + b), m->bs, part, (pt_u32)b);
    }
}

static int write_out(const img *m, const char *path)
{
    FILE *f = fopen(path, "wb");
    size_t want = (size_t)(m->bs * m->blocks);

    if (f == NULL) {
        perror(path);
        return 1;
    }
    if (fwrite(m->data, 1, want, f) != want) {
        perror(path);
        fclose(f);
        return 1;
    }
    fclose(f);
    printf("%s: %llu blocks of %u bytes (%llu KiB)\n", path,
           (unsigned long long)m->blocks, (unsigned)m->bs,
           (unsigned long long)(want / 1024));
    return 0;
}

/*
 * 8 MiB, 512-byte blocks. Small enough to build and boot quickly, large
 * enough that the partitions are realistically offset rather than sitting
 * at block 1.
 */
#define FIX_BLOCKS 16384

static int build_mbr(const char *path)
{
    img m;

    img_init(&m, 512, FIX_BLOCKS);
    img_mbr_init(&m);

    /*
     * Slot 0 is a decoy: FAT16, the PC side of a shared card. It must NEVER
     * become a unit, and the bounds tests aim just past partition 1's end
     * at it specifically - a filesystem bug scribbling here is the failure
     * this device exists to prevent.
     */
    img_mbr_entry(&m, 0, 0x06, 2048, 2048);

    /* Two Amiga partitions, one of each accepted type. */
    img_mbr_entry(&m, 1, 0x76, 4096, 1024);
    img_mbr_entry(&m, 2, 0x30, 8192, 512);

    /* Fill the decoy with something recognisable that is NOT our pattern,
     * so a mis-translated read shows up as wrong data rather than as a
     * plausible-looking block. */
    {
        pt_u64 b;
        for (b = 0; b < 2048; b++) {
            memset(img_blk(&m, 2048 + b), 0xEE, 512);
            memcpy(img_blk(&m, 2048 + b), "DECOY-FAT", 9);
        }
    }

    fill_partition(&m, 1, 4096, 1024);
    fill_partition(&m, 2, 8192, 512);

    {
        int rc = write_out(&m, path);
        img_free(&m);
        return rc;
    }
}

static int build_gpt(const char *path)
{
    img         m;
    fx_gpt_part parts[3];

    img_init(&m, 512, FIX_BLOCKS);

    /* Decoy first, so the Amiga partitions are not entry 0 either. */
    parts[0].type  = fx_guid_efi_system;
    parts[0].first = 2048;
    parts[0].last  = 4095;
    parts[0].name  = "EFI";

    parts[1].type  = fx_guid_amiga_spec;
    parts[1].first = 4096;
    parts[1].last  = 5119;
    parts[1].name  = "DOS3:Work";

    /*
     * Deliberately the WinUAE byte order on the second one: a card prepared
     * under WinUAE must work too, and this is the only place that tolerance
     * is exercised end to end rather than in a unit test.
     */
    parts[2].type  = fx_guid_amiga_winuae;
    parts[2].first = 8192;
    parts[2].last  = 8703;
    parts[2].name  = "DOS3:Swapped";

    img_gpt(&m, parts, 3, 128, 128);

    {
        pt_u64 b;
        for (b = 0; b < 2048; b++) {
            memset(img_blk(&m, 2048 + b), 0xEE, 512);
            memcpy(img_blk(&m, 2048 + b), "DECOY-EFI", 9);
        }
    }

    fill_partition(&m, 1, 4096, 1024);
    fill_partition(&m, 2, 8192, 512);

    {
        int rc = write_out(&m, path);
        img_free(&m);
        return rc;
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s mbr|gpt out.hdf\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "mbr") == 0) {
        return build_mbr(argv[2]);
    }
    if (strcmp(argv[1], "gpt") == 0) {
        return build_gpt(argv[2]);
    }
    fprintf(stderr, "unknown layout '%s'\n", argv[1]);
    return 2;
}
