/*
 * test_unitmap.c - host tests for the translation, bounds and geometry layer.
 *
 * The bounds checks are the device's defining safety property, so the cases
 * that matter most here are the ones that try to escape a partition: a start
 * past the end, a length running off the end, a 64-bit wrap, and the same
 * three again through the SCSI path, which must not be a back door.
 */

#include "../src/unitmap.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond) do {                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

/* The locals are deliberately ugly: a plain `g` here shadowed the
 * `um_geometry g` in the geometry tests. */
#define CHECK_EQ(got, want) do {                                             \
        long long ceq_got_ = (long long)(got);                               \
        long long ceq_want_ = (long long)(want);                             \
        checks++;                                                            \
        if (ceq_got_ != ceq_want_) {                                         \
            failures++;                                                      \
            printf("  FAIL %s:%d  %s: got %lld, want %lld\n",                \
                   __FILE__, __LINE__, #got, ceq_got_, ceq_want_);           \
        }                                                                    \
    } while (0)

#define TEST(name) printf("%s\n", name)

/* A 1000-block partition starting at LBA 2048, 512-byte blocks. */
static void mk(um_unit *u, pt_u64 start, pt_u64 count, pt_u32 bs)
{
    memset(u, 0, sizeof(*u));
    u->start_lba     = start;
    u->block_count   = count;
    u->block_size    = bs;
    u->writable      = 1;
    u->media_present = 1;
    CHECK_EQ(um_unit_init(u), UM_OK);
}

static void test_unit_numbering(void)
{
    TEST("unit numbering round-trips");
    CHECK_EQ(um_unit_number(0, 0), 0);
    CHECK_EQ(um_unit_number(0, 3), 3);
    CHECK_EQ(um_unit_number(1, 0), 100);
    CHECK_EQ(um_unit_number(2, 17), 217);
    CHECK_EQ(um_unit_disk(217), 2);
    CHECK_EQ(um_unit_part(217), 17);
    CHECK_EQ(um_unit_disk(0), 0);
    CHECK_EQ(um_unit_part(99), 99);
}

static void test_block_shift(void)
{
    um_unit u;

    TEST("block_shift is derived, and odd block sizes are refused");
    mk(&u, 0, 1, 512);
    CHECK_EQ(u.block_shift, 9);
    mk(&u, 0, 1, 1024);
    CHECK_EQ(u.block_shift, 10);
    mk(&u, 0, 1, 4096);
    CHECK_EQ(u.block_shift, 12);

    memset(&u, 0, sizeof(u));
    u.block_size = 1536;                    /* not a power of two */
    CHECK_EQ(um_unit_init(&u), UM_IOERR_BADADDRESS);
    u.block_size = 256;                     /* below the minimum */
    CHECK_EQ(um_unit_init(&u), UM_IOERR_BADADDRESS);
    u.block_size = 8192;                    /* above the maximum */
    CHECK_EQ(um_unit_init(&u), UM_IOERR_BADADDRESS);
}

static void test_translate_basic(void)
{
    um_unit u;
    um_xfer x;

    TEST("translation adds the partition start");
    mk(&u, 2048, 1000, 512);

    /* First block of the partition. */
    CHECK_EQ(um_translate(&u, 0, 512, 0, &x), UM_OK);
    CHECK_EQ(x.abs_lba, 2048);
    CHECK_EQ(x.blocks, 1);
    CHECK_EQ(x.abs_byte_off, 2048ULL * 512);
    CHECK_EQ(x.needs_64bit, 0);

    /* Tenth block, eight blocks long. */
    CHECK_EQ(um_translate(&u, 10 * 512, 8 * 512, 0, &x), UM_OK);
    CHECK_EQ(x.abs_lba, 2058);
    CHECK_EQ(x.blocks, 8);

    /* Exactly the last block. */
    CHECK_EQ(um_translate(&u, 999 * 512, 512, 0, &x), UM_OK);
    CHECK_EQ(x.abs_lba, 2048 + 999);

    /* The whole partition in one request. */
    CHECK_EQ(um_translate(&u, 0, 1000 * 512, 0, &x), UM_OK);
    CHECK_EQ(x.blocks, 1000);
}

static void test_bounds(void)
{
    um_unit u;
    um_xfer x;

    TEST("SAFETY: requests outside the partition are refused");
    mk(&u, 2048, 1000, 512);

    /* One block past the end. */
    CHECK_EQ(um_translate(&u, 1000 * 512, 512, 0, &x), UM_IOERR_BADADDRESS);
    /* Starts inside, runs one block off the end. */
    CHECK_EQ(um_translate(&u, 999 * 512, 2 * 512, 0, &x), UM_IOERR_BADADDRESS);
    /* Starts inside, runs a long way off the end. */
    CHECK_EQ(um_translate(&u, 500 * 512, 1000 * 512, 0, &x),
             UM_IOERR_BADADDRESS);
    /* Absurdly far past the end. */
    CHECK_EQ(um_translate(&u, 0xFFFFFFFFULL * 512, 512, 0, &x),
             UM_IOERR_BADADDRESS);
    /* The whole partition plus one block. */
    CHECK_EQ(um_translate(&u, 0, 1001 * 512, 0, &x), UM_IOERR_BADADDRESS);
}

static void test_bounds_wrap(void)
{
    um_unit u;
    um_xfer x;

    TEST("SAFETY: a 64-bit wrap cannot fake an in-range request");
    mk(&u, 2048, 1000, 512);

    /*
     * An offset near 2^64 whose block count would wrap the addition back
     * into the partition if the sum were not checked first. This is the
     * failure mode the two-part test plus the wrap check exists to stop.
     */
    CHECK_EQ(um_translate(&u, 0xFFFFFFFFFFFFFE00ULL, 512, 0, &x),
             UM_IOERR_BADADDRESS);
    CHECK_EQ(um_translate(&u, 0xFFFFFFFFFFFFF000ULL, 4096, 0, &x),
             UM_IOERR_BADADDRESS);
}

static void test_length_and_alignment(void)
{
    um_unit u;
    um_xfer x;

    TEST("length and offset must be whole blocks");
    mk(&u, 2048, 1000, 512);

    CHECK_EQ(um_translate(&u, 0, 0, 0, &x), UM_IOERR_BADLENGTH);
    CHECK_EQ(um_translate(&u, 0, 1, 0, &x), UM_IOERR_BADLENGTH);
    CHECK_EQ(um_translate(&u, 0, 511, 0, &x), UM_IOERR_BADLENGTH);
    CHECK_EQ(um_translate(&u, 0, 513, 0, &x), UM_IOERR_BADLENGTH);
    /* A misaligned offset is an address error, not a length error. */
    CHECK_EQ(um_translate(&u, 1, 512, 0, &x), UM_IOERR_BADADDRESS);
    CHECK_EQ(um_translate(&u, 256, 512, 0, &x), UM_IOERR_BADADDRESS);
}

static void test_write_protect_and_media(void)
{
    um_unit u;
    um_xfer x;

    TEST("write protect and absent media are reported, in that order");
    mk(&u, 2048, 1000, 512);

    u.writable = 0;
    CHECK_EQ(um_translate(&u, 0, 512, 0, &x), UM_OK);          /* read ok */
    CHECK_EQ(um_translate(&u, 0, 512, 1, &x), UM_TDERR_WriteProt);

    /*
     * Media absent must win over everything, and must be checked before any
     * arithmetic: block_size and block_shift are zero on a unit whose media
     * has gone, so an out-of-range offset must still report DiskChanged
     * rather than BADADDRESS.
     */
    u.media_present = 0;
    CHECK_EQ(um_translate(&u, 0, 512, 0, &x), UM_TDERR_DiskChanged);
    CHECK_EQ(um_translate(&u, 1 << 30, 512, 1, &x), UM_TDERR_DiskChanged);
    CHECK_EQ(um_translate(&u, 0, 0, 0, &x), UM_TDERR_DiskChanged);
}

static void test_needs_64bit(void)
{
    um_unit u;
    um_xfer x;

    TEST("64-bit is selected by the ABSOLUTE address, including the end");

    /* A small partition sitting entirely below 4GB: never 64-bit. */
    mk(&u, 2048, 1000, 512);
    CHECK_EQ(um_translate(&u, 0, 512, 0, &x), UM_OK);
    CHECK_EQ(x.needs_64bit, 0);

    /*
     * The case the proposal singles out, and the one devtest cannot test for
     * us: a SMALL partition whose absolute start is past 4GB. devtest's
     * 4GB sub-tests only run when the unit itself exceeds 4GB.
     */
    mk(&u, 0x900000ULL, 1000, 512);     /* start byte = 4.5GB */
    CHECK_EQ(um_translate(&u, 0, 512, 0, &x), UM_OK);
    CHECK_EQ(x.needs_64bit, 1);
    CHECK_EQ(x.abs_byte_off, 0x900000ULL * 512);

    /*
     * A partition straddling the 4GB line: the first blocks are 32-bit
     * addressable and the later ones are not, so the flag must follow the
     * individual request, not the partition.
     */
    mk(&u, 0x7FFFF0ULL, 64, 512);       /* start byte ~= 4GB - 8KB */
    CHECK_EQ(um_translate(&u, 0, 512, 0, &x), UM_OK);
    CHECK_EQ(x.needs_64bit, 0);
    CHECK_EQ(um_translate(&u, 63 * 512, 512, 0, &x), UM_OK);
    CHECK_EQ(x.needs_64bit, 1);

    /*
     * A single request straddling the boundary: the start is below 4GB but
     * the last byte is above it. Checking only the start would hand a 32-bit
     * command an address it cannot express.
     */
    mk(&u, 0x7FFFFEULL, 8, 512);
    CHECK_EQ(um_translate(&u, 0, 4 * 512, 0, &x), UM_OK);
    CHECK_EQ(x.needs_64bit, 1);
}

static void test_exact_4gb_boundary(void)
{
    um_unit u;
    um_xfer x;

    TEST("the last byte below 4GB is still 32-bit");

    /* Absolute byte offset 0xFFFFFE00, length 512: last byte 0xFFFFFFFF. */
    mk(&u, 0x7FFFFFULL, 2, 512);
    CHECK_EQ(um_translate(&u, 0, 512, 0, &x), UM_OK);
    CHECK_EQ(x.abs_byte_off, 0xFFFFFE00ULL);
    CHECK_EQ(x.needs_64bit, 0);

    /* One block further on is the first that needs 64 bits. */
    CHECK_EQ(um_translate(&u, 512, 512, 0, &x), UM_OK);
    CHECK_EQ(x.abs_byte_off, 0x100000000ULL);
    CHECK_EQ(x.needs_64bit, 1);
}

static void test_4kn(void)
{
    um_unit u;
    um_xfer x;

    TEST("4Kn blocks translate");
    mk(&u, 64, 500, 4096);
    CHECK_EQ(um_translate(&u, 0, 4096, 0, &x), UM_OK);
    CHECK_EQ(x.abs_lba, 64);
    CHECK_EQ(x.blocks, 1);
    CHECK_EQ(x.abs_byte_off, 64ULL * 4096);

    CHECK_EQ(um_translate(&u, 499 * 4096, 4096, 0, &x), UM_OK);
    CHECK_EQ(x.abs_lba, 64 + 499);
    /* 512 is not a whole block here. */
    CHECK_EQ(um_translate(&u, 0, 512, 0, &x), UM_IOERR_BADLENGTH);
    CHECK_EQ(um_translate(&u, 500 * 4096, 4096, 0, &x), UM_IOERR_BADADDRESS);
}

static void test_scsi_is_not_a_back_door(void)
{
    um_unit u;
    pt_u64  abs;

    TEST("SAFETY: the SCSI path is bounded exactly like the TD path");
    mk(&u, 2048, 1000, 512);

    CHECK_EQ(um_translate_scsi(&u, 0, 1, 0, &abs), UM_OK);
    CHECK_EQ(abs, 2048);
    CHECK_EQ(um_translate_scsi(&u, 999, 1, 0, &abs), UM_OK);
    CHECK_EQ(abs, 3047);

    /* The same three escapes as the TD path, and they must all fail. */
    CHECK_EQ(um_translate_scsi(&u, 1000, 1, 0, &abs), UM_IOERR_BADADDRESS);
    CHECK_EQ(um_translate_scsi(&u, 999, 2, 0, &abs), UM_IOERR_BADADDRESS);
    CHECK_EQ(um_translate_scsi(&u, 0, 1001, 0, &abs), UM_IOERR_BADADDRESS);
    CHECK_EQ(um_translate_scsi(&u, 0xFFFFFFFFFFFFFFFFULL, 1, 0, &abs),
             UM_IOERR_BADADDRESS);
    CHECK_EQ(um_translate_scsi(&u, 0, 0, 0, &abs), UM_IOERR_BADLENGTH);

    /* Write protect and media apply here too. */
    u.writable = 0;
    CHECK_EQ(um_translate_scsi(&u, 0, 1, 1, &abs), UM_TDERR_WriteProt);
    u.media_present = 0;
    CHECK_EQ(um_translate_scsi(&u, 0, 1, 0, &abs), UM_TDERR_DiskChanged);
}

static void test_geometry(void)
{
    um_unit     u;
    um_geometry g;

    TEST("geometry: the whole unit is the partition");
    mk(&u, 2048, 1000, 512);
    um_geometry_fill(&u, 0, &g);
    CHECK_EQ(g.total_sectors, 1000);
    CHECK_EQ(g.cylinders, 1000);
    CHECK_EQ(g.heads, 1);
    CHECK_EQ(g.track_sectors, 1);
    CHECK_EQ(g.cyl_sectors, 1);
    CHECK_EQ(g.sector_size, 512);
    CHECK_EQ(g.removable, 0);

    /* The identity that makes a mount entry trivial. */
    CHECK_EQ(g.total_sectors, g.cylinders * g.heads * g.track_sectors);

    um_geometry_fill(&u, 1, &g);
    CHECK_EQ(g.removable, 1);

    mk(&u, 64, 500, 4096);
    um_geometry_fill(&u, 0, &g);
    CHECK_EQ(g.sector_size, 4096);
    CHECK_EQ(g.total_sectors, 500);
}

static void test_geometry_clamps(void)
{
    um_unit     u;
    um_geometry g;

    TEST("geometry: a partition over 2^32 blocks clamps, never wraps");
    mk(&u, 1, 0x100000000ULL, 512);     /* exactly 2^32 blocks */
    um_geometry_fill(&u, 0, &g);
    CHECK_EQ(g.total_sectors, 0xFFFFFFFFUL);
    CHECK_EQ(g.cylinders, 0xFFFFFFFFUL);

    mk(&u, 1, 0x1FFFFFFFFULL, 512);
    um_geometry_fill(&u, 0, &g);
    CHECK_EQ(g.total_sectors, 0xFFFFFFFFUL);

    /* One block below the limit is reported exactly. */
    mk(&u, 1, 0xFFFFFFFFULL, 512);
    um_geometry_fill(&u, 0, &g);
    CHECK_EQ(g.total_sectors, 0xFFFFFFFFUL);
    mk(&u, 1, 0xFFFFFFFEULL, 512);
    um_geometry_fill(&u, 0, &g);
    CHECK_EQ(g.total_sectors, 0xFFFFFFFEUL);
}

static void test_single_block_partition(void)
{
    um_unit u;
    um_xfer x;

    TEST("a one-block partition is addressable and bounded");
    mk(&u, 5000, 1, 512);
    CHECK_EQ(um_translate(&u, 0, 512, 0, &x), UM_OK);
    CHECK_EQ(x.abs_lba, 5000);
    CHECK_EQ(um_translate(&u, 512, 512, 0, &x), UM_IOERR_BADADDRESS);
    CHECK_EQ(um_translate(&u, 0, 1024, 0, &x), UM_IOERR_BADADDRESS);
}

int main(void)
{
    printf("unitmap tests\n\n");

    test_unit_numbering();
    test_block_shift();
    test_translate_basic();
    test_bounds();
    test_bounds_wrap();
    test_length_and_alignment();
    test_write_protect_and_media();
    test_needs_64bit();
    test_exact_4gb_boundary();
    test_4kn();
    test_scsi_is_not_a_back_door();
    test_geometry();
    test_geometry_clamps();
    test_single_block_partition();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
