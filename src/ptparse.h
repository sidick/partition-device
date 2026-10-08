/*
 * ptparse.h - MBR / EBR / GPT partition table parser for partunit.device
 *
 * Host-testable: no AmigaOS dependencies, no allocation, no I/O of its own.
 * The caller supplies a block-read callback and all buffers.
 *
 * ENDIANNESS: MBR and GPT fields are little-endian on disk; RDB fields are
 * big-endian. The target (m68k) is big-endian and the development host is
 * not, so every multi-byte field is assembled byte-by-byte by the rd_le
 * and rd_be helpers. Never overlay a struct on a disk buffer in this module,
 * and never memcpy into an integer. Tests running on a little-endian host
 * cannot catch a violation of this rule, so it must hold by construction.
 */

#ifndef PTPARSE_H
#define PTPARSE_H

#include <stddef.h>

/*
 * Fixed-width types. The Amiga cross-compilers in use do not all ship a
 * usable <stdint.h>, so the types are named locally and can be switched to
 * exec/types.h equivalents in the device build without touching the parser.
 */
#ifdef PTPARSE_AMIGA
# include <exec/types.h>
typedef UBYTE            pt_u8;
typedef UWORD            pt_u16;
typedef ULONG            pt_u32;
typedef unsigned long long pt_u64;
#else
# include <stdint.h>
typedef uint8_t          pt_u8;
typedef uint16_t         pt_u16;
typedef uint32_t         pt_u32;
typedef uint64_t         pt_u64;
#endif

/* Hard caps. These exist to bound every loop and allocation in the parser. */
#define PT_MAX_BLOCK_SIZE   4096  /* refuse larger; scratch is sized to this */
#define PT_MIN_BLOCK_SIZE   512
#define PT_MAX_PARTITIONS   128   /* caller's array may be smaller */
#define PT_MAX_GPT_ENTRIES  512   /* NumEntries above this is rejected       */
#define PT_MAX_EBR_LINKS    64    /* EBR chain iteration cap                 */
#define PT_RDB_SCAN_BLOCKS  63    /* blocks 0..62, matching WinUAE/Amiberry  */
#define PT_GPT_NAME_CHARS   36    /* 72 bytes of UTF-16LE                    */

/* Return codes. */
typedef enum {
    PT_OK = 0,
    PT_ERR_IO,            /* the read callback failed                        */
    PT_ERR_PARAM,         /* bad arguments or unsupported block size         */
    PT_ERR_NO_TABLE,      /* no MBR or GPT found                             */
    PT_ERR_TRUNCATED,     /* caller's partition array was too small          */
    PT_ERR_CORRUPT        /* a table was present but failed validation       */
} pt_result;

/* Which scheme the partitions came from. */
typedef enum {
    PT_SCHEME_NONE = 0,
    PT_SCHEME_MBR,
    PT_SCHEME_GPT
} pt_scheme;

/*
 * Why a disk was refused, or a note about how it was parsed. Surfaced by the
 * tool's LIST so a user can tell "no Amiga partitions here" from "backed off
 * because the OS owns this disk".
 */
typedef enum {
    PT_NOTE_NONE = 0,
    PT_NOTE_RDB_WHOLE_DISK,    /* RDSK found on the raw disk: back off       */
    PT_NOTE_GPT_BACKUP_USED,   /* primary header bad, backup accepted        */
    PT_NOTE_MBR_IGNORED_HYBRID,/* valid GPT present, MBR deliberately ignored*/
    PT_NOTE_EBR_CHAIN_CAPPED,  /* chain hit PT_MAX_EBR_LINKS                 */
    PT_NOTE_EBR_CHAIN_BROKEN,  /* chain was non-increasing or out of bounds  */
    PT_NOTE_ENTRIES_TRUNCATED, /* more partitions on disk than array slots   */
    PT_NOTE_FAT_SUPERFLOPPY    /* block 0 is a FAT BPB, not a partition table*/
} pt_note;

/* One partition found on the disk. Amiga-typed or not; see is_amiga. */
typedef struct {
    pt_u64 start_lba;      /* absolute, in blocks of dev->block_size         */
    pt_u64 block_count;
    pt_u8  mbr_type;       /* MBR/EBR type byte; 0 under GPT                 */
    pt_u8  is_amiga;       /* 1 if this should become a unit                 */
    pt_u8  is_logical;     /* 1 if it came from the EBR chain                */
    pt_u8  guid_byteswapped; /* GPT: matched WinUAE's byte order, not spec   */
    int    table_index;    /* slot/entry index within its table, for naming  */
    char   name[PT_GPT_NAME_CHARS + 1]; /* GPT name, Latin-1; "" under MBR   */
} pt_partition;

/* Parse results. */
typedef struct {
    pt_scheme    scheme;
    pt_note      note;
    unsigned int count;        /* partitions written to the caller's array   */
    unsigned int amiga_count;  /* how many of those have is_amiga            */
    unsigned int seen;         /* partitions present on disk, before capping */
} pt_table;

/*
 * Read exactly one block of dev->block_size bytes at the given LBA.
 * Returns 0 on success, non-zero on failure. Must not be called by the
 * caller; the parser owns the sequencing.
 */
typedef int (*pt_read_fn)(void *user, pt_u64 lba, void *buf);

typedef struct {
    pt_read_fn read;
    void      *user;
    pt_u32     block_size;    /* PT_MIN_BLOCK_SIZE..PT_MAX_BLOCK_SIZE, pow2  */
    pt_u64     total_blocks;  /* 0 if unknown; bounds checks are skipped     */
} pt_device;

/*
 * Scratch space the parser needs. Caller-owned so the parser never allocates.
 * One block buffer for the table being read, one 128-byte staging area for a
 * GPT entry that straddles a block boundary.
 */
typedef struct {
    pt_u8 block[PT_MAX_BLOCK_SIZE];
    pt_u8 entry[128];
    pt_u64 cached_lba;        /* internal: which LBA is in block[]           */
    int    cache_valid;
} pt_scratch;

/*
 * Parse the partition table.
 *
 * GPT is probed before MBR, and a valid GPT wins outright: the MBR is then
 * ignored entirely (the UEFI rule) and note is set to
 * PT_NOTE_MBR_IGNORED_HYBRID. This is deliberate, and differs from
 * brcm-emmc.device, which can emit both sets of units from one hybrid card.
 *
 * Writes up to max_parts entries into parts, in table order. If the disk
 * holds more than that, PT_OK is still returned with note set to
 * PT_NOTE_ENTRIES_TRUNCATED and tbl->seen giving the true total - never a
 * silent truncation.
 *
 * Does NOT sniff for an RDB; see pt_sniff_rdb. The caller decides the
 * back-off policy.
 */
pt_result pt_parse(const pt_device *dev, pt_scratch *scratch,
                   pt_partition *parts, unsigned int max_parts,
                   pt_table *tbl);

/*
 * Scan blocks 0..PT_RDB_SCAN_BLOCKS-1 of the whole disk for a Rigid Disk
 * Block, to decide whether the OS already owns this disk.
 *
 * Requires a checksum-valid RDSK/CDSK - magic alone produces false positives
 * and would back us off disks we should be parsing. Recognises the byteswapped
 * (DRKS) and ADIDE-scrambled (CPRM) forms, since those are RDBs the OS may
 * still mount. Applies the Win9x-trashed retry (zero bytes 0xDC..0xDF and
 * re-checksum) for detection only - never writes a repair back.
 *
 * Sets *found to 1 if an RDB was found, 0 otherwise, and *at_block to where.
 * The range is 63 blocks, not the documented RDB_LOCATION_LIMIT of 16,
 * because that is what WinUAE's and Amiberry's mount-time scans use; a
 * 16-block scan would miss a disk the OS will happily mount.
 */
pt_result pt_sniff_rdb(const pt_device *dev, pt_scratch *scratch,
                       int *found, pt_u32 *at_block);

/* Also used by the RDB sniff; exposed for tests. */
pt_u32 pt_crc32(pt_u32 seed, const pt_u8 *buf, size_t len);

/* Human-readable note/result text for the tool. Never NULL. */
const char *pt_note_str(pt_note n);
const char *pt_result_str(pt_result r);

#endif /* PTPARSE_H */
