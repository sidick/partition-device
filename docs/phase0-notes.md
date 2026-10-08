# Phase 0 notes

Prior-art reading and convention confirmation for `partition.device`, per
`proposal.md`'s Phase 0. Each section records what was established, with
citations, and flags anything that **changes the proposal**.

All six strands complete: devtest ✅ · Amiberry ✅ · WinUAE ✅ ·
Emu68 + AROS ✅ · Aminet name check ✅ · lide.device ✅

## Decisions taken (2026-10-08)

| Decision | Outcome |
|---|---|
| **Name** | **`partunit.device`** — partitions become units, which is exactly what it does. `partition.device` was verified free on Aminet but was dropped to avoid reading as the device member of the `partition.library` (AROS) / `partition.resource` (Pulchart) family, by a third author. |
| **Relationship to `ptable.library`** | **Build independently — own the parser.** Reconsidered and reversed: `ptable.library` is three weeks old with one author and three packages, so whether it becomes the ecosystem's parsing layer is unknown. A disk-loaded filter device whose defining property is bounds enforcement should not have that guarantee resting on an external dependency that may not be maintained. Registering units in `partition.resource` stays a **later option if demand appears** — additive, soft runtime check, not in any phase. Still open a conversation with the author before release. |
| **Unit numbering** | **Keep `unit = disk × 100 + partition`.** Accepted costs: `devtest -p` will not list disks past the first, and `open()` must return `TDERR_BadUnitNum` for *every* absent unit number so unit-walking scanners do not stop at the gaps (see the lide LUN rule). |

Open, not yet decided: RDB sniff range (0-62 vs 0-15), whether to accept MBR type
`0x30` alongside `0x76`, and hybrid-MBR policy.

> **Two findings dominate everything below. Read these first.**
>
> 1. **`ptable.library` / `partition.resource` (Jaroslav Pulchart) was released
>    2026-10-07 — the day before this proposal was written.** It parses
>    RDB/MBR/GPT and publishes every partition into a shared
>    `partition.resource`, and it already has two consumers shipping
>    (`compactflash.device` 2.0+, `fat95` 4.0+). This is substantially the same
>    problem, solved a different way, by another author, right now. It needs a
>    decision before any code is written. See
>    [Overlapping prior art](#the-decision-ptablelibrary--partitionresource).
> 2. **Emu68 and WinUAE disagree on the GPT type GUID's byte order**, so
>    WinUAE's Amiga-GPT support almost certainly does not work on
>    Emu68-prepared cards. The proposal treats the two as agreeing. See
>    [the GUID section](#-resolved-emu68-and-winuae-disagree-on-the-guid-byte-order).

---

## WinUAE — recognises both conventions, but not with the contract we assumed

Checkout HEAD `c9a38a62`. Headline: **WinUAE has no partition-table parser of
its own.** On Windows it delegates all MBR/GPT parsing to the OS
(`IOCTL_DISK_GET_DRIVE_LAYOUT_EX`) and reads the resulting
`PARTITION_INFORMATION_EX` array; on Unix it does nothing at all
(`od-unix/hardfile_host.cpp:200-213` opens the whole block device with
`hfd->offset = 0` and never looks at sector 0). The only Amiga-side partition
logic in the tree is RDB, plus A2090/MAST "BABE" and ADIDE/byteswapped variants.

### ⚠ The typed partition is expected to contain an RDB, not "whatever"

This is the big one. A matched 0x76 MBR primary or Amiga-GPT partition is opened
as a real-drive hardfile with a base offset
(`od-win32/hardfile_win32.cpp:2156-2159`):

```c
hfd->offset = udi->offset;
hfd->physsize = hfd->virtsize = udi->size;
hfd->ci.blocksize = udi->bytespersector;
if (udi->partitiondrive) hfd->flags |= HFD_FLAGS_REALDRIVEPARTITION;
```

Because `ci.sectors == 0` and `HFD_FLAGS_REALDRIVE` is set, `is_hardfile()`
returns `FILESYS_HARDDRIVE` (`filesys.cpp:552-564`), which routes to
`rdb_mount()` (`filesys.cpp:9056-9061`). **WinUAE therefore scans the first 63
blocks *inside* the typed partition for a checksum-valid `RDSK`, and exposes
each RDB `PART` block as a separate AmigaDOS unit.**

> **Changes the proposal.** The prior-art section asserts the contract is "a
> matching type means the partition is a unit and its entire contents are
> Amiga-owned — whatever is inside (an RDB, a bare filesystem) is the OS's
> business once the unit exists", and attributes that contract to WinUAE as well
> as Emu68. For real media WinUAE does **not** honour the bare-filesystem half:
> a typed partition with a bare FFS filesystem inside and no RDB fails
> `rdb_mount()` with `"failed, no supported partition tables detected"` and is
> not mounted. (The `create_virtual_rdb()` synthesis at
> `hardfile.cpp:387-480` rescues bare-filesystem *HDF files*, not real
> partitions.)
>
> Our design is unaffected in substance — partition.device presents the unit and
> hands RDB contents to the OS `Mounter`, which is WinUAE's behaviour one layer
> up — but two statements need fixing: the claimed contract is Emu68's, not
> jointly WinUAE's, and the "bare filesystem inside a typed partition" case that
> the proposal's in-device mounter exists to serve has **no emulator precedent
> on real media**. That makes it a genuine extension, which is fine, but it
> should be labelled as one rather than as matching an existing convention.

Addressing inside such a partition is fully partition-relative: `hdf_seek()`
adds `hfd->offset` and bounds-checks against `physsize`
(`hardfile_win32.cpp:2381-2390`), so reads past the partition fail rather than
bleeding into the neighbour — the same safety property we are building, and
independent confirmation it is the right one.
`HFD_FLAGS_REALDRIVEPARTITION` also suppresses whole-drive locking
(`hardfile_win32.cpp:1943`) and the first-write warning (`:2709`).

### ⚠ The GPT type GUID byte order is ambiguous, and it affects our fixtures

Exactly one definition, Windows-only, `od-win32/hardfile_win32.cpp:47`:

```c
const static GUID PARTITION_GPT_AMIGA = { 0xbcee823f, 0xc987, 0x9740, { 0x81,0x65,0x89,0xd6,0x54,0x05,0x57,0xc0 } };
```

Added by commit `7e28559e` "GPT Amiga partition type support" (Toni Wilen,
2025-01-04), touching only that file. **No comment explains the value.**

A Windows `GUID` is `{DWORD Data1; WORD Data2; WORD Data3; BYTE Data4[8]}`, and
GPT stores Data1/2/3 little-endian on disk. So:

| | on-disk bytes |
|---|---|
| Canonical string `3F82EEBC-87C9-4097-8165-89D6540557C0`, encoded per the UEFI spec | `BC EE 82 3F · C9 87 · 97 40 · 81 65 89 D6 54 05 57 C0` |
| What WinUAE's constant matches | `3F 82 EE BC · 87 C9 · 40 97 · 81 65 89 D6 54 05 57 C0` |

The comparison is a raw `memcmp` against the GUID as Windows already decoded it
(`hardfile_win32.cpp:1920-1929`), so **the two spellings are not
interchangeable.** WinUAE as it stands matches the type GUID written as a *flat
big-endian byte array*, not the canonical mixed-endian UEFI encoding of the
string the proposal quotes.

> **Changes the proposal and the test plan.** The likely explanation is that the
> canonical-looking string in circulation was produced by reading Emu68's flat
> byte array and hyphenating it. If so, generating fixtures with
> `sgdisk -t N:3F82EEBC-87C9-4097-8165-89D6540557C0` — which encodes
> mixed-endian per spec — produces bytes `BC EE 82 3F …` that **WinUAE will not
> recognise**, and our fixtures would silently test the wrong thing. The
> proposal's testing section names `sgdisk`/`parted` for fixture generation, so
> this must be resolved before Phase 1 writes a single fixture.
>
> Resolution path: (a) the pending Emu68 strand should show which byte order
> Emu68's source writes and compares; (b) confirm empirically against a real
> Emu68-formatted card; (c) regardless of the answer, **accept both byte orders
> on read** — the cost is one extra `memcmp` and it makes us the tolerant
> implementation in an ecosystem that is demonstrably confused. For *writing*
> (a future partition editor), pick whatever Emu68 writes and say so in the docs.

Two distinct use sites, both Windows-only:

- **Enumeration** (`hardfile_win32.cpp:3324-3356`): each matching GPT partition
  becomes its own entry in the harddrive picker, named by its *unique*
  `PartitionId` GUID plus the GPT partition name
  (`:GP#%08x_%04x_…_%s`). Non-matching GPT partitions are skipped entirely, and
  **if no Amiga-type partition exists the whole physical drive is hidden**
  (`goto end`, `:3348-3356`) — GPT drives are never offered whole.
- **Re-resolution** of a configured `:GP#…` name back to an offset
  (`:1818-1824` string→GUID, `:1920-1929` match): requires **both** the type
  GUID and the partition's identity GUID to match.

No GPT handling exists outside `od-win32/`: no `"EFI PART"` string, no header
CRC32, no backup-header fallback, no GPT parsing of HDF files anywhere.

### MBR type 0x76 is Amithlon heritage, and 0x30 goes with it

The proposal credits Emu68 with the `0x76` convention. WinUAE's own comment
names a different origin, and the support long predates Emu68 — present in the
oldest commit in the tree (`dddda495`, the 2020 source import) with identical
wording:

`od-win32/hardfile_win32.cpp:1903`:
```c
// check for amithlon partitions, if none found = quick mount not possible
```

And the type test accepts **two** types (`:3306-3311`):
```c
if (pi->Mbr.PartitionType != 0x76 && pi->Mbr.PartitionType != 0x30) {
    write_log(_T("type not 0x76 or 0x30\n"));
} else {
    write_log(_T("selected\n"));
    udi->partitiondrive = true;
}
```

Later commits: `cc0137e7` "Add harddrive 0x76 partition support updates and
fixes" (2023-09-06), `d2838c08` "Improved 0x76 partition handling, hide GPT
drives" (2023-09-16).

> **Changes the proposal.** Attribute `0x76` to Amithlon (Emu68 adopted it), and
> **decide whether to accept `0x30` as well.** Accepting it is nearly free and
> buys Amithlon-prepared media; the proposal currently names only `0x76`.

Note the asymmetry versus GPT: **every** non-zero-length, Windows-recognised MBR
partition is listed as a selectable unit (named `:P#<PartitionNumber>_<drive>`);
the 0x76/0x30 test only sets `partitiondrive`, it does not filter
(`:3262-3323`). Re-resolution of a stored `:P#N_` name matches on **partition
number with no type check at all** (`:1798-1817`, `:1911-1919`). Also
`gotpart` is never set in the current code (`:3313-3323`), so after the MBR loop
it always falls through to `"non-empty MBR partition table detected, doing RDB
check anyway"` and then `checkrdb:` (`:3357-3368`).

WinUAE never walks an EBR chain itself, but Windows returns logicals in the
layout array with `PartitionNumber >= 5`, so a logical 0x76 is picked up
incidentally as `:P#5_…`. Nothing distinguishes primary from logical.

### RDB detection — three different scan ranges, and 63 is the authoritative one

| Context | Range | Code |
|---|---|---|
| Actual mount (`rdb_mount`) | blocks **0..62**, step `ci.blocksize` | `filesys.cpp:8733-8800` |
| Real-media acceptance (`safetycheck`) | blocks **0..62** | `hardfile_win32.cpp:363` |
| GUI HDF auto-detect | blocks **0..15**, step 512 | `win32gui.cpp:14838-14870` |
| Scramble/byteswap detect (`hdf_read_rdb`) | offsets **< 16 × 512** only | `hardfile.cpp:1234-1255` |

> **Settles the open question raised by the Amiberry strand.** Both emulators
> agree the mount-time range is **0..62**; `0..15` is only a cosmetic GUI sniff
> and the descramble window. Our whole-drive back-off should scan **0..62**, not
> the 1..15 the proposal specifies, or we will fail to back off a disk the OS
> will happily mount.

Two further requirements the proposal does not mention:

- **The `RDSK` checksum must pass, not just the magic.** `rdb_checksum()`
  (`filesys.cpp` / `hardfile_win32.cpp`) verifies the sum-to-zero longword
  checksum *and* that the stored block number matches the block it was found at.
  A bare `memcmp("RDSK", …)` sniff would produce false positives and back off
  disks we should be parsing.
- **Win9x-trashed RDBs** (`filesys.cpp:8762-8789`): if the magic is right but the
  checksum fails, WinUAE zeroes bytes `0xDC..0xDF`, re-checksums, logs
  `"Windows 95/98/ME trashed RDB detected, fixing.."` and **writes the repaired
  block back to the medium**. We should recognise this form for back-off
  purposes (it is an RDB the OS may well mount) but must **never** write — we
  are a read-mostly filter device and repairing another driver's metadata is not
  our business.

Confirmed for the proposal's "one real clash": the `rdb_mount()` loop makes **no
`55 AA` test** and does not stop at block 0 — an MBR at block 0 is simply not
noticed, and the scan continues until a checksum-valid `RDSK`/`CDSK` appears.
Commits `f15dbf73` "Check for RDB even if Windows reports as drive having single
MBR partition" and `734bf6a7` "RDB and MBR combination partition table fix"
exist specifically for this coexistence case, adding the comment `// check if
drive is MBR partitioned with RDB on top of it.` (`:3291`) and changing the
single-partition bail-out from a type test to `StartingOffset == 0`
(`:3270-3272`). So all three sources agree: **MBR at 0 plus RDB above it means
the RDB owns the disk.**

Also: the scan step is `ci.blocksize`, so on a 4Kn drive "block 1" is byte 4096,
not 512 — our block-size inheritance must apply to the sniff, not just to I/O.
`legalrdbblock()` (`filesys.cpp:8169-8176`) requires `block > 0 && block <
virtsize/blocksize`, rejecting block 0 as a PART/FSHD/LSEG target even though
the RDSK itself may sit there.

### Block size sources

- Windows real drives: `dg.BytesPerSector` from `IOCTL_DISK_GET_DRIVE_GEOMETRY`,
  **rejected if `< 512` or `> 2048`** (`hardfile_win32.cpp:3221-3230`), stored
  as `udi->bytespersector` → `hfd->ci.blocksize` (`:2158`). `hdf_seek` enforces
  offset alignment to `ci.blocksize` (`:2390-2400`).
- Unix: `BLKSSZGET` / `DKIOCGETBLOCKSIZE`, default 512
  (`od-unix/hardfile_host.cpp:43-75`).
- HDF files: config, or sniffed from `rdb_blockbytes` at RDSK offset 16
  (`win32gui.cpp:14851-14856`, `is_rdb_block()` `:15395-15407`).

That 2048-byte ceiling is worth noting: WinUAE would refuse a 4Kn drive
outright. We inherit whatever the underlying Amiga device reports and should not
impose a ceiling, but non-512 support is the filesystem's problem, as the
proposal already says.

### No Emu68 interop intent is recorded

`grep -rIniE "pistorm|emu68|brcm|sdhc|emmc"` returns only unrelated hits
(`DwmEnableMMCSS` in `win32gfx.cpp`). No occurrence of the phrase "Amiga
partition type". `ChangeLog` says nothing about GPT, 0x76 or GUIDs. The GPT GUID
literal has no explanatory comment.

> So the shared ground between WinUAE and Emu68 is **Amithlon's `0x76`** plus a
> GPT GUID Toni Wilen took from an unstated upstream — not a negotiated
> convention. The proposal's framing of "the standard to honour" is sound as an
> engineering choice, but it should not imply the ecosystem coordinated on it.

### Useful for testing

Dangerous-drive escape hatches, if we test partition.device against WinUAE with
real media attached: registry key `DangerousDrives`
(`hardfile_win32.cpp:434-460`) and `harddrive_dangerous = 0x1234dead` (`:98`)
bypass the RDB/empty safety gate entirely.

`safetycheck()` (`hardfile_win32.cpp:357-470`) scans blocks 0..62 for, in order:
ADIDE-scrambled `CPRM` (`39 10 D3 12`, returns −3), `"RDSK"` or byteswapped
`"DRKS"` (−1), PCMCIA `CIS@` + `"Commodore\0Amiga\0"` (−2); otherwise it reports
Windows' partition style (−11 GPT, −6 MBR, −10 unknown, −8 not mounted, −9
empty). Its header comment (`:94-97`) is stale, claiming it only accepts drives
with `RDSK` in block 0 or a zeroed block 0 — the same stale comment Amiberry
inherited.

---

## Emu68 — the convention, read from source

**Correction to the proposal's premise:** the device sources are *not* in
`Emu68` or `Emu68-tools`. Both devices have their own repos and are pulled into
Emu68 as prebuilt blobs (`Emu68/src/boards/brcm-sdhc.device.h`, `.../brcm-emmc.device.h`):

- <https://github.com/michalsc/brcm-sdhc.device> (HEAD `0ad06bc`)
- <https://github.com/michalsc/brcm-emmc.device> (HEAD `9f8d46b`, *"brcm-emmc.device
  can support GPT partitions now, not only MBR"*)

### ⚠ RESOLVED: Emu68 and WinUAE disagree on the GUID byte order

`brcm-emmc.device/src/partitions.c:12-14`:

```c
#define GPT_AMIGA_RIGID_DISK_BLOCK \
    { LE32(0x3F82EEBC), LE16(0x87C9), LE16(0x4097), \
    0x81, 0x65, { 0x89, 0xD6, 0x54, 0x05, 0x57, 0xC0 }}
```

with `LE32` = `__builtin_bswap32` (`src/emmc.h:355-356`) and `uuid_t` laid out
`{ULONG time_low; UWORD time_mid; UWORD time_hi_and_version; UBYTE
clock_seq_hi; UBYTE clock_seq_low; UBYTE node[6];}` (`include/common/uuid.h:8-16`).
On the big-endian m68k those swaps make the struct hold the on-disk bytes
`BC EE 82 3F · C9 87 · 97 40 · 81 65 · 89 D6 54 05 57 C0` — i.e. **the standard
UEFI mixed-endian encoding of `3F82EEBC-87C9-4097-8165-89D6540557C0`.**
Comparison is field-by-field against the raw entry (`partitions.c:~205-217`),
with no swapping of the entry.

Set against the WinUAE finding above:

| | on-disk bytes matched |
|---|---|
| **Emu68** (`brcm-emmc.device`) | `BC EE 82 3F C9 87 97 40 81 65 89 D6 54 05 57 C0` ✅ spec-correct |
| **WinUAE** (`hardfile_win32.cpp:47`) | `3F 82 EE BC 87 C9 40 97 81 65 89 D6 54 05 57 C0` ❌ flat byte array |

**These do not match.** Emu68 is correct per the UEFI spec; WinUAE's literal
looks like someone grouped the on-disk byte sequence into GUID fields as though
they were big-endian. The practical consequence is that **WinUAE's Amiga-GPT
support does not recognise an Emu68-prepared card**, and vice versa.

> **Resolution for us.** `sgdisk -t N:3F82EEBC-87C9-4097-8165-89D6540557C0`
> writes the spec encoding, which is Emu68's — so the proposal's fixture plan is
> correct as written, and Emu68 is the authority to follow. **Accept both byte
> orders on read** (one extra `memcmp`) so an Amiga-GPT card prepared under
> WinUAE still works; **write only the spec encoding**. Worth reporting upstream
> to WinUAE — and worth verifying empirically against a real Emu68 card before
> we rely on this analysis.
>
> Also: the proposal's success criterion "presents the same units under
> partition.device as it does under `brcm-emmc.device`" is testable; the
> equivalent claim for WinUAE is not, and should be dropped.

### Geometry: Emu68 does *not* use the 1×1 synthetic geometry

`brcm-sdhc.device/src/io.c:493-511` (and the identical eMMC `io.c:506-520`):

```c
g->dg_SectorSize   = sd_BlockSize;        /* always 512 */
g->dg_TotalSectors = unit->su_BlockCount; /* the PARTITION's length */
g->dg_TrackSectors = SECTOR_COUNT;        /* 64  */
g->dg_Heads        = HEAD_COUNT;          /* 128 */
g->dg_CylSectors   = SECTOR_COUNT * HEAD_COUNT; /* 8192 */
g->dg_Cylinders    = g->dg_TotalSectors / 8192;
g->dg_DeviceType   = DG_DIRECT_ACCESS;
g->dg_Flags        = 0;                   /* non-removable */
```

So it is whole-partition-as-unit (correct, and `su_StartBlock` is invisible to
clients) but with a **fixed 128 heads × 64 sectors** rather than our 1×1.

> **A deliberate divergence to document, not a correction.** Emu68's
> `dg_Cylinders = TotalSectors / 8192` *truncates*, so `dg_TotalSectors !=
> Cylinders × Heads × TrackSectors` and up to 8191 blocks fall off the end of
> any mount entry derived from the cylinder fields. Our 1 surface / 1 block per
> track / cylinders = blocks geometry is exact, loses nothing, and makes mount
> entries trivially `LowCyl 0 / HighCyl blocks-1` as the proposal says. devtest
> checks neither (see above), and AROS independently arrives at the same
> conclusion — `partition_support.c:137-153` falls back to "one block per
> cylinder (flat LBA)" precisely when a partition is not cylinder-aligned, which
> for modern 1 MiB-aligned tables is always. Keep our geometry; note in the docs
> that it differs from Emu68's and why.

Emu68's `HD_SCSICMD` READ CAPACITY reports `su_BlockCount - 1`
(`io.c:341-348`) and MODE SENSE reports the same synthetic 128×64
(`io.c:378-397`) — consistent with the proposal's "substitute the partition's
size".

### ⚠ Emu68's devices mount the RDB themselves — the proposal says they don't

`brcm-sdhc.device/src/unit_task.c:563-719`, `MountPartitions()`, runs in each
unit's task before the I/O loop (`unit_task.c:744`). Per unit it:

1. Scans `su_StartBlock + 0..15` (`RDB_LOCATION_LIMIT`) for
   `IDNAME_RIGIDDISK` and validates the checksum over `rdb_SummedLongs`
   (`unit_task.c:584-605`) — **it does sniff inside the partition for `RDSK`**,
   at partition-relative blocks 0-15.
2. Walks `rdb_PartitionList`, checksums each `PART`, honours `PBFF_NOMOUNT` and
   `PBFF_BOOTABLE`.
3. Builds a 24-longword parameter packet with `paramPkt[2] = su_UnitNum` and
   `pb_Environment[0..19]` verbatim, then calls **`MakeDosNode()` and
   `AddBootNode()`** (`unit_task.c:676-719`). Non-bootable partitions get
   `DE_BOOTPRI = -128` and a NULL ConfigDev.
4. `LoadFilesystem()` (`unit_task.c:243-…`) walks `rdb_FileSysHeaderList`/`LSEG`
   and creates a `FileSysEntry` only if the RDB copy is newer than
   `FileSystem.resource`'s, then applies `fse_PatchFlags` to the DeviceNode.
5. `FixNameConflict()` renames colliding DOS device names.

The eMMC driver has the identical `MountPartitions()` (`src/unittask.c:555-719`).

> **Changes the proposal.** The Mounting section says a unit containing an RDB
> "is left alone: that is the OS's job, done by the OS's own `Mounter` (3.5+) or
> AutoMounter, **exactly as for a `brcm-emmc.device` unit on a PiStorm**." That
> last clause is factually wrong — Emu68's device mounts the RDB itself, in the
> unit task, with `MakeDosNode`/`AddBootNode`.
>
> Our hand-off-to-`Mounter` design may still be the better choice for a
> disk-loaded filter device (we have no diag-time constraint and no reason to
> duplicate `Mounter`), but it is a **divergence from Emu68, not a match**, and
> the justification has to be made on its own merits. The honest framing: Emu68
> is a ROM-resident boot driver and must mount to be bootable; we are
> disk-loaded and post-boot, so mounting RDB contents ourselves would duplicate
> an OS component for no gain.

Normative prose, Emu68-tools-old `README.md:13`: *"Once initialised, it reads
the geometry of SD card, initialises physical and virtual units and subsequently
adds corresponding Boot Nodes in order to make the Amiga partitions on SD card
bootable."*

### Unit numbering: unit 0 is the whole medium, partitions from 1

`brcm-sdhc.device/src/init.c:488-493` creates unit 0 unconditionally
(`su_StartBlock = 0`, `su_BlockCount` from the CSD) *before* reading the MBR.
Each `0x76` primary then takes the next number in **MBR slot order**
(`init.c:528-531`); the eMMC GPT loop does the same in entry-array index order
(`init.c:397-402`). `sd.unit0=off` only skips task creation
(`init.c:542-543`) and does **not** renumber, so numbering is stable. Non-typed
partitions consume no unit number.

Documented policy (<https://github.com/michalsc/Emu68/blob/master/docs/Options.md>:38-51):
`sd.unit0=off | ro | rw`, **default `ro`**, *"Use with care, as Unit 0 of the
device represents the entire card, including partition table and FAT32 boot
partition."* And `SD_Preparation.md:121`: *"Please **do not** change drive type
of **address 0**."*

> **Relevant to our unit model.** Emu68 reserves unit 0 for the whole medium and
> numbers partitions from 1. We number `disk × 100 + partition` with partitions
> from 0 and no whole-disk unit — defensible, since the underlying device
> already provides whole-medium access and exposing it again would be the
> "double presentation" risk the proposal names. But combined with the
> `devtest -p` probe-space problem noted above, the ×100 scheme now has two
> marks against it. Worth one explicit decision rather than two drive-by ones.

### Bounds enforcement: `IOERR_BADADDRESS`, and we should match

Both devices apply the same two-part test before every transfer
(`brcm-sdhc.device/src/io.c`, identically in eMMC):

```c
if (offset >= unit->su_BlockCount ||
    (offset + iostd->io_Length / sd_BlockSize) > unit->su_BlockCount) {
    io->io_Error = IOERR_BADADDRESS;
    iostd->io_Actual = 0;
}
```

Sites: `CMD_READ` `io.c:524`, `TD_READ64`/`NSCMD_TD_READ64` `:548`, `CMD_WRITE`
`:575`, `TD_WRITE64`/`TD_FORMAT64`/`NSCMD_*` `:607`, and **inside `HD_SCSICMD`
for READ(6)/(10)/WRITE(6)/(10)** at `:198, 228, 262, 298`. Only after passing
does it add `su_StartBlock`. So a `0x76` unit genuinely cannot touch unit 0's
FAT32 boot partition — independent confirmation of the proposal's defining
safety property, including the no-SCSI-back-door rule.

Other codes: `IOERR_BADLENGTH` when `io_Length` is not a whole multiple of the
block size; `TDERR_WriteProt` for writes to a read-only unit (`io.c:569, 599`);
`TDERR_NotSpecified` on underlying transfer failure; `IOERR_NOCMD` for
unsupported commands. SCSI bounds failures also set `scsi_Status = 0x02` CHECK
CONDITION (`io.c:414-420`).

> **Adopt `IOERR_BADADDRESS`** as our out-of-range code — it matches Emu68 and
> satisfies devtest's "any nonzero error" requirement.

Two Emu68 quirks to avoid: the 64-bit paths compute `offset = off64 >> 9`,
hardcoding 512 while `CMD_READ` divides by `sd_BlockSize`; and `endblock` is
computed at `io.c:433` and never used.

### Emu68's limits — three of them are bugs worth not inheriting

- **No EBR chain walking in either device.** Both read LBA 0 once and iterate
  `i < 4` over primaries only; `0x05`/`0x0F`/`0x85` are treated as any other
  non-`0x76` type and ignored. Hard-capped by `sd_Units[5]` / `emmc_Units[5]`,
  sized for *"5 units at most for the case where SDCard has 4 primary
  partitions type 0x76"* (`src/sdcard.h:52`).
  > So **our EBR/logical support is an extension of the convention, not an
  > implementation of it.** Label it as such.
- **`emmc_Units[5]` overflows on a fifth Amiga-type GPT partition.** The GPT
  loop appends with no bounds check on `emmc_UnitCount`, writing past
  `emmc_Units[4]` into `emmc_UnitCount` and the following `SignalSemaphore`.
  The 1.1 beta.1 release note claims *"No more 4-partition limit like it is with
  MBR scheme"* — the array does not back that claim. **The single most important
  thing to know when mirroring the convention**, and worth reporting upstream.
- **32-bit LBA ceiling (2 TiB).** Entries whose start or end has a non-zero high
  longword are skipped with *"Partition%ld is too big for 32-bit sector numbers,
  skipping"*, despite GPT being 64-bit. Our 64-bit arithmetic is a genuine
  improvement.
- **Infinite loop on a corrupt RDB.** In `MountPartitions()` the
  `next_part = buff.part.pb_Next` assignment sits *inside* the checksum-valid
  branch (`unit_task.c:633`), so a `PART` block with a bad checksum leaves
  `next_part` unchanged and the `while` spins forever.
- `brcm-sdhc.device` **does not check the `0x55AA` signature at all** before
  walking the MBR (that check exists only in the eMMC GPT path). We should.
- Inclusive-end convention: `su_BlockCount = LastSector - FirstSector + 1`.

### Protective-MBR gating, and a hybrid-MBR bug

`check_gpt_support()` requires slot 0 type `0xEE`, `first_sector == 1`,
`sector_count == blockCount - 1`, and slots 1-3 type 0, returning
`has_gpt && protective_mbr`. `init.c:419-424` uses that to choose between GPT
and the `0x76` scan — **the two are mutually exclusive, GPT first.**

But a hybrid MBR (valid GPT, failed protective test) makes the function return 0
*after it has already appended the GPT units*, so init then also runs the `0x76`
scan and **one card yields both sets of units**. No explicit handling.

> Our parser must decide hybrid policy explicitly. The proposal mentions
> "protective and hybrid MBR handling" but not the rule. Suggest: if a valid GPT
> is present, use it and ignore the MBR entirely (the UEFI rule), and log that
> the MBR was ignored.

### Normative prose on `0x76`

<https://github.com/michalsc/Emu68/blob/master/docs/tutorials/SD_Preparation.md>:18:

> *"Instead of using disk as a whole, you can define primary partitions of type
> `0x76` which will appear as separate hard drive units on your m68k operating
> system. Do anything with them and your boot partition will stay safe."*

:48: *"It will be changed to a type `0x76` which is an information for Emu68
(and WinUAE or Amithlon) that this is a virtual hard drive."* — note Emu68's own
docs credit **Amithlon**, confirming the WinUAE finding. :20: *"GPT is not yet
supported by the SDHC driver of Emu68."*

GPT, Emu68 1.1 beta.1 release notes
(<https://github.com/michalsc/Emu68/releases/tag/v1.1.0-beta.1>):

> *"You can create GPT partitioned microSD cards containing the partitions of
> type `3F82EEBC-87C9-4097-8165-89D6540557C0` - each of them will be visible as
> separate device unit on AmigaOS. No more 4-partition limit like it is with MBR
> scheme."*

Not documented anywhere: that the drivers mount RDB partitions and add boot
nodes (beyond the Emu68-tools-old README sentence), the synthesised 128×64
geometry, the inclusive `LastSector` convention, or the 32-bit LBA ceiling.

---

## AROS `partition.library` — the GPT validation checklist, and its gaps

**Correction:** it is at `rom/partition/`, not `workbench/libs/partition`
(`workbench/c/Partition` is the CLI tool). Files: `partitiongpt.c`,
`partitionmbr.c`, `partitionebr.c`, `partitionrdb.c`, `crc32.c`.

### Copy these

- **CRC32 over the header's own `HeaderSize`, with the CRC field zeroed first**
  (`partitiongpt.c:270-278`) — not a fixed 92. `crc32.c:50-63` is standard
  CRC-32/ISO-HDLC (reflected, `0xEDB88320`, init `0xFFFFFFFF`, final
  complement), which is what GPT requires.
- **Honour `EntrySize` and `NumEntries` from the header**
  (`partitiongpt.c:342-344`); stride by `entrysize`, not
  `sizeof(struct GPTPartition)` (`:420-421`), and keep a full-length copy of
  each entry plus its `entrySize` so larger future entries survive
  read/modify/write (`:396, 405, 409`, explained at `:551-555`). Nothing
  hardcodes 128/128.
- **Verify the entry-array CRC over `entrysize * cnt`**, not the block-rounded
  size (`:359-364`).
- **Detect unused entries by an all-zero *type* GUID** (`:387-388`) — correct
  per spec; `PartitionID` is not the right field. Tolerate gaps, with the
  reasoning in the comment at `:379-386`: *"NumEntries in the header holds total
  number of preallocated entries, not the number of used ones… Just in case, we
  allow gaps between used entries. However (tested with MacOS X Disk Utility)
  partition editors seem to squeeze the table."*
- **Probe GPT before MBR** (`partition_support.c:16`), and keep RDB last —
  `partition_support.c:23-26`: *"Keep RDB last. RDB scans up to 16 blocks and in
  case of MBR->EBR->RDB having RDB first detects RDB as root and causes MBR and
  EBR offsets not be taken into account."*
- **Reject FAT superfloppies before treating block 0 as an MBR** — the
  `!FAT_IsBPBPlausible(...)` guard (`partitionmbr.c:29-30`) exists because a FAT
  BPB can end in `0xAA55`.
- **Flat LBA when a partition is not cylinder-aligned**
  (`partition_support.c:137-153`): *"We could find the highest common factor…
  but currently we simply use one block per cylinder (flat LBA)."* Independent
  arrival at our synthetic geometry.

### Do not copy these

- **No upper bound on `HeaderSize`.** `GPT_MAX_HEADER_SIZE 512` is defined
  (`partitiongpt.h:35`) and **never referenced**; validation is only
  `HeaderSize >= 92` (`:263-264`). A header claiming a huge `HeaderSize` makes
  the CRC loop read far past the one-block buffer — a real OOB read. **Clamp to
  the block size.** AROS also zeroes the CRC field destructively in the caller's
  buffer and never restores it.
- **The backup-header fallback is dead code in the probe path.**
  `ERROR_BAD_CRC` is `#define`d 255 (`partitiongpt.c:42`) but
  `GPTCheckHeader()` returns **2** for bad-CRC (`:278`), so the
  `if (res == ERROR_BAD_CRC)` at `:310-311` and `:319` is never true and the
  backup read at `:314-316` never runs. Worse, 2 is truthy to
  `openpartitiontable.c:56`, so a CRC-broken GPT still claims the disk and never
  degrades to MBR. Recovery only works in the open path, where the mapping is
  correct (`:336-337`, `:459-472`).
- **The backup is consulted only on CRC failure**, never when the primary is
  unreadable or lacks the signature (`res == 0` → no backup at all). And the
  backup LBA is taken from the possibly-corrupt primary with no range check.
  > Our rule: try the backup on *any* primary failure, and bound the backup LBA
  > against `dg_TotalSectors`.
- **Primary wins unconditionally when both headers are valid but disagree** — no
  MyLBA/AlternateLBA reciprocity check, no repair. On write AROS writes backup
  first, primary second by design (`:572-583`), so a torn write leaves a new
  backup and an old primary that the read path silently prefers.
- **No sanity checks on the entry array**: no `entrysize >= 128`, no
  multiple-of-8, no `cnt` cap, **no overflow check on `entrysize * cnt`** (plain
  ULONG multiplies at `:344, :360` — an overflowed product yields a small
  allocation while the CRC and entry loops run the full extent). No check that
  the entry-array LBA is within the disk.
- **No per-entry sanity at all** beyond the unused check: no
  `EndBlock >= StartBlock`, no containment in `DataStart..DataEnd` or
  `dg_TotalSectors`, no overlap detection, no `StartBlock != 0`.
  `initPartitionHandle(root, …, startblk, endblk - startblk + 1)` runs
  unconditionally (`:399`), so `endblk < startblk` wraps `count_sector` and
  produces garbage `de_HighCyl`.
- **32-bit truncation.** Start/end are computed as `UQUAD` but
  `initPartitionHandle()` takes `ULONG first_sector, ULONG count_sector`
  (`partition_support.c:126`), so **every GPT partition's start and length is
  silently truncated to 32 bits** in the resulting `DosEnvec`/`DriveGeometry`,
  even though `PT_STARTBLOCK`/`PT_ENDBLOCK` report full 64-bit values (`:617-623`).
- **The EBR walk has no loop protection whatsoever.**
  `PartitionEBROpenPartitionTable()` (`partitionebr.c:98-155`): the induction
  variable `i` is a `UBYTE`, is incremented, and is **never read** — no
  iteration cap, no monotonically-increasing-LBA check, no visited set, no bound
  against the extended partition's extent. A cyclic chain allocates a
  `PartitionHandle` + `EBRData` per iteration and `Enqueue`s it, terminating only
  by exhausting memory. Reachable from early boot partition scanning.
  > **The proposal's EBR walking must have an iteration cap, a
  > strictly-increasing-LBA check, and a visited set.** This is the one AROS
  > defect most likely to bite us, because we are walking the same chain.
  >
  > Also note AROS's spec deviation: `block_no` from the link entry is used as
  > an absolute LBA (`:117`) while the logical start is
  > `block_no + pcpt[0].first_sector` (`:127`) — the link LBA should be relative
  > to the extended partition's start, which coincides only on the first link.
- **Protective-MBR detection is stricter than UEFI**: the `0xEE` entry must be
  in **slot 0** and start at exactly LBA 1 (`partitiongpt.c:305-306`). A
  protective MBR with `0xEE` in slots 1-3, which some tools emit, fails GPT
  detection and is parsed as a plain MBR. `partitionmbr.c` itself never looks at
  partition types at all.
- **AROS's own type GUID is non-conformant** —
  `partitiongpt.c:56-62`: *"The first four bytes (time_low) hold DOS Type ID
  (for simple mapping), so we set them to zero here. We ignore it during
  comparison."* Interesting idea (it is how AROS carries a DosType in GPT, the
  problem our proposal solves with a `<DosType>:<DOSName>` partition name) but
  it means AROS matches a *family* of GUIDs, not one. Not something to imitate.
- **Hybrid MBR is neither detected nor reconciled**, and the write-side gap is
  an acknowledged TODO (`partitiongpt.c:520-528`) about IntelMac shadow entries.
- `MBR_STATUS_VALID()` is the lenient `((!(status & 0x0F)) || (status & 0x80))`
  (`partitionmbr.h:34`) rather than just `0x00`/`0x80`.

### Block size

Discovered via `TD_GETGEOMETRY` (`partition_support.c:29-41`,
`openrootpartition.c:70-78`), stored as `de_SizeBlock` **in longwords**
(`dg_SectorSize >> 2`), recovered as `de_SizeBlock << 2`. No 512 default and no
fallback — a failed `TD_GETGEOMETRY` aborts. CD-ROM devices are rejected
outright (`openrootpartition.c:72`).

4Kn (`de_SizeBlock == 1024`) is structurally handled throughout GPT/MBR/EBR.
Two real ceilings: `partitionebr.c:33-42` refuses to probe if the sector size
exceeds its 4096-byte stack buffer (same for RDB at `partitionrdb.c:189, 607, 955`),
so **8Kn media silently fails detection**; and `partition_support.c:59` computes
`((de_SizeBlock << 2) / 512)` for the 4 GB 64-bit-command threshold by integer
division, so **any sector size below 512 collapses the product to 0 and
NSD/`TD_READ64` is never negotiated.**

### Not verifiable

`FAT_IsBPBPlausible()` / `FAT_ParseBPB()` live outside `rom/partition`
(`<linklibs/fatbpb.h>`), so the MS-Basic-Data→DOSType probe at
`partitiongpt.c:243-244` is unaudited. `AROS_ROUNDUP2()` is defined elsewhere.

---

## The decision: `ptable.library` / `partition.resource`

**This is the Phase 0 finding that matters most, and it is not a naming issue.**

Jaroslav Pulchart's **`ptable.library`** parses RDB/MBR/GPT/flat layouts and
*"publishes every partition into a shared `partition.resource`"*. Version 2.2,
uploaded to Aminet **2026-10-07** — the day before this proposal was written.

- <https://aminet.net/package/util/libs/ptable.v20261007>
- <https://github.com/pulchart/amigaos-ptable>
- Consumers already shipping, same date: `compactflash.device` 2.0+
  (<https://github.com/pulchart/cfd>) and `fat95` 4.0+
  (<https://github.com/pulchart/fat95>).

Aminet readme search for `partition.resource` returns exactly those three
packages.

So the `partition.*` namespace in the *resource* slot is taken, by an actively
developed project solving substantially our problem with a different
architecture: a **shared parsing library plus a published resource that other
devices consume**, versus our **filter device that presents units**.

> **Needs a decision before Phase 1.** The options, as I see them:
>
> 1. **Build on it.** Consume `partition.resource` for the parsing layer instead
>    of writing our own MBR/EBR/GPT parser, and contribute the unit-presentation
>    layer that it lacks. This deletes most of Phase 1, inherits a parser two
>    shipping devices already exercise, and avoids a second incompatible
>    partition stack. Costs: a hard dependency on a three-week-old library, and
>    the proposal's "host-testable C parser module" test strategy would need
>    rethinking.
> 2. **Build alongside it**, interoperating where sensible (e.g. register our
>    units in `partition.resource` so `fat95` and `cfd` can see them). Keeps our
>    parser and our test strategy; risks ecosystem fragmentation.
> 3. **Build independently** and document the relationship honestly.
>
> Either way: **talk to the author before naming or releasing.** The proposal's
> Risks section anticipated a name collision; the real risk turned out to be a
> capability collision.

### Architecturally nearest existing work: `kcshdproxy`

<https://github.com/svanderburg/kcshdproxy> (Sander van der Burg) installs
`kcshdproxy.device`, an Exec device that *"intercepts relevant SCSI and
trackdisk I/O requests"* and *"translate[s] offset values in such a way that the
beginning of the emulated PC hard drive starts at 0."*

**This is the nearest thing in existence to our I/O path** — a proxy block
device doing offset translation over another device. The proposal's prior-art
section does not mention it. Read it before writing Phase 1's BeginIO.

### Other prior art the proposal missed

- **GiggleDisk** (Geit / Guido Mersmann), V1.19, 68k + MorphOS —
  <https://www.geit.de/eng_giggledisk.html>, Aminet `disk/misc/giggledisk`:
  *"Analysing the entire hard drive by using the RDB and MBR and it auto creates
  mount and dos driver files. It even automatically mounts drives of a specific
  type."* Handles MBR (PC/Linux/Pegasos), RDB, **VHD type `0x76`
  (Amithlon/UAE)**, and unpartitioned media. The closest functional prior art
  outside Emu68, and it predates everything in the proposal's list.
- **MountDos 1.2** (Harry "Piru" Sintonen, 2000), Aminet `disk/misc/MountDos12`
  — reads MS-DOS partition tables from IDE/SCSI, generates mountlists,
  auto-mounts via CrossDOS or fat95; FAT32 via fat95, **extended and hidden
  partition support**, AmigaE source included. Mountlist entries, not units.
- `disk/misc/mount_msdos`, `disk/misc/fdisk`, `disk/misc/partcopy`,
  `disk/salv/RDBrecov`, `disk/misc/rdp391`.
- **`filedisk.device`** in ImageMount 1.4 (Roger Håseth, 2019), Aminet
  `disk/misc/ImageMount`.
- **hst-imager** (Henrik Stengaard) — host-side, open issue #85 for PiStorm GPT
  support. Adjacent, not a clash.
- **MorphOS `RAWDISK:`** — a handler exposing raw drives plus a partition-table
  directory of per-partition files. Same concept, different mechanism.

### The cautionary precedent for a generic device name

**`diskimage.device` exists twice**, from two authors. Fredrik Wikström's
readme states it is *"not to be confused with Thore Boeckelmann's
diskimage.device, since it is a from scratch development and not based on it in
any way, though the same mountlists can be used for both devices."* Note that
is the same Thore Böckelmann the proposal cites for AutoMounter — so the
ecosystem has already had this exact problem, with an adjacent author.

---

## Aminet name check — `partition.device` is literally free

Verified with working controls, which is what makes the zero meaningful. Aminet
advanced search exposes separate `name`/`desc`/`readme`/`content` indexes; all
four return **0** for `partition.device`
(<https://aminet.net/search?type=advanced&readme=partition.device>).

Controls proving the index works: `readme=scsi.device` → **126** packages;
`desc=scsi.device` → 12; `readme=disk.device` → **111**, which also shows the
index is **substring**-matching (catching `trackdisk.device`, `ramdisk.device`,
`harddisk.device`) — so zero hits also rules out any `*partition.device` suffix
form. 85,663 packages indexed.

Near-miss sweep, all 0: `partitions.device`, `part.device`, `rdb.device`,
`mbr.device`, `gpt.device`, `volume.device`, `slice.device`, `block.device`,
`media.device`, `mounter.device`, `carve.device`.

Elsewhere: **AROS** has `partition.library` and no `partition.device` —
confirmed from `rom/partition/partition.conf` (`basename Partition`,
`libbase PartitionBase`, `version 3.3`), repo-scoped code search 0 hits.
**WinUAE** uses `uaehf`/`uaescsi`/`uaenet`/`uaeserial.device`. **Emu68** uses
`brcm-sdhc`/`brcm-emmc.device`. **PiStorm, lide.device, a4091-software**: 0 hits
each. **GitHub globally**: ~40 hits, all coincidental (`psutil` attribute
access, Go struct fields, Haiku/BonsOS C). **os4depot** `driver/storage` has no
`partition.device`.

> **Recommendation: rename anyway.** Not because the name is taken, but because
> `partition.library` (AROS) and `partition.resource` (Pulchart, active)
> already exist, and `partition.device` would read as the device member of a
> family it has no relationship to, by a third author. Candidates verified free
> on all three Aminet indexes:
>
> 1. **`partmap.device`** — names the mechanism (the partition map) rather than
>    the generic noun; stays truthful if we later add APM or BSD disklabel.
> 2. **`partunit.device`** — most self-documenting: partitions become units.
>    Clunky, zero ambiguity.
> 3. **`vpart.device`** — `v` for virtual, matching that our units are synthetic
>    views; reads well beside `uaehf.device` and `brcm-sdhc.device`.
> 4. **`slice.device`** — short, and "slice" is the BSD term for exactly this;
>    but Amiga ears may hear copper/display slice first.
> 5. **`pmount.device`** — only if the mounting behaviour is the headline rather
>    than the offset translation.
>
> Also free: `ptmount`, `pslice`, `xpart`, `parts`, `carve`, `mbr`, `gpt`, `rdb`.

### Could not verify

- **EAB (eab.abime.net) is behind Anubis anti-bot** — every fetch returned the
  challenge page, including the ImageMount thread (`t=94982`). Only
  search-engine snippets, not treated as evidence. Needs a browser; worth doing
  before release.
- **os4depot's search endpoint** returned nothing usable; `driver/storage` was
  browsed directly, but the full catalogue and readme texts were not searched.
- **MorphOS has no reachable authoritative device-name inventory.** `RAWDISK:`
  and HDConfig confirmed to exist; cannot assert MorphOS has no
  `partition.device`.
- **OS 3.5/3.9 BoingBag device inventories** — no authoritative list found. Low
  risk (a Commodore/Haage&Partner `partition.device` would be well documented)
  but unverified.
- **Aminet's `content` index may not decompress every archive**, so a device
  name inside a binary could in principle hide. The readme index is the stronger
  signal and is clean.

---

## lide.device — device surface

Repo `/Users/simond/src/lide.device` @ `b6b4c13`.

### ✅ The `ETD_*` + change-number requirement, answered exactly

lide implements `ETD_READ`, `ETD_WRITE`, `ETD_FORMAT` and
`NSCMD_ETD_READ64`/`WRITE64`/`FORMAT64`, and does compare `iotd_Count`
(`iotask.c:463-479`):

```c
case ETD_READ:
case NSCMD_ETD_READ64:
    direction = READ;
    goto validate_etd;
case ETD_WRITE:
case ETD_FORMAT:
case NSCMD_ETD_WRITE64:
case NSCMD_ETD_FORMAT64:
    direction = WRITE;
validate_etd:
    if (iotd->iotd_Count < unit->changeCount) {
        error = TDERR_DiskChanged;
        break;
    } else {
        goto transfer;
    }
```

`iotd = (struct IOExtTD *)ioreq` (`iotask.c:406`), valid because `IOExtTD`
begins with `IOStdReq`. Five details that must be matched:

- **The comparison is `<`, not `!=`.** A count *ahead* of the device's counter
  is accepted; only a stale one is rejected.
- **`changeCount` starts at 1, not 0** (`iotask.c:251`), so `iotd_Count == 0` is
  *always* rejected, on every unit including fixed disks. This is exactly
  devtest's probe. **If our change count starts at 0, devtest's strictest test
  passes where lide's fails — i.e. we would be wrong.** Initialise to 1.
- The check runs in the task, not `begin_io`, so the rejection arrives via
  `ReplyMsg` with `io_Error = TDERR_DiskChanged`, not as quick-IO.
- It **precedes** the media-presence and range checks, so a stale count reports
  `TDERR_DiskChanged` even when the offset is also out of range.
- `changeCount` only ever advances for removable media
  (`atapi_update_presence`, `atapi.c:1150-1162`); fixed units sit at 1 forever.

`ETD_SEEK`/`NSCMD_ETD_SEEK64` are **not** implemented — lide implements no seek
command at all (`TD_SEEK`, `ETD_SEEK`, `TD_SEEK64`, `NSCMD_TD_SEEK64`,
`NSCMD_ETD_SEEK64` all return `IOERR_NOCMD`), nor `CMD_RESET`, `CMD_FLUSH`,
`TD_RAWREAD`, `TD_RAWWRITE`, `TD_GETNUMTRACKS`.

> Note devtest's seek tests therefore print `(unsupported)` against lide too —
> more evidence that "devtest green" means a reviewed report, not a pass.

### ⚠ The LUN rule is non-negotiable, and it constrains our unit numbering

`device.c:542-554` decodes `lun = unitnum / 10; unitnum %= 10`, and:

> ```
> /* IMPORTANT: Must return TDERR_BadUnitNum when lun > 0
>  * SCSI Unit encoding places the LUN in the 10s column of the unit number
>  * HDToolbox scans each LUN of a unit and stops searching if it sees an error
>  * other than TDERR_BadUnitNum
>  * So if this is not returned, only one drive will ever be detected
>  */
> ```

Anything probing by walking unit numbers — HDToolbox, and lide's own bundled
mounter sweeping `target + lun*10` for targets 0-7 (`mounter.c:1560-1574`) —
depends on `TDERR_BadUnitNum` being distinguishable from a hard failure.

> **Direct consequence for our ×100 scheme (which is now a settled decision).**
> `open()` must return `TDERR_BadUnitNum` — never `IOERR_OPENFAIL` — for every
> unit number that does not exist, because the gaps in our numbering are
> *dense* in probe terms: a scanner walking 0,1,2,…,99 before reaching 100 must
> see `TDERR_BadUnitNum` for all 97 absent ones or it stops early. Note lide
> itself is asymmetric here (`unitnum > highestUnit` gives `IOERR_OPENFAIL`,
> `device.c:556-559`); **we must not copy that asymmetry.** This is the concrete
> cost of ×100, and it is one line of code to pay.

### The advertised command set, and three traps in it

`supported_commands[]` (`device.c:683-716`) is one global list shared by all
units: `CMD_CLEAR`, `CMD_UPDATE`, `CMD_READ`, `CMD_WRITE`, `CMD_START`,
`CMD_STOP`, `TD_ADDCHANGEINT`, `TD_REMCHANGEINT`, `TD_REMOVE`, `TD_CHANGENUM`,
`TD_CHANGESTATE`, `TD_EJECT`, `TD_GETDRIVETYPE`, `TD_GETGEOMETRY`, `TD_MOTOR`,
`TD_PROTSTATUS`, `TD_READ64`, `TD_WRITE64`, `TD_FORMAT64`, `ETD_READ`,
`ETD_WRITE`, `ETD_FORMAT`, `NSCMD_ETD_READ64`/`WRITE64`/`FORMAT64`,
`NSCMD_DEVICEQUERY`, `NSCMD_TD_READ64`/`WRITE64`/`FORMAT64`, `HD_SCSICMD`.

Do not inherit these three inconsistencies:

1. **`TD_FORMAT` is handled but not advertised** (`device.c:844`,
   `iotask.c:488`).
2. **`ETD_FORMAT` is advertised but unreachable** — in the list (`device.c:706`)
   and handled (`iotask.c:469`), but **absent from `begin_io`'s case list**
   (`device.c:836-856`), so it falls to `default:` and returns `IOERR_NOCMD`.
3. **One list for all units**, so ATAPI CD units advertise `TD_READ64` etc.
   identically to hard disks even though `atapi_translate` takes only a 32-bit
   LBA. **A filter fronting heterogeneous children should build the list
   per-unit** from what each child actually advertised.

### `NSCMD_DEVICEQUERY` — and a size discrepancy worth knowing

`device.c:865-886`: rejects NULL or odd `io_Data` with `IOERR_BADADDRESS`,
rejects `io_Length < sizeof(struct NSDeviceQueryResult)` with `IOERR_BADLENGTH`,
then fills `DevQueryFormat = 0`, `SizeAvailable = sizeof(struct
NSDeviceQueryResult)`, `DeviceType = NSDEVTYPE_TRACKDISK`, `DeviceSubType = 0`,
`SupportedCommands = (UWORD *)supported_commands`, and sets `io_Actual` to the
same size.

`struct NSDeviceQueryResult` in lide's `newstyle.h:20-35` is **16 bytes**, with
the header's own warning *"May be extended in the future! Check
SizeAvailable!"*.

> **Cross-check against devtest:** devtest's *local* copy of the struct is 36
> bytes (`devtest.c:248-261`) and it sets `io_Length = sizeof(...)` = 36. lide's
> `>=` test passes, lide writes 16 bytes and reports `SizeAvailable = 16`. So
> **we must accept an `io_Length` larger than our own struct and report the
> bytes actually written** — not `io_Length`, and not a hardcoded 36. Getting
> this backwards is invisible to devtest (it never reads `SizeAvailable`) but
> would mislead real callers.

`SupportedCommands` points at our own static const array, never copied into the
caller's buffer.

### `TD_GETGEOMETRY` — three things to copy

`td_get_geometry` (`device.c:623-654`): odd/NULL `io_Data` → `IOERR_BADADDRESS`;
`io_Length < sizeof(struct DriveGeometry)` → `IOERR_BADLENGTH`; **`memset` the
whole struct to zero first** so reserved fields are clean; and the
**`ULONG_MAX` clamp** —

```c
if (unit->logicalSectors > ULONG_MAX) geometry->dg_TotalSectors = ULONG_MAX;
else                                  geometry->dg_TotalSectors = unit->logicalSectors;
```

`dg_BufMemType = MEMF_PUBLIC` (PIO only, no Chip-RAM constraint),
`dg_DeviceType = unit->deviceType`, `dg_Flags = atapi ? DGF_REMOVABLE : 0`,
`io_Actual = sizeof(struct DriveGeometry)`.

> The clamp is directly relevant: our units are 64-bit addressable but
> `dg_TotalSectors` is a ULONG, so a >2 TB partition must clamp rather than
> wrap. The proposal mentions the 32-bit `DosEnvec` cylinder fields but not this.

Note also lide **synthesises** geometry for large drives rather than trusting
IDENTIFY — `>= 267382800` sectors → `heads = 64, spt = 256, cylinders = sectors
>> 14` (*"For drives larger than 127GB fudge the geometry"*, `ata.c:361-365`);
`>= 16514064` → `16/255` (*"a drive larger than 8GB will report 16383/16/63
(CHS) … generate a new Cylinders value"*, `ata.c:366-373`). Synthetic geometry
is normal practice, not a liberty we are taking.

ATAPI units zero `blockSize`/`logicalSectors` on media removal
(`atapi.c:1158-1160`), so `TD_GETGEOMETRY` on an empty drive reports
`dg_SectorSize = 0`. **Our vanished-partition units need a defined answer
here** — the proposal says they report no-disk via `TD_CHANGESTATE` but does not
say what geometry they report.

### The TD64 offset convention, and a load-bearing fallthrough

`io_Offset` is the **low 32 bits of a byte offset**; `io_Actual` is the **high 32
bits** (the NSD spec's `io_HighOffset` — that symbol never appears in lide).
Assembly, `iotask.c:500-510`:

```c
lba_high = ioreq->io_Actual >> blockShift;
lba_low  = ioreq->io_Actual << (32 - blockShift);
lba_low |= (ioreq->io_Offset >> blockShift);
lba = ((uint64_t)lba_high << 32 | lba_low);
count = (ioreq->io_Length >> blockShift);
```

*"This looks like a lond-winded way to get the LBA doesn't it? Splitting up the
operation like this results in smaller code size (avoids 64-bit math from
libgcc)"* (`iotask.c:502-503`).

**The device must zero `io_Actual` for the 32-bit commands, because callers
leave it dirty** (`device.c:836-841`):

```c
case CMD_READ:
case ETD_READ:
case CMD_WRITE:
case ETD_WRITE:
    ioreq->io_Actual = 0; // Clear high offset for 32-bit commands
case TD_CHANGESTATE:
```

That fallthrough is load-bearing, and lide's own mounter is one of the dirty
callers (`mounter.c:285-288`, `1171-1174` set `io_Command`/`io_Offset`/
`io_Data`/`io_Length` for `CMD_READ` and never touch `io_Actual`).
`ETD_FORMAT`/`TD_FORMAT` are *not* in the group — part of the breakage above.

Validation order after assembly (`iotask.c:493-521`): no-media →
`TDERR_DiskChanged` **first** (before arithmetic, since `blockShift` is zero in
that state); `count == 0` → `IOERR_BADLENGTH` (so a sub-block `io_Length`
truncates to 0 and is rejected, not treated as partial); `(lba + count) >
logicalSectors` → `IOERR_BADADDRESS`.

### `HD_SCSICMD` — the accepted set and the epilogue

For plain ATA units (`iotask.c:130-206`), exactly: `0xA1` ATA PASSTHROUGH(12),
`0x00` TEST UNIT READY, `0x12` INQUIRY, `0x1A` MODE SENSE(6), `0x25` READ
CAPACITY(10), `0x9E` READ CAPACITY(16), `0x08`/`0x0A` READ/WRITE(6),
`0x28`/`0x2A` READ/WRITE(10), `0x88`/`0x8A` READ/WRITE(16). **Everything else
→ `IOERR_NOCMD`** (`iotask.c:203-205`). `io_Data == NULL` →
`IOERR_BADADDRESS`; `io_Length` is **not** validated.

> That list is almost exactly the proposal's allowlist, arrived at
> independently. Adopt it, and note lide bounds-checks the rewritten LBA on the
> same path (`data == NULL || (lba + count) > logicalSectors` →
> `IOERR_BADADDRESS`, `iotask.c:176-201`) — the no-back-door rule, implemented.

The epilogue a filter must mirror exactly (`iotask.c:209-225`):

```c
scsi_command->scsi_CmdActual = scsi_command->scsi_CmdLength;
if (error != 0) {
    scsi_command->scsi_Status = SCSI_CHECK_CONDITION;   /* 2 */
    if (scsi_command->scsi_SenseActual == 0)
        fake_scsi_sense(scsi_command,0,0,error);
    return HFERR_BadStatus;
} else {
    scsi_command->scsi_Status = 0;
    return 0;
}
```

So `scsi_CmdActual` is always set; on failure `io_Error` becomes
**`HFERR_BadStatus` (−42), not the underlying error** — the real cause travels
only in the sense data; and sense is filled only if a lower layer hasn't.

`fake_scsi_sense` (`scsi.c:31-86`) bails to `scsi_SenseActual = 0` unless
`SCSIF_AUTOSENSE` is set, `error != 0`, `sense != NULL` and `scsi_SenseLength >=
sizeof(struct SCSI_FIXED_SENSE)`. Otherwise it writes a 0x70 fixed descriptor
and **stashes the Amiga error code in the FRU byte** — a nice debugging
convention worth copying. Mapping: `IOERR_BADADDRESS` → key 0x05 asc 0x21;
`IOERR_BADLENGTH` → 0x05/0x1A; `IOERR_NOCMD` and `HFERR_BadStatus` → 0x05/0x20;
`IOERR_UNITBUSY` → 0x03/0x04; `TDERR_NotSpecified`/`HFERR_SelTimeout` →
0x0B/0x08.

One bug not to copy: `sense->additional = (UBYTE)sizeof(sense) - 7`
(`scsi.c:45`) takes `sizeof` of a *pointer* → `4 - 7` → 0xFD. Should be
`sizeof(struct SCSI_FIXED_SENSE) - 7`.

`SCSICmd`s are **pre-allocated two per unit and reused** via in-use flags
(`scsi.c:161-213`) — *"allocated once per unit and reused … to avoid repeated
alloc/free cycles that lead to memory fragmentation"*, with a second one so
*"REQUEST SENSE / autosense can be issued while a command obtained from
`scsi_get_unit_cmd()` is still live, without the two aliasing each other"*. Our
filter issues child `HD_SCSICMD`s per request; adopt the same pooling.

### Change notification

- `TD_CHANGENUM` immediate, `io_Actual = changeCount` (`device.c:766-769`).
- `TD_CHANGESTATE` queued; `io_Actual` 0 = present, 1 = absent, and **`io_Error`
  is always 0** — state is never an error (`iotask.c:442-449`).
- `TD_PROTSTATUS` queued; `TDERR_WriteProt` from the lower layer is
  **deliberately swallowed** and converted to `io_Error = 0, io_Actual = 1`
  (`iotask.c:451-461`). Write protection is a status, not an error.
- `TD_ADDCHANGEINT` immediate, and **sets `IOF_QUICK` so the common tail never
  replies** (`device.c:789`, *"Must not Reply to this request"*), then
  `Disable()`/`AddHead`/`Enable()`. The request stays outstanding until removed.
- `TD_REMCHANGEINT` immediate, `Disable()` not `Forbid()` — *"Must Disable()
  rather than Forbid()!"* (`device.c:801`) because the list is walked from
  `Cause()`-driven interrupt context. Returns success whether or not found, and
  does **not** reply the removed request.
- `TD_REMOVE` is a **single slot** (`unit->changeInt = io_Data`,
  `device.c:780-783`); a second `TD_REMOVE` silently overwrites the first.
- **Polling**, which is our `POLL` fallback precedent: one timer per channel on
  `UNIT_VBLANK`, armed at `CHANGEINT_INTERVAL` = 2 s (`iotask.c:623-635`,
  `iotask.h:9`), only created if the channel has removable units
  (`iotask.c:273`). `diskchange_check` (`iotask.c:340-385`) walks units under
  the shared semaphore and, on an edge against `mediumPresentPrev`, fires the
  `TD_REMOVE` interrupt first then every queued `TD_ADDCHANGEINT`, **all inside
  one `Forbid()`/`Permit()`**.

### Skeleton shape, briefly

`device.c` (RomTag, `init`, `init_device`, open/close/expunge, `begin_io`,
`abort_io`, geometry, supported-commands), `iotask.c` (the per-channel task),
`ata.c`/`atapi.c` (transports), `scsi.c` (SCSI-Direct emulation),
`lide_alib.c` (private amiga.lib reimplementation so the device links
standalone), `blockcopy.h`, `debug.c`, `newstyle.h`, `td64.h`.

**No unit array** — units live on a `struct MinList units` in `DeviceBase`
(`device.h:110`) guarded by `struct SignalSemaphore ulSem`, with
`struct IDEUnit` beginning with `struct MinNode` so the node pointer *is* the
`io_Unit` pointer. Every traversal uses the KS-1.3-safe idiom
(`device.c:217-221`):

```c
if (SysBase->LibNode.lib_Version >= 36) ObtainSemaphoreShared(&dev->ulSem);
else                                    ObtainSemaphore(&dev->ulSem);
```

because `ObtainSemaphoreShared` does not exist before V36.

**One IO task per ATA channel** (not per unit, not global), priority 11, 8 KB
stack, created by `L_CreateTask` so `tc_UserData` is populated before the task
runs (`lide_alib.c:156`), with a `Wait(SIGF_SINGLE)` startup handshake
(`device.c:436-485`). `begin_io` answers immediate commands in place and
`PutMsg`es everything else to the task's port after clearing `IOF_QUICK`
(`device.c:857-862`).

`ioreq_is_valid` (`device.c:211-239`) gates **every** entry point, checking both
`io_Unit` membership in the list *and* `io_Device == dev`.

**`expunge` never expunges** — *"Don't expunge / If expunged the driver would be
gone until reboot"* (`device.c:510-523`), preceded by the RKRM warning that
Expunge *"may NEVER Wait() or otherwise take long time to complete"*.

`set_dev_name` (`device.c:73-110`) handles name collision by prepending
`2nd.`/`3rd.`/`4th.` — our own precedent for DOS-name uniquing, at the device
level.

### Error-code table and `io_Actual` conventions

Worth lifting wholesale. Key entries beyond those already quoted:
`ioreq_is_valid` failure → `TDERR_NotSpecified` (the initial value, never
explicitly set, `device.c:727`); unknown command → `IOERR_NOCMD` with
`io_Actual = 0` explicitly zeroed in the task path (`iotask.c:565-569`) *because
it may still hold a caller's high offset*; `AbortIO` returns **0 when nothing
was aborted** (`device.c:917`, *"0 indicates that the IO was *NOT* aborted"*)
and cannot abort in-flight requests.

`io_Actual` is **always set on success, including for no-ops** — `TD_MOTOR`,
`CMD_CLEAR`, `CMD_UPDATE` all set it to 0 explicitly (`device.c:762`) rather
than leaving it untouched. `CMD_UPDATE`/`CMD_CLEAR` are pure no-ops in lide
because there is no cache; **a filter that buffers must actually implement them**
and still answer `io_Actual = 0, io_Error = 0`.

### `MaxTransfer` / `Mask` — lide never reads them

A grep finds `de_MaxTransfer`/`de_Mask` only in the mounter's two synthetic
DosEnvecs (`mounter.c:1242-1243`, `1340-1341`, both `0x100000` /
`0x7FFFFFFE`); RDB partitions get theirs copied verbatim from the partition
block and passed to `MakeDosNode` unexamined. lide accepts arbitrary
`io_Length`, chunks internally at `MAX_TRANSFER_SECTORS = 256`
(`ata.h:12-14`), and never masks buffer addresses — it has no DMA.

> **The asymmetry to think about.** The DosEnvec we publish is the only thing
> constraining callers' buffers and lengths, and filesystems trust it
> absolutely. Pass a child's values through unchanged and we inherit its
> constraints (correct, conservative — and what the proposal says). **Widen them
> and we must actually bounce buffers on every path including `HD_SCSICMD`**, or
> we hand a DMA-constrained child an address it cannot reach. Narrow them and we
> may break an existing RDB's recorded values. The proposal's "default to the
> underlying device's known-safe values, overridable per device" is the right
> policy; the override must be documented as dangerous.

### Alignment

No Chip-RAM handling (`dg_BufMemType = MEMF_PUBLIC`, PIO only), but odd-address
handling is pervasive: `ata_read`/`ata_write` switch to slower unaligned
routines on an odd buffer (`ata.c:474-481`, `563-570`); the ATAPI layer bounces
through an aligned allocation —

> `// Some bozo with an unaligned data buffer... (lookin' at you HDToolbox!)`
> `// Allocate an aligned buffer and CopyMem to / from this one`
> — `atapi.c:1032-1033`

— returning `TDERR_NoMem` if the bounce buffer can't be allocated;
`scsi_ata_passthrough` instead **rejects** odd `scsi_Data` with
`IOERR_BADADDRESS` (`ata.c:823`, `830`); and both struct-returning commands
reject odd `io_Data` outright. **Pick one policy and apply it consistently** —
bounce or reject, not both.

### Scar tissue worth quoting

Olaf Barthel's trackfile.device IORequest lifecycle, three rules, each
attributed in the source:

> *"IMPORTANT: Mark IORequest as "complete" or otherwise CheckIO() may consider
> it as "in use" in spite of never having been used at all. This also avoids
> that WaitIO() will hang on an IORequest which has never been used."*
> — `device.c:591-598`, before `ln_Type = NT_REPLYMSG` in `open`

> *"This makes sure that WaitIO() is guaranteed to work and will not hang."*
> — `device.c:732-738`, before `ln_Type = NT_MESSAGE` at the top of `begin_io`

> *"If the IO is still in queue then we can remove it / MUST be done inside a
> Disable()!"* — `device.c:919-924`

The `NT_REPLYMSG` → `NT_MESSAGE` transition is the subtle one: a freshly opened
request must look *complete* so `CheckIO` doesn't report it busy and `WaitIO`
doesn't hang; `BeginIO` must flip it to `NT_MESSAGE` so `WaitIO` on a genuinely
pending request works. **Both halves are required**, and a filter device gets
them wrong by omission very easily. Also: invalidate `io_Device`/`io_Unit` on
open failure (`device.c:612`) and unconditionally in `close`
(`device.c:678-679`).

And the 68000 erratum behind the fast copier (`blockcopy.h:12-16`):

> *"The 68000 does an extra memory access at the end of a movem instruction! …
> With the src of end-52 the error reg will be harmlessly read instead."*

HDToolbox appears three times as the reason for a workaround — the LUN rule, the
READ CAPACITY partial-medium hack (*"Implement this so HDToolbox stops moaning
about track size"*, `scsi.c:281-282`), and the unaligned bounce. Assume it will
be pointed at our units.

---

## lide.device — mounter addendum

### Vendoring and licence

`3rdparty/mounter/update-mounter.sh:10-21` maintains the subtree by **patch
replay** (clone upstream, `git format-patch <pinned> --stdout | git am
--directory "3rdparty/mounter"`), so local edits conflict on every sync — treat
it as read-only.

The BSD-2 block in `mounter.c:10-19` and `ndkcompat.h:2-11` is **truncated**:
both clauses require retaining "the following disclaimer" and no disclaimer text
is present. Same defect in `3rdparty/mounter/README.md:142-152`. Relevant if we
vendor it.

`ndkcompat.h:19-29` exists only to redefine `PRIu32/PRId32/PRIx32` for
`INCLUDE_VERSION < 47`, with the comment *"ULONG has changed from NDK 3.9 to NDK
3.2. However, PRI*32 did not. What is the right way to implement this?"*

### Defects not to inherit

1. **`mounter.c:979`** — `copymem(&pp->de, &part->pb_Environment,
   (pb_Environment[0]+1)*4)` with **no upper bound on `de_TableSize`** → heap
   overflow past `struct ParameterPacket` from a malformed RDB. Clamp it.
   (We only reach this if we ever parse an RDB, which we don't — but the same
   pattern applies to our `DosEnvec` construction.)
2. **Name uniquing never consults the live `DosList`** — only
   `ExpansionBase->MountList` (`mounter.c:841, 862, 1248`), so mounting into a
   running system can collide with an existing volume name.
   > **The one to fix first.** The proposal's in-device mounter runs post-boot on
   > a disk-loaded device, which is exactly the case lide's uniquing gets wrong.
   > Our uniquing must check the live `DosList`, not just the MountList.
3. `mounter.c:727` vs `674` — `FreeMem(fse, sizeof(struct FileSysEntry))`
   under-frees by `strlen(creator)+1`; pool corruption on the
   relocation-failure path.
4. `mounter.c:870` — the `name[len-1] < '9'` bound yields `DH0.9.1` rather than
   failing cleanly at `.9`.
5. `md->wasLastDev`/`wasLastLun`/`ret` are never reset per unit
   (`mounter.c:1000, 1024-1025`); `md` is allocated once for the whole 8-target
   sweep.
6. `de_TableSize = sizeof(struct DosEnvec)` in `register_legacy`
   (`mounter.c:1331`) — bytes where longwords are required. Compiled out for
   lide; do not copy the pattern.

### The mounter is silent in lide builds

`mounter.c:93-95`: `#ifndef A4091` / `#define printf(...)`. Since `A4091` is not
defined in lide's build, **every `dbg()`/`printf()` in `mounter.c` compiles to
nothing regardless of `DEBUG=`**; `DEBUG_MOUNTER` only enables
`USE_SERIAL_OUTPUT`, which `mounter.c` does not consume. Budget for our own
tracing.

### KS 1.3 `FileSystem.resource` fabrication

`FSHDProcess` (`mounter.c:609-709`): if `OpenResource(FSRNAME)` returns NULL it
**creates the resource itself** (`:619-632`) — one `AllocMem` sized
`sizeof(struct FileSysResource) + strlen("FileSystem.resource")+1 +
strlen(creator)+1`, strings laid out after the struct, `W_NewList`, `ln_Type =
NT_RESOURCE`, then `AddTail(&SysBase->ResourceList, …)` by hand rather than
`AddResource`. Version arbitration: an existing entry with `fse_Version >=
version` and `newOnly=TRUE` returns NULL meaning "nothing to load". `FSHDAdd`
uses **`AddHead`, not `Enqueue`** (`:719`), so the newest-loaded filesystem
shadows earlier ones; `fse_Node.ln_Pri`/`ln_Type` are left zero.

### `fsrelocate` (relevant only if we ever load a filesystem ourselves)

`mounter.c:398-606` is a self-contained LoadSeg subset: rejects non-zero
resident-library name size (`:415-419`), rejects overlays (`:420-424`, *"this
function does not support overlay binary files"*), handles
`HUNKF_CHIP|HUNKF_FAST` → explicit memory-flags long and `HUNKF_CHIP` alone →
`MEMF_PUBLIC|MEMF_CHIP` (`:446-479`), builds the BPTR seglist by hand with
`hunkData[0] = size+2`, bounds-checks reloc hunk index and offset (`:535-554`),
and relocates **odd addresses byte-by-byte for 68000/010** (`:556-563`).

**`LSEG_DATASIZE` is hardcoded `512/4 - 5 = 123`** (`mounter.c:97`) — so RDB
filesystem loading is broken on 2048-byte-sector media even though `readblock`
uses `md->blocksize`. A block-size assumption of exactly the kind our parser
must avoid.

`copymem` exists instead of `memcpy` because (`:236-240`) *"compiler built-in
memcpy() can have extra dependencies which will make boot rom build
impossible."*

### Dead API not to replicate

`MountStruct.unitNum` (`mounter.h:9-13`, documented as a count-prefixed ULONG
array) is **never read** anywhere in `mounter.c`. The `MountDrive` return-value
contract at `:1525-1533` (including *"-2 = Skipped, previous unit had RDBFF_LAST
set"*) is likewise stale — a hardcoded 8-target sweep replaced both.
`Makefile:26` defines `-DNO_RDBLAST=1`, which no source file references.

### Scar tissue worth knowing

`bootrom/bootldr.S:76-79`, a vendor-interop hack:

> `; For AT-Bus devices change the manufacturer/prod id to prevent Oktapussy meddling with us`
> `move.w #$0A1C,cd_Rom+er_Manufacturer(a3)   ; Manufacturer: A1K.org`
> `move.b #$7D,cd_Rom+er_Product(a3)          ; Product: Matzes IDE-Controller`

And on diag-time constraints, `bootldr.S:51-57` notes only ~2 KB of stack is
guaranteed at `DAC_CONFIGTIME`, which is why `struct MountData` (~6 KB —
`MAX_BLOCKSIZE*3` buffer, `mounter.c:96, 123`) is `AllocMem`'d rather than
stacked (`:1545`).

> Does not apply to us — we are disk-loaded by deliberate choice, which is one
> more point in favour of the proposal's "no ROM-resident form" constraint.

## Amiberry — no GPT support at all

Checked `/Users/simond/src/amiberry` directly. The result is a plain negative,
and it narrows the claim the proposal is entitled to make.

### The GUID and MBR type 0x76 are both absent

**Amiberry does not recognise `3F82EEBC-87C9-4097-8165-89D6540557C0` in any
spelling.** Greps for `3F82EEBC`, `82EEBC`, `540557C0`, `89D6`, `0xEEBC` and the
mixed-endian initialiser prefix `0xBC,0xEE` return zero hits across source,
headers, docs, scripts, `libretro/`, `tools/` and `external/`. The one textual
near-miss is base64 font data in an SVG badge.

**MBR type `0x76` is not handled either** — there is no `parttype` /
`partition_type` / `pt_type` symbol anywhere in `src/`, so no MBR type byte is
ever dispatched on.

**GPT is entirely absent**: `grep -rIniw "gpt"` returns nothing, as do `EFI
PART`, `EFIPART`, `GUID Partition`, `gpt_header`, `guid_part`. There is no
header CRC32 validation, no backup-header logic, no entry-array CRC — none of it
exists.

> **Changes the proposal.** The prior-art section says of the GUID convention
> that "WinUAE recognises the same GUID", and the Why section implies the
> partition-as-unit model has ecosystem-wide precedent. Amiberry — the most
> widely used emulator on the Pi/Linux side, and the one this session can drive
> directly over MCP — is **not** part of that ecosystem. The claim should be
> narrowed to Emu68 (and WinUAE, pending that strand) rather than stated
> generally, and Amiberry's absence is itself an argument for the project:
> partition.device running *inside* an Amiberry guest would give Amiberry's
> users the capability the emulator lacks.

### How an Amiga partition inside a PC table reaches Amiberry today

By delegating to the host OS, not by parsing. `scan_harddrives_linux()`
enumerates `/sys/block/<disk>/<disk>N` children via `is_partition_of()`
(`src/osdep/amiberry_hardfile.cpp:170-186, 286-312`) and offers each `/dev/sdXN`
as its own selectable "hard drive"; macOS accepts `diskNsM` slices via
`is_disk_name()` (`src/osdep/amiberry_hardfile.cpp:377-398`). The user picks the
slice, and Amiberry treats it as if it were a whole disk, RDB-sniffing from
*its* block 0.

That is the manual, host-mediated version of what partition.device automates —
and it only works because Linux/macOS already parsed the table.

The only MBR awareness in the tree is inverted and unrelated: the ATonce
bridgeboard walks the Amiga RDB to find the partition *containing* a PC MBR and
serves it as the bridgeboard's C: drive (`src/atonce.cpp:519-521, 574-636`),
reading the four primary entries only to recover CHS geometry from the ending-CHS
bytes and testing the type byte solely for `== 0` ("unused",
`src/atonce.cpp:611`). No EBR walk, no LBA use.

### RDB sniff range: Amiberry scans 63 blocks, not 16

`rdb_mount()` (`src/filesys.cpp:8829-8886`) loops `rdblock = 0; rdblock < 63`,
accepting checksum-valid `RDSK` or `CDSK` (`rdb_checksum()`,
`src/filesys.cpp:8287-8307`). It refuses the disk outright if
`63 * blocksize > virtsize` (`src/filesys.cpp:8845-8850`).

> **Open question for the proposal.** Our whole-drive back-off sniffs `RDSK` in
> blocks 1-15, following the documented `RDB_LOCATION_LIMIT` of 16. Amiberry
> (and, per its WinUAE ancestry, probably WinUAE) scans to block 62. A disk with
> an RDB at block 20 would therefore be *mounted by the emulator* but **missed
> by our back-off**, so we would parse its MBR and present units for a disk the
> OS already owns — precisely the clash the back-off exists to prevent. Options:
> scan 0-62 to match the emulators, or scan 0-15 per spec and document the gap.
> Cheap to widen; decide before Phase 1.

Three *other* sniffers in Amiberry use a 16-block window and so disagree with
its own mounter — `hardfile_testrdb()`
(`src/osdep/amiberry_gui.cpp:1344-1377`), `updatehdfinfo()`
(`src/osdep/amiberry_gui.cpp:1430-1444`), `is_rdb_hardfile()`
(`src/osdep/amiberry_rp9.cpp:1160-1178`) — and `zfile.cpp:349` keys image-type
detection on `RDSK` at block 0 only. A disk with an RDB at block 16-62 mounts
but is mis-described by the GUI. Instructive: even the emulators are not
self-consistent about this range.

### MBR at block 0 + RDB at blocks 1-15: the RDB wins, permissively

Confirmed for the exact case the proposal calls "the one real clash". Block 0
holding boot code and `55 AA` matches neither the BABE/MAST test nor
`RDSK`/`CDSK`/`DRKS`, so the loop falls through and continues to block 1; the
RDB is found and `pt_rdsk()` mounts its PART list normally. **There is no
"MBR present, therefore refuse" guard anywhere**, and RDB partition blocks are
bounds-checked only against the whole device (`legalrdbblock()`,
`src/filesys.cpp:8268-8275`) — Amiberry will let RDB partitions point anywhere,
including over PC partitions.

> Confirms the proposal's back-off, by agreement on outcome rather than
> mechanism: Amiberry's answer to "MBR at 0, RDB at 1" is "the RDB owns this
> disk", which is exactly what our backing-off concedes. We must *not* refuse an
> RDB because an MBR precedes it.

### Incidental findings worth keeping

- **Amiberry accepts a PC-partitioned raw disk where WinUAE's logic intended to
  refuse it.** `safetycheck()`
  (`src/osdep/amiberry_hardfile.cpp:711-770`) scans 63 blocks for
  `RDSK`/`DRKS`/ADIDE/PCMCIA-CIS; for a non-empty disk with no RDB (i.e. a GPT
  or MBR disk) WinUAE gates on whether the host has it mounted, but Amiberry's
  `ismounted()` is **stubbed to always return 0**
  (`src/osdep/amiberry_hardfile.cpp:704-708`). Control therefore reaches
  `"hd accepted, not empty and not mounted in Windows"; return -8`, and the
  `harddrive_dangerous == 0x1234dead` escape hatch is dead code. The header
  comment (`:83-86`) still claims it only accepts drives with `RDSK` in block 0
  or a zeroed block 0.
- **ADIDE/byteswap de-mangling only covers the first 16 blocks**
  (`hdf_read_rdb()`, `src/hardfile.cpp:1227-1246`, triggers for
  `offset < 16 * 512`) while the scan reaches block 62 — a latent inconsistency
  in Amiberry, and a reminder that scrambled RDBs exist (`DRKS` byteswapped,
  `CPRM` = `39 10 d3 12` ADIDE). Our sniff should at minimum not mistake these
  for "no RDB"; worth deciding whether to recognise them.
- **Block size never comes from the media.** `hfd->ci.blocksize` comes from
  config, default 512 (`src/hardfile.cpp:285`, `src/cfgfile.cpp:5488`), or is
  auto-detected from `rdb_BlockBytes` for RDB images
  (`src/osdep/amiberry_gui.cpp:1363-1371`). The host's real sector size *is*
  collected into `uae_driveinfo.bytespersector`
  (`src/osdep/amiberry_hardfile.cpp:261-265, 465`) but **never propagated into
  `ci.blocksize`** — so on 4Kn media Amiberry's RDB scan runs at 512-byte
  stride. Our inheriting block size from the underlying unit is the right call
  and is better than the emulator.
- With no RDB and explicit geometry configured, block 0 is read as a bare
  filesystem and its first longword taken as the DOS type (`dofakefilesys()`,
  `src/filesys.cpp:8958-8965`) — so an MBR at block 0 becomes a nonsense
  dostype.

### No interop intent is recorded

`grep -rIniE "emu68|pistorm|brcm.sdhc|brcm.emmc"` and `grep -rni "amiga
partition"` return nothing across source, headers, markdown and scripts.
`docs/` has no mention of GPT, GUIDs, MBR or PC-partitioned media, nor does
`README.md`. There is no `whatsnew` (a WinUAE artefact Amiberry did not inherit).

## devtest (Chris Hooper)

**Source:** <https://github.com/cdhooper/amiga_devtest> — standalone repo, single
`devtest.c` (~6300 lines), HEAD `f5fc034` (2026-09-03). Not vendored in
`kicksmash32`; `lide.device` only mentions it in `README.md:149`.
`a4091-software` ships a binary, not the source.

### "devtest green" is not a well-defined success criterion

`test_packets()` deliberately discards individual test failures when running the
full suite — `devtest.c:4894-4901`, comment: *"Ignore individual test failures
above, since no driver passes all tests."* So `devtest -t` **always exits 0**.
Only `-p`, `-g`, `-i` and `-c <cmd>` can return nonzero (`devtest.c:6256-6273`).

> **Changes the proposal.** The Phase 1 goal "`devtest` green on a Copperline
> image" and the success criterion "`devtest` passes on partition.device units"
> need restating as a *reviewed clean report* plus *`-p`, `-g` and `-i` exiting
> zero*. Otherwise the criterion is trivially met by a device that fails
> everything.

### Synthetic geometry is safe — the Phase 0 question is answered "yes"

`TD_GETGEOMETRY` gets **no consistency checking at all**. Every geometry field
reference is a `printf` or a size computation (`devtest.c:1093-1094, 1336-1344,
1676-1677, 2719-2723`):

- `dg_TotalSectors == dg_Cylinders * dg_Heads * dg_TrackSectors` — never checked.
- `dg_Heads == 1`, `dg_TrackSectors == 1` — no objection; prints as `C=n H=1 S=1`.
- `dg_CylSectors`, `dg_BufMemType` — never referenced. (Still set `dg_CylSectors`
  = 1 correctly, for real filesystems.)
- `dg_DeviceType`, `dg_Flags` — printed only; `DGF_REMOVABLE` is the sole flag
  read. Report not-removable for a partition.
- `dg_SectorSize` — **does** matter: becomes `g_sector_size`, driving SCSI
  READ_6/WRITE_6 block counts (`devtest.c:1908, 1958`), the `do_seek_capacity`
  binary search (`devtest.c:903-945`), and integrity reporting. Must be the real
  block size and nonzero. Inheriting from the underlying unit is correct.

The proposal's 1-surface / 1-block-per-track / cylinders-=-blocks geometry passes
unremarked.

### The strictest requirement in the tool: ETD_* + TD_CHANGENUM

`test_etd_command()` (`devtest.c:2944-2989`) sends every `ETD_*` /
`NSCMD_ETD_*` command with `iotd_Count = 0` and demands it be **rejected with
exactly `TDERR_DiskChanged`**. `rc == 0` is a fail ("command accepted with
invalid iotd_Count"); `rc != TDERR_DiskChanged` is also a fail. It then fetches
the real count via `TD_CHANGENUM` and expects the retry to succeed.

So: `TD_CHANGENUM` must return a **nonzero** change count, `TD_CHANGESTATE` must
report disk-present (`io_Actual == 0`, `devtest.c:2768`), and **every `ETD_*`
command must compare `iotd_Count` against the current change number.**

> **Changes the proposal.** The device-surface section lists `TD_CHANGENUM`/
> `CHANGESTATE`/`ADDCHANGEINT`/`REMCHANGEINT`/`PROTSTATUS` as "forwarded" and
> does not mention the `ETD_*` command family at all. `ETD_READ`, `ETD_WRITE`,
> `ETD_SEEK`, `ETD_FORMAT` and the `NSCMD_ETD_*64` variants must be implemented
> with the `iotd_Count` check. Forwarding alone is not enough: the change number
> that `ETD_*` is checked against is *partition.device's own* per-unit change
> number, which must be derived from (but is not identical to) the underlying
> unit's — a media change invalidates every unit on that disk.

### Commands exercised

Dispatch table `devtest.c:4496-4538`, driver `devtest.c:4633-4800`.

Default `-t` (non-destructive): `TD_GETGEOMETRY`, `TD_CHANGENUM`,
`TD_CHANGESTATE`, `TD_PROTSTATUS`, `TD_GETDRIVETYPE`, `TD_GETNUMTRACKS`,
`TD_RAWREAD`, `CMD_UPDATE`, `CMD_CLEAR`, `HD_SCSICMD` (INQUIRY + TUR),
`NSCMD_DEVICEQUERY`, `CMD_READ`, `ETD_READ`, `TD_READ64`, `NSCMD_TD_READ64`,
`NSCMD_ETD_READ64`, `TD_SEEK`, `ETD_SEEK`, `TD_SEEK64`, `NSCMD_TD_SEEK64`,
`NSCMD_ETD_SEEK64`, `TD_MOTOR`.

`-tt` adds `CMD_START`/`STOP`, `TD_EJECT`/`LOAD`, `TD_ADDCHANGEINT`/
`REMCHANGEINT` (`devtest.c:4848-4853`). `-d` adds the write/format family
(`devtest.c:4854-4863`). `TD_RAWWRITE` is hard-disabled (`if (0 && ...)`,
`devtest.c:4666`).

Only three failures cascade: `CMD_READ` (skips ETD/64-bit reads,
`devtest.c:4685-4689`), `TD_SEEK` (skips the seek family,
`devtest.c:4704-4708`), `NSCMD_DEVICEQUERY` (skips all eight NSCMD tests,
`devtest.c:4672-4678`). Everything else is a pure probe — `IOERR_NOCMD` prints
as `(unsupported)` and is the documented expected result for `TD_GETDRIVETYPE`/
`TD_GETNUMTRACKS`/`TD_RAWREAD` on a hard disk (`README.md:339-341`).

### NSCMD_DEVICEQUERY — less is demanded than expected

`test_nsd_devicequery()` (`devtest.c:3164-3200`) requires only `DoIO() == 0`,
`DevQueryFormat == 0`, and `DeviceType == NSDEVTYPE_TRACKDISK` (5). It **never
dereferences `SizeAvailable`, `DeviceSubType` or `SupportedCommands`** (declared
at `devtest.c:248-261`, never read; the `"Check SizeAvailable!"` comment at
`devtest.c:260` is aspirational). Fill the struct properly anyway for real
filesystems — devtest just won't catch it if we don't.

### TD64 / NSD64 — probed, not required, and skipped under 4 GB

Offset convention confirmed: high 32 bits of the byte offset in `io_Actual`, low
in `io_Offset`, for both the `TD_*64` and `NSCMD_*64` families
(`devtest.c:3135-3141, 3223-3229, 3963-3969, 4025-4031`).

The 4 GB-crossing sub-tests only run when `g_devsize >= (1<<32) + BUFSIZE*2`
(`devtest.c:3976, 4037, 4077, 4163, 4214, 4258`). **A partition under 4 GB skips
every 4 GB-boundary check**, so the Phase 2 fixture set needs at least one
partition starting beyond 4 GB to exercise the translation at all — which is
exactly the case the proposal cares about, since a partition's *absolute* start
can cross 4 GB while the partition itself is small. devtest will never test that
for us; our own fixtures must.

`g_has_nsd` (`devtest.c:3190`) makes devtest prefer `NSCMD_*64` over `TD_*64`
for all its internal >4 GB I/O.

### HD_SCSICMD — one command actually matters

SCSI-direct is not required for a clean `-t` report: `test_hd_scsicmd_tur()`
always returns 0 and even treats sense key NOT_READY as success
(`devtest.c:2896-2923`); `test_cmd_scsi()` always returns 0
(`devtest.c:4411-4490`); READ CAPACITY(16) failing is documented as normal
(`README.md:174-179`).

**But `drive_geometry()` (the `-g` report) ends with
`rc = scsi_read_mode_pages(...)` and returns it unchanged
(`devtest.c:1415, 1492`) — so rejecting MODE SENSE(6) makes `devtest -g` exit
nonzero** even though every printed line is fine. This is the only place a
SCSI-direct refusal produces a failing exit code, and it vindicates the
proposal's decision to forward `MODE SENSE`.

Commands devtest issues: INQUIRY (6-byte, alloc 36, `devtest.c:828-842`), TEST
UNIT READY (`devtest.c:845`), READ CAPACITY(10) (`devtest.c:857`), READ
CAPACITY(16) (`devtest.c:872`), MODE SENSE(6) all-pages with DBD
(`devtest.c:1020-1039`), READ(6)/WRITE(6) at LBA 0 in `-bb`
(`devtest.c:1888-1990`).

`setup_scsidirect_cmd()` (`devtest.c:716-739`) **always ORs in
`SCSIF_AUTOSENSE`** and supplies `scsi_SenseData`/`scsi_SenseLength`;
`scsi_SenseActual` is read back at `devtest.c:811`. Our `HD_SCSICMD` path must
honour autosense and fill `scsi_SenseActual`.

> Confirms the proposal: READ CAPACITY must report the **partition's** size.
> devtest does not offset SCSI LBAs by its own `g_devstart` (`devtest.c:1908,
> 1958`), so a drive-sized READ CAPACITY would disagree wildly with
> `TD_GETGEOMETRY` and make "Read-to capacity" stop at the partition end.

### Destructive modes must never touch a live partition

Writes happen only under `-d`, always behind `are_you_sure()` unless `-y`
(`devtest.c:6162-6165`). Read-only is the default — `-t`, `-g`, `-p`, `-b` and
`-i` without `-d` are all non-destructive (non-destructive `-i` does
read/read/compare, `devtest.c:5086-5093`).

Under `-d`, writes land at byte offsets 0 and 8192 (`BUFSIZE`,
`devtest.c:109`) and, on >4 GB units, at `1<<32` and `(1<<32)+8192`.
`save_overwritten_data()`/`restore_overwritten_data()`
(`devtest.c:3854-3886`) back those two blocks up — **but only for `-t -d`, not
for `-b -d` or `-i -d`**, and `-dd` disables even that (`devtest.c:3856, 3875`).
The write benchmark (`-bd`) writes sequentially from offset 0 for up to
512 KB × 50 × 10 (`run_bandwidth()`, `devtest.c:2379-2390`); `-i -d` writes
across the whole unit.

> **Changes the testing plan.** `devtest -d`, `-bd` and `-i -d` must only ever
> be aimed at a dedicated scratch partition in a Copperline fixture, never at a
> unit holding a filesystem we care about. Worth stating in the docs, because
> partition-as-unit makes it *easier* to destroy exactly one filesystem by
> accident.

Also noted: `test_cmd_write()` adds `g_devstart` to the offset
(`devtest.c:3900`) and then `check_write()` → `do_read_cmd()` adds it again
(`devtest.c:2636`) — a double-add bug in volume mode, harmless with
device+unit invocation.

### Out-of-range behaviour

No dedicated out-of-range test and no expected-error table; `err_to_str[]`
(`devtest.c:448-499`) is display-only.

But `do_seek_capacity()` (`devtest.c:887-949`, the "Read-to capacity" row) does
a power-of-two search of single-sector reads at escalating offsets and treats
**any nonzero `io_Error` as past-the-end**. So our out-of-range reads must fail
*reliably, with any error code, without hanging or crashing* — and must not
silently succeed, or the search runs away. The proposal's bounds enforcement is
what makes this row correct.

`ERROR_PAST_END` (70) is devtest-internal (`devtest.c:441, 2646, 2672`), never
an expected device error.

### Two operational gotchas for partition-as-unit

1. **Never invoke devtest by volume name against partition.device.** Given a
   volume (`devtest -g Work:`), devtest reads the `FileSysStartupMsg`/`DosEnvec`
   and computes its own `g_devstart`/`g_devend` from
   `de_LowCyl`/`de_HighCyl`/`de_Surfaces`/`de_BlocksPerTrack`
   (`devtest.c:6166-6188`), then adds `g_devstart` to every trackdisk offset —
   so **the partition offset is applied twice**, once by devtest and once by us,
   and reads land outside the partition (where we correctly reject them). With a
   bare `partition.device <unit>` invocation `g_devstart = 0` and everything is
   clean. This belongs in the docs.

2. **Sparse unit numbering truncates the `-p` probe.** `scsi_probe_unit()`
   iterates units 0-7 × LUN 0-7 as `target + lun*10` and **stops at the first
   LUN that fails to open on each target** (`devtest.c:1233-1252`).

> **Open question for the proposal.** Our `unit = disk × 100 + partition`
> scheme is sparse and exceeds that probe space: units 0, 1, 2 on disk 0 are
> found, but disk 1's units (100, 101, …) are outside the `target + lun*10`
> grid entirely, so `devtest -p` will never list them. `-p` also exits nonzero
> if nothing opened. This doesn't break `-t`/`-g`/`-i` on an explicitly named
> unit, so it is cosmetic — but it is the first concrete cost of the ×100
> scheme, and worth weighing against a dense-numbering alternative before
> Phase 1 fixes it in place.

### Minor

- `test_cmd_read()` fails if the first 8 KB reads back as all `0x5a`
  (`devtest.c:3059-3069`, "No data"). A real filesystem or a zeroed partition is
  fine; only a partition literally full of `0x5a` trips it.
- `-g`'s "Partition" row only appears in volume mode (`devtest.c:1309-1328`), so
  with device+unit our synthetic geometry stands alone.
- `-tt` will try `TD_EJECT`/`TD_LOAD`; return `IOERR_NOCMD`. The README warns
  these "may cause your media to eject" (`README.md:370-371`).
