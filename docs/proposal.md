# Proposal: partunit.device — MBR/GPT Partitions as Amiga Block-Device Units

**Status:** Candidate (Oct 2026), revised after Phase 0. Working name was
`partition.device`; renamed to **`partunit.device`** because
`partition.library` (AROS) and `partition.resource` (Pulchart, active) already
exist by other authors and a `partition.device` would read as the device member
of a family it has no relationship to. Aminet name check for `partunit.device`
is clean on all three indexes. Disk-loaded device only; a ROM-resident form is
explicitly out of scope
**Feasibility estimate:** High (~85%) — a filter device over existing block
devices, with lide.device as the behavioural model for the device surface;
MBR/GPT are public specifications; nothing novel in the I/O path
**Dependencies:** an underlying Amiga block device per disk (`scsi.device`,
Emu68's SD device, USB mass storage, MIRAGE/copperhf under Copperline);
`expansion.library` (`MakeDosNode`/`AddBootNode`); Chris Hooper's `devtest`
(<https://github.com/cdhooper/amiga_devtest>) and the devsoak harness for
testing; the amiga-live host scanner for fixture cross-checks. **No runtime
dependency on any third-party library** — see Relationship to ptable.library

> Phase 0 is complete. Its findings are in [`phase0-notes.md`](phase0-notes.md),
> which carries the citations behind every claim in this document; this proposal
> has been revised against them. Where Phase 0 contradicted the original
> proposal, the correction is marked **[P0]**.

## Summary

A block device that parses **MBR (including extended/logical partitions) and
GPT** partition tables on configured underlying units and presents each
Amiga partition as a **unit of itself**, with bounds enforcement, synthetic
whole-unit geometry, TD64/NSD support and forwarded media-change
notification — then, like a normal disk driver, **mounts its own units at
init** via `MakeDosNode`/`AddBootNode`, with a switch for people who prefer
to mount by hand. The design goal is stated plainly: behave as closely as
possible to a normal Amiga disk driver (lide.device is the model), with
exactly one deliberate difference — **units are partitions, not whole
drives.**

**RDB is entirely out of scope.** The OS and its drivers own RDB parsing and
mounting; this device never interprets an RDB. Its only RDB awareness is a
signature sniff, used for two decisions (below): backing off a whole-drive
Amiga disk the OS already owns, and leaving a unit that contains an RDB to
the OS's own `Mounter`.

## Why

Every modern Amiga-adjacent platform boots from PC-partitioned media — the
Pi's FAT boot partition for Emu68, U-Boot/UEFI on the m68k-machine and
Amithlon-NG, CF/SD shared with PCs and Macs — and today an Amiga
filesystem inside such a disk is reachable only by hand-computing a
`LowCyl`/`HighCyl` mount entry against the raw device, which breaks the
moment geometry differs and offers no protection for the neighbouring PC
partitions.

**USB mass storage makes this the mainstream case, and a growing one.** A
USB stick or card reader is PC-partitioned by definition — nobody RDB-
partitions media they also plug into a laptop — and it is the most
removable storage the platform has, which is exactly what the media-change
design below exists for. The USB side is expanding on precisely the
platforms this targets: Poseidon on classic hardware, the Poseidon 6
backport with `xhci.device` on Emu68, `usbscsi.device` everywhere. As more
Amigas gain USB storage, "plug in a stick formatted on a PC and have its
Amiga partition appear as a unit" stops being a convenience and becomes the
expected behaviour. Emu68's `0x76` convention solves one platform's boot case
and nothing else; GPT has no Amiga type outside Emu68's own eMMC driver.

### What this does that nothing else does **[P0]**

Phase 0 found substantially more overlapping prior art than the original
proposal credited (see Prior art). After accounting for all of it, exactly two
properties remain unique to this design, and they are the justification:

1. **It works over block devices nobody will modify.** Emu68 presents typed
   partitions as units, but only on its *own* SD/eMMC devices.
   `ptable.library` does the parsing, but a device has to adopt it — and you
   cannot add anything to the `scsi.device` in a Kickstart ROM, or to
   Poseidon's `usbscsi.device`. A filter layers over them unmodified. This is
   the entire USB and legacy-controller case, and it is structurally
   unreachable by the alternatives.
2. **Bounds enforcement.** A unit refuses I/O outside its partition, so a
   filesystem bug cannot scribble over the PC side of a shared card — a
   property normal RDB partitions never had. Every mounter-based alternative
   hands the filesystem the raw device plus a `LowCyl`/`HighCyl` pair and hopes.

Secondary, still worth having: tools get a clean "this unit *is* the partition"
target (devsoak, backup, a future partition editor, `devtest`), and mount
entries become trivial (`Unit N, LowCyl 0, HighCyl geometry-1`).

## Relationship to `ptable.library` / `partition.resource` **[P0]**

Jaroslav Pulchart's `ptable.library` (Aminet, 2026-10-07) parses
RDB/MBR/GPT/flat layouts and publishes every partition into a shared
`partition.resource`, with `compactflash.device` 2.0+ and `fat95` 4.0+ already
consuming it. It is the nearest thing to this project and it predates it by a
day.

**Decision: build independently and own the parser.** Consuming it would delete
most of Phase 1, but it is three weeks old with one author and three packages,
and whether it becomes the ecosystem's parsing layer is unknown. A device whose
defining property is bounds enforcement should not rest that guarantee on an
external dependency that may not be maintained.

The reasoning is straightforward: doing it ourselves means not relying on a
package that may or may not continue to be updated. The parser is the one part
whose correctness the bounds guarantee depends on, so it should be code we
control and can fix.

**Interop is a later option, not scoped work.** If demand appears, registering
our units in `partition.resource` when it happens to be present — so `fat95`
and `cfd` can see them — is an additive change that costs nothing to defer and
would be a soft runtime check, never a dependency. Not in any phase below;
revisit if users ask. Still worth opening a conversation with the author before
release, since he may be planning a device layer himself.

## Prior art (read before building)

- **lide.device** (LIV2; GPL-2): the device model to imitate — pure block
  device in `device.c`/`ata.c`/`iotask.c` (units = drives, geometry from
  IDENTIFY, TD64/NSD/SCSI-direct), with partition handling in a *separate*
  `3rdparty/mounter` component (`mounter.c`, adapted from the A4091 driver,
  © Toni Wilen) that scans the device's own units at init and calls
  `MakeDosNode`/`AddBootNode`. Device-plus-mounter is the normal-driver
  shape this proposal copies. Built on jbilander's `SimpleDevice` skeleton.
  GPL: behavioural reference only; the OS APIs involved are documented.
  **Phase 0 read it in full — see `phase0-notes.md` for the command surface,
  the `ETD_*` contract, the LUN rule, and the mounter defects not to inherit.**
- **`ptable.library` / `partition.resource`** (Jaroslav Pulchart, 2026) **[P0]**
  — see above. <https://github.com/pulchart/amigaos-ptable>
- **`kcshdproxy`** (Sander van der Burg) **[P0]** —
  <https://github.com/svanderburg/kcshdproxy>. An Exec device that *"intercepts
  relevant SCSI and trackdisk I/O requests"* and translates offsets so an
  emulated PC drive starts at 0. **Architecturally the nearest existing thing to
  our I/O path.** Read before writing BeginIO.
- **GiggleDisk** (Geit / Guido Mersmann) **[P0]** —
  <https://www.geit.de/eng_giggledisk.html>. Analyses a drive via RDB *and* MBR,
  auto-creates mount and DOSDrivers files, auto-mounts by type; handles MBR
  (PC/Linux/Pegasos), RDB, **VHD type `0x76`**, and unpartitioned media. The
  closest functional prior art outside Emu68, and it predates everything in the
  original proposal's list. A mounter, not a unit provider.
- **The mounter lineage**: AutoMounter (Thore Böckelmann, 37.4, 2002;
  freeware, no source) monitors removable drives and mounts RDB disks, and
  MS-DOS disks by reading the MBR — first partition only, no extended
  partitions, FAT via CrossDOS/fat95; SCSIMounter and the OS 3.5/3.9
  Mounter are the same family. **MountDos 1.2** (Harry "Piru" Sintonen, 2000)
  belongs here too **[P0]** — MS-DOS tables from IDE/SCSI, generated mountlists,
  **extended and hidden partition support**, AmigaE source included. None
  presents partitions as units, none handles Amiga filesystems in MBR/GPT.
  Four of AutoMounter's behaviours are adopted below (per-device `MaxTransfer`
  override, `POLL` fallback, DOS-name uniquing, media-change re-parse).
- **Emu68's conventions — the standard to honour, not reinvent.** MBR:
  primary partitions of type `0x76` appear as separate hard-drive units
  under `brcm-sdhc.device`. GPT: Emu68 1.1 beta.1 introduced
  `brcm-emmc.device` with GPT support, where partitions of type
  **`3F82EEBC-87C9-4097-8165-89D6540557C0`** each appear as their own device
  unit. Sources are **not** in the `Emu68`/`Emu68-tools` trees (prebuilt blobs
  only) but in <https://github.com/michalsc/brcm-sdhc.device> and
  <https://github.com/michalsc/brcm-emmc.device> **[P0]**.
  The contract: **a matching type means the partition is a unit** —
  but see the three corrections below.
- **AROS `partition.library`** (RDB/MBR/EBR/GPT) at `rom/partition/`
  **[P0]** — *not* `workbench/libs/partition`. The GPT parsing reference, and
  equally a catalogue of what it forgets to validate. The UEFI specification is
  the normative source. Linux `partitions/efi.c` as a second opinion.
- **fat95** mounts FAT partitions inside MBR media on Amiga — precedent that
  MBR-parsed partitions already live happily on this OS.
- **`diskimage.device` exists twice**, from two authors, one readme explicitly
  disclaiming the other **[P0]** — the cautionary precedent for a generic device
  name, and the reason for the rename.

### Three corrections to the conventions this proposal claimed **[P0]**

1. **`0x76` is Amithlon's convention, not Emu68's.** WinUAE's comment reads
   `// check for amithlon partitions`, the support predates Emu68 by years, and
   Emu68's own docs say the type is *"an information for Emu68 (and WinUAE or
   Amithlon)"*. **WinUAE also accepts `0x30` alongside `0x76`** — we should too.
2. **Emu68's devices mount the RDB themselves.** `MountPartitions()` in each
   unit task sniffs `RDSK` at partition-relative blocks 0-15, walks the `PART`
   list, loads filesystems from `FSHD`/`LSEG`, and calls
   `MakeDosNode`/`AddBootNode`. The original claim that a typed unit's contents
   are "the OS's business, exactly as for a `brcm-emmc.device` unit" was wrong.
   Our hand-off to `Mounter` is a **divergence**, justified on its own terms:
   Emu68 is a ROM boot driver and must mount to be bootable, while we are
   disk-loaded and post-boot, so mounting RDB contents ourselves would duplicate
   an OS component for no gain.
3. **Emu68 and WinUAE match different bytes for the GPT GUID, and only Emu68's
   are spec-correct.** Verified by execution — each constant compiled verbatim
   (Emu68's for big-endian m68k, run under `qemu-m68k`) and byte-dumped, against
   an independent `sgdisk` ground truth:

   | Source | 16 bytes |
   |---|---|
   | WinUAE | `3f 82 ee bc 87 c9 40 97 81 65 89 d6 54 05 57 c0` |
   | Emu68 | `bc ee 82 3f c9 87 97 40 81 65 89 d6 54 05 57 c0` |
   | `sgdisk`, canonical string | `bc ee 82 3f c9 87 97 40 81 65 89 d6 54 05 57 c0` |

   UEFI 2.10 Appendix A requires the first three fields little-endian, so Emu68
   is right. **They do not interoperate, and no single encoding satisfies
   both.** Emu68 is the authority to follow; `sgdisk` with the canonical string
   produces its encoding, so the fixture plan below is sound. **Accept both byte
   orders on read, write only the spec encoding.** Reading both is the whole
   fix, since we never write partition tables — and it is safe rather than lax:
   the WinUAE spelling is the byteswapped form of an Amiga-specific GUID, so it
   cannot plausibly collide with another vendor's registered type and we are not
   widening the match in a way that could claim a non-Amiga partition as a unit.
   WinUAE's own ChangeLog
   writes the GUID in canonical order while its code literal does not, so this
   looks like a transcription slip worth reporting upstream. Amiberry, for its
   part, has **no GPT support whatsoever** — not the GUID, not `0x76`, no GPT
   code at all — so the convention is Emu68's alone, not ecosystem-wide.

## Architecture

```
   ┌───────────────┐  ┌───────────────┐          ┌──────────────────────┐
   │ filesystem    │  │ devtest /     │          │ mounter (in-device,  │
   │ (FFS/PFS3/SFS)│  │ devsoak / tool│          │ at init; NOMOUNT off)│
   └───────┬───────┘  └───────┬───────┘          └──────────┬───────────┘
           └──────────────────┴─────────────┬───────────────┘
                                            ▼
                     ┌──────────────────────────────────────────┐
                     │ partunit.device   unit = disk*100 + part │
                     │  parse MBR/EBR/GPT · bounds · offset     │
                     │  translate · synthetic geometry · TD64/  │
                     │  NSD · ETD change-num · change-notify    │
                     └──────────────────┬───────────────────────┘
                                        ▼
                     ┌──────────────────────────────────────────┐
                     │ underlying units (config-listed only):   │
                     │ scsi.device 0 · sdhc 0 · usbscsi 1 · …   │
                     └──────────────────────────────────────────┘
```

- **Ownership-bounded, never scanning.** The device touches only the
  underlying device/unit pairs listed in its config; it never enumerates the
  system for disks. (This is also why a ROM form is out of scope: a ROM
  module would need either config or a global scan, neither acceptable.)
- **Parser**: protective and hybrid MBR handling, GPT header + CRC32
  validation with backup-header fallback, extended partitions walked via the
  EBR chain, logical block size inherited from the underlying unit (4 KiB
  sectors translate; filesystem support for non-512 blocks is the
  filesystem's concern and documented as such). **Parser hardening
  requirements, all derived from defects found in AROS's shipping
  implementation [P0]:**
  - **Clamp `HeaderSize`** to the block size before CRC-ing over it. AROS
    defines a maximum and never uses it, so a hostile header makes its CRC loop
    read far past the buffer.
  - **CRC32 over the header's own `HeaderSize`** with the CRC field zeroed, and
    do not destroy the field in the caller's buffer.
  - **Try the backup header on *any* primary failure**, not only a CRC failure,
    and **bound the backup LBA** against the device's total sectors. AROS tries
    the backup only on bad CRC, takes the LBA from the possibly-corrupt primary
    unchecked, and its probe-path fallback is dead code besides.
  - **Honour `EntrySize` and `NumEntries`** from the header; stride by
    `EntrySize`, never `sizeof(entry)`. Nothing is fixed at 128/128.
  - **Check `EntrySize * NumEntries` for overflow** before allocating, and
    sanity-bound both.
  - **Per-entry: `EndBlock >= StartBlock`, containment within the device, and
    `StartBlock != 0`.** AROS checks none of these and produces wrapped lengths.
  - **The EBR walk must have an iteration cap, a strictly-increasing-LBA check,
    and a visited set.** AROS has *no* loop protection at all — a cyclic chain
    allocates per iteration until memory is exhausted. This is the Phase 0
    defect most likely to bite us, since we walk the same chain.
  - **Detect unused GPT entries by an all-zero *type* GUID**, not the partition
    ID.
  - **Reject a FAT superfloppy before treating block 0 as an MBR** — a FAT BPB
    can end in `0xAA55`.
  - **Check the `0x55AA` signature**, which `brcm-sdhc.device` does not.
  - **64-bit throughout.** AROS truncates every GPT start and length to 32 bits;
    Emu68 skips any partition with a non-zero high longword (a 2 TiB ceiling).
- **Which partitions become units**: MBR primaries and EBR logicals of type
  `0x76` **or `0x30`** **[P0]**, and GPT partitions of the Emu68 type GUID **in
  either byte order** **[P0]**. Nothing else — a FAT or Linux partition is never
  a unit, which is what protects the PC side of a shared card.
  - **[P0]** `0x30`'s provenance is confirmed from WinUAE's ChangeLog:
    *"accept also partition type 0x30 (another Amithlon like RDB drive inside
    real PC partition)"*. **Open question:** the same ChangeLog elsewhere says
    *"Amithlon partition type (0x78/0x30)"*, mentioning a `0x78` that appears
    nowhere in current code. Check Amithlon documentation before deciding
    whether to accept it as a third type; accepting all three on read is cheap.
- **Hybrid MBR policy, stated explicitly [P0]**: if a valid GPT is present, use
  it and ignore the MBR entirely (the UEFI rule), logging that the MBR was
  ignored. Emu68 gets this wrong — a hybrid MBR makes its protective-MBR test
  fail *after* the GPT units are already appended, so one card yields both sets
  of units.
- **Whole-drive back-off (the one real clash)**: a disk that is *entirely*
  Amiga can carry an RDB behind an MBR at block 0 placed only so Windows
  doesn't offer to format it. That disk already belongs to the OS. The device
  sniffs for the `RDSK` signature **over blocks 0-62** of the *whole disk* and
  backs off that disk entirely (config `FORCE` to override), without
  interpreting the RDB.
  - **[P0] The range is 0-62, not the documented `RDB_LOCATION_LIMIT` of 16.**
    Both WinUAE's and Amiberry's mount-time scan is 63 blocks; 0-15 is only a
    cosmetic GUI sniff. A disk with an RDB at block 20 would be mounted by the
    OS but missed by a 16-block back-off, producing exactly the clash this
    exists to prevent.
  - **[P0] Require the checksum, not just the magic.** Both emulators' *mounting*
    paths verify the sum-to-zero longword checksum *and* that the stored block
    number matches. A bare `memcmp("RDSK", …)` would false-positive and back off
    disks we should be parsing. (WinUAE's real-drive *safety check* is laxer,
    accepting magic alone — but the question we are answering is "will the OS
    mount this?", and on a bad checksum it will not. Follow the mounting path.)
    Apply the Win9x-trashed retry — zero bytes `0xDC..0xDF`, re-checksum —
    before concluding "no RDB".
  - **[P0] Clamp `rdb_SummedLongs` to the block size before summing.**
    `brcm-emmc.device` uses it unchecked as a loop bound over a 512-byte buffer,
    which is a straight over-read from malformed media. Our sniff runs on
    untrusted media by definition.
  - **[P0] Recognise scrambled forms** for back-off purposes — byteswapped
    `DRKS` and ADIDE `CPRM` (`39 10 D3 12`) are RDBs the OS may well mount. But
    **never write**: WinUAE repairs Win9x-trashed RDBs in place, and repairing
    another driver's metadata is not our business.
  - The scan step is the inherited block size, so "block 1" is byte 4096 on a
    4Kn drive, not 512 **[P0]**.
  - A typed partition that happens to contain an RDB is *not* a clash — it's a
    unit whose contents the OS mounts (see Mounting).
- **Unit model**: `unit = disk × 100 + partition`, disk index from config
  order, partition index from table order — the `scsi.device`
  board×100 + LUN×10 + target idiom, stable across boots. Slots persist for
  removable disks (see media change).
  - **[P0] `open()` must return `TDERR_BadUnitNum` — never `IOERR_OPENFAIL` —
    for every absent unit number.** lide's comment is emphatic: *"HDToolbox
    scans each LUN of a unit and stops searching if it sees an error other than
    `TDERR_BadUnitNum`. So if this is not returned, only one drive will ever be
    detected."* Our numbering is sparse, so a scanner walking 0…99 before
    reaching 100 must see `TDERR_BadUnitNum` for all 97 gaps. lide itself is
    asymmetric here (`unitnum > highestUnit` gives `IOERR_OPENFAIL`); **do not
    copy that.**
  - **[P0] Accepted cost:** `devtest -p` probes as `target + lun*10` and stops
    at the first open failure per target, so it will never list disks past the
    first. Cosmetic — `-t`, `-g` and `-i` all work on an explicitly named unit.
  - **[P0] Divergence from Emu68**, which reserves unit 0 for the whole medium
    (default read-only) and numbers partitions from 1. We expose no whole-disk
    unit, because the underlying device already provides one and re-presenting
    it is the double-presentation risk below.
- **Device surface, modelled on lide**: `NSCMD_DEVICEQUERY` advertising TD,
  TD64 and NSD command sets; `TD_GETGEOMETRY` returning a synthetic geometry
  in which the whole unit is the partition (1 surface, 1 block/track,
  cylinders = blocks, block size inherited); `TD_CHANGENUM`/`CHANGESTATE`/
  `ADDCHANGEINT`/`REMCHANGEINT`/`PROTSTATUS` forwarded; `CMD_UPDATE`/`CLEAR`
  forwarded. **Plus the `ETD_*` family — see below.**
  - **[P0] The synthetic geometry is confirmed safe.** devtest does no
    consistency checking of `TD_GETGEOMETRY` whatsoever — it never verifies
    `dg_TotalSectors == Cylinders × Heads × TrackSectors`, and `dg_Heads == 1` /
    `dg_TrackSectors == 1` pass unremarked. Only `dg_SectorSize` matters to it.
  - **[P0] This diverges from Emu68 deliberately.** Emu68 reports a fixed
    128 heads × 64 sectors with `dg_Cylinders = TotalSectors / 8192`, which
    *truncates* — up to 8191 blocks fall off the end of any mount entry derived
    from the cylinder fields. Ours is exact and makes mount entries trivially
    `LowCyl 0 / HighCyl blocks-1`. AROS independently arrives at the same
    answer, falling back to "one block per cylinder (flat LBA)" whenever a
    partition is not cylinder-aligned — which, for modern 1 MiB-aligned tables,
    is always.
  - **[P0] Clamp `dg_TotalSectors` to `ULONG_MAX`** rather than wrapping; the
    field is a ULONG and our units are 64-bit addressable. `memset` the whole
    struct first so reserved fields are clean, and reject NULL or odd `io_Data`
    with `IOERR_BADADDRESS`.
  - **[P0] Build the advertised command list per-unit**, from what each child
    actually advertised. lide shares one global list across ATA and ATAPI units,
    so its CD units claim TD64 support they do not have. We front heterogeneous
    children by design, so this matters more for us than for lide.
  - **[P0] Report `SizeAvailable` as the bytes actually written**, and accept an
    `io_Length` larger than our own struct. devtest's local
    `NSDeviceQueryResult` is 36 bytes against lide's 16; it never reads
    `SizeAvailable`, so getting this wrong is invisible to the test but
    misleading to real callers.
- **The `ETD_*` family is mandatory [P0]** — this was missing from the original
  proposal, and it carries devtest's single strictest requirement.
  `test_etd_command()` sends every `ETD_*`/`NSCMD_ETD_*` command with
  `iotd_Count = 0` and demands rejection with **exactly `TDERR_DiskChanged`**;
  accepting it is a failure. So:
  - Implement `ETD_READ`, `ETD_WRITE`, `ETD_FORMAT` and
    `NSCMD_ETD_READ64`/`WRITE64`/`FORMAT64`, each comparing `iotd_Count`
    against the unit's change number. (lide implements no seek command at all,
    so `ETD_SEEK` is optional.)
  - **The comparison is `iotd_Count < changeCount`, not `!=`** — a count ahead
    of ours is accepted; only a stale one is rejected.
  - **Initialise the change count to 1, not 0.** lide starts at 1, so
    `iotd_Count == 0` is always rejected, on every unit including fixed ones.
    Starting at 0 would make devtest's strictest test pass where lide's
    fails — i.e. we would be wrong.
  - The check must **precede** the media-presence and range checks, and must
    run in the IO task (arriving via `ReplyMsg`), not in `BeginIO`.
  - The change number is **ours, per unit**, derived from but not identical to
    the underlying unit's: a media change invalidates every unit on that disk.
  - Do not inherit lide's two inconsistencies here: `TD_FORMAT` handled but not
    advertised, and `ETD_FORMAT` advertised but unreachable because `BeginIO`
    never dispatches it.
- **Offset translation in 64-bit arithmetic**: partition start LBA + request
  offset; the underlying command is TD64/NSD64 when the absolute address
  crosses 4 GB and the underlying unit advertised them (discovered via its
  own `NSCMD_DEVICEQUERY` at init), plain `CMD_READ`/`WRITE` otherwise.
  Beyond 2 TB the device addresses fine; the `DosEnvec` cylinder fields are
  32-bit, so mounting such partitions is the filesystem's and mount entry's
  limitation, documented as lide documents it.
  - **[P0] The convention**: `io_Offset` is the low 32 bits of a *byte* offset,
    `io_Actual` the high 32 bits (the NSD spec's `io_HighOffset`).
  - **[P0] Zero `io_Actual` on the incoming 32-bit commands before using it as a
    high offset — callers leave it dirty.** lide's own mounter is one of the
    dirty callers. This is a one-line omission that produces wild
    mis-addressing.
- **Bounds enforcement on every data command**: out-of-range requests fail
  with an I/O error rather than touching the neighbour. This is the device's
  defining safety property.
  - **[P0] Use `IOERR_BADADDRESS`**, matching Emu68, which applies the same
    two-part test (`offset >= count || offset + length/blocksize > count`)
    before every transfer *and* inside `HD_SCSICMD`, adding the partition start
    only after it passes. `IOERR_BADLENGTH` when `io_Length` is not a whole
    multiple of the block size.
  - **[P0] Out-of-range reads must fail reliably and must not hang**, because
    devtest's "Read-to capacity" row does a power-of-two search treating any
    nonzero error as past-the-end; a silent success makes it run away.
- **`HD_SCSICMD` policy**: non-addressing commands (`INQUIRY`, `TEST UNIT
  READY`, `MODE SENSE`, `READ CAPACITY` with the *partition's* size
  substituted) are forwarded; `READ`/`WRITE (6/10/12/16)` have their LBA
  rewritten by the partition offset and bounds-checked; everything else is
  rejected. Bounds enforcement must not have a SCSI-direct back door.
  - **[P0] lide's accepted set is almost exactly this list**, arrived at
    independently: `0xA1`, `0x00`, `0x12`, `0x1A`, `0x25`, `0x9E`,
    `0x08`/`0x0A`, `0x28`/`0x2A`, `0x88`/`0x8A`, everything else
    `IOERR_NOCMD`.
  - **[P0] `MODE SENSE(6)` is not optional**: `devtest -g` ends by calling
    `scsi_read_mode_pages()` and returns its result unchanged, so rejecting it
    makes `devtest -g` exit nonzero even though every printed line is fine. It
    is the only SCSI command whose refusal produces a failing exit code.
  - **[P0] Honour `SCSIF_AUTOSENSE` and fill `scsi_SenseActual`** — devtest
    always sets it. Mirror lide's epilogue exactly: `scsi_CmdActual =
    scsi_CmdLength` always; on failure `scsi_Status = SCSI_CHECK_CONDITION` and
    return **`HFERR_BadStatus`**, with the real cause only in the sense data.
    Stashing the Amiga error code in the sense FRU byte is a lide convention
    worth copying.
  - **[P0] Pool the `SCSICmd` structures** two per unit with in-use flags, as
    lide does, rather than allocating per request — explicitly to avoid memory
    fragmentation, with the second so autosense can be issued while the first
    is live.
- **`MaxTransfer`/`Mask`** default to the underlying device's known-safe
  values and are overridable per underlying device in config (AutoMounter's
  lesson: some IDE/ATAPI paths need `0x0001FE00`).
  - **[P0] The override must be documented as dangerous.** The DosEnvec we
    publish is the only thing constraining callers' buffers and lengths, and
    filesystems trust it absolutely. Passing a child's values through is
    correct and conservative; **widening them obliges us to bounce buffers on
    every path including `HD_SCSICMD`**, or we hand a DMA-constrained child an
    address it cannot reach.
- **Alignment: pick one policy and apply it consistently [P0].** lide bounces
  odd buffers in the ATAPI path (*"Some bozo with an unaligned data buffer…
  lookin' at you HDToolbox!"*) but *rejects* them in SCSI passthrough. Assume
  HDToolbox will be pointed at our units — it is the named reason for three
  separate lide workarounds.
- **IORequest lifecycle [P0]** — Olaf Barthel's trackfile.device rules, which a
  filter device gets wrong by omission: set `ln_Type = NT_REPLYMSG` in `open` so
  `CheckIO` doesn't report a fresh request busy and `WaitIO` doesn't hang, and
  flip it to `NT_MESSAGE` at the top of `BeginIO` so `WaitIO` on a genuinely
  pending request works. **Both halves are required.** Invalidate
  `io_Device`/`io_Unit` on open failure and unconditionally in `close`. Validate
  every request on every entry point, checking both unit membership and
  `io_Device`. `AbortIO` must return 0 when nothing was aborted, and must
  `Remove`/`ReplyMsg` inside a single `Disable()`.
- **Never expunge [P0]** — *"If expunged the driver would be gone until
  reboot"*, and Expunge runs from the memory allocator so it may never `Wait()`.

## Mounting (the normal-driver behaviour)

At init, after parsing, the device presents every typed partition as a
unit, then — unless `NOMOUNT` is set — mounts those it can:

- **A unit containing an RDB** (`RDSK` signature, checksum-valid, within the
  unit's first 16 blocks) is left alone: that is the OS's job, done by the OS's
  own `Mounter` (3.5+) or AutoMounter. partunit.device only presents the unit;
  the tool's `LIST` marks it "RDB inside — use Mounter". **[P0] This is a
  deliberate divergence from Emu68**, which mounts such contents itself; the
  justification is that Emu68 is a ROM boot driver and must, whereas we are
  disk-loaded and post-boot and would merely be duplicating `Mounter`.
- **A unit containing a bare filesystem** is mounted by the in-device
  mounter: post-boot `AddBootNode` simply mounts (boot priority is
  irrelevant for a disk-loaded device, and root bootability stays the
  platform's job). The `DosEnvec` is the synthetic geometry above with
  `LowCyl 0`/`HighCyl N−1`; the filesystem comes from ROM (FFS) by DosType
  or from a handler path (`L:pfs3aio`) per config, since MBR/GPT have no
  LSEG equivalent. The DosType comes from the GPT partition *name* field
  where it follows a `<DosType>:<DOSName>` grammar (e.g. `DOS3:Work` — a
  small addition on top of the existing GUID, never a competing type), else
  from a per-partition config line (the only option under MBR, which has
  nowhere to put it). A typed unit with no recoverable DosType is presented
  unmounted.
  - **[P0] This case has no emulator precedent on real media.** WinUAE routes a
    typed partition to `rdb_mount()` and fails it outright if there is no RDB
    inside; Emu68 likewise expects an RDB. A bare filesystem inside a typed
    partition is therefore a **genuine extension** of the convention, not a
    match to it. That is fine, and it is most of the convenience this project
    offers — but it should be labelled as an extension in the docs.
  - **[P0] AROS's alternative is worth knowing but not imitating**: it carries
    the DosType in the first four bytes of the type GUID itself, matching a
    *family* of GUIDs rather than one. Non-conformant, and it would break
    interoperation with Emu68.
- DOS names are **uniqued AutoMounter-style** (`.0`, `.1`, …) when two disks
  offer the same name.
  - **[P0] Uniquing must consult the live `DosList`, not just
    `ExpansionBase->MountList`.** lide's mounter checks only the MountList,
    which is wrong for exactly our case — a disk-loaded device mounting
    post-boot into a running system, where the colliding name may belong to an
    already-mounted volume. This is the lide mounter defect most relevant to us.
    Also avoid its off-by-one, which yields `DH0.9.1` rather than failing
    cleanly at `.9`.

## Media change (the hard part of being a device)

Removable underlying units change their partition table when media changes.
The device forwards change notification and, on a change, **re-parses the
table and refreshes its unit slots**: partitions that vanished report
no-disk via `TD_CHANGESTATE` (like a trackdisk unit with the disk ejected)
rather than disappearing as units; new partitions populate free slots;
mounted DOS devices for vanished partitions are left to the filesystem's
own no-disk handling, exactly as with a floppy. Drivers without change
interrupts get AutoMounter's `POLL` fallback with a configurable interval.

**[P0] Specifics confirmed from lide:**

- `TD_CHANGESTATE` reports presence in `io_Actual` (0 = present, 1 = absent)
  and **`io_Error` is always 0** — state is never an error. Likewise
  `TD_PROTSTATUS` swallows a lower-layer `TDERR_WriteProt` and reports
  `io_Error = 0, io_Actual = 1`; write protection is a status.
- `TD_ADDCHANGEINT` must set `IOF_QUICK` so the request is **never replied** —
  it stays outstanding until removed — and the list must be manipulated under
  `Disable()`, not `Forbid()`, because it is walked from `Cause()`-driven
  interrupt context. `TD_REMCHANGEINT` returns success whether or not the
  request was found, and does not reply it.
- On a change edge, fire the single `TD_REMOVE` interrupt first, then every
  queued `TD_ADDCHANGEINT`, **all inside one `Forbid()`/`Permit()`**.
- The `POLL` precedent: lide uses one `UNIT_VBLANK` timer per channel at a
  2-second interval, created only if that channel has removable units.
- **A vanished-partition unit needs a defined `TD_GETGEOMETRY` answer.** Emu68
  and lide both zero the block size on media removal, so geometry reports
  `dg_SectorSize = 0`. Decide and document ours rather than leaving it to fall
  out of the code.

## Configuration (`ENV:partunit/config`, persisted in `ENVARC:`)

    DISK scsi.device 0                      ; disk 0 → units 0..99
    DISK usbscsi.device 1 POLL 5 MAXTRANSFER 0x1FE00
    DISK brcm-sdhc.device 0 FORCE           ; parse even if an RDB signature is seen
    PART 0.2 DOSTYPE DOS3 NAME Work         ; MBR partitions need DosType/name supplied
    PART 1.0 HANDLER L:pfs3aio
    NOMOUNT                                 ; present units only; mount by hand

Line-based, trivial to parse, `ENV:` read live at init, `ENVARC:` never read
directly — the house's ENV/ENVARC rule.

## The tool

One Shell command: `LIST` (disks, partitions, unit numbers, DosTypes, mounted
state, blocked/backed-off disks with the reason), `EMIT` (ready-made
DOSDrivers entries for every unit, for the `NOMOUNT` audience), and
`RESCAN disk` (re-parse on demand). RC-coded for scripts, SanaInfo-style.

## Testing

- **`devtest`** — <https://github.com/cdhooper/amiga_devtest>, lide's own
  block-device conformance tool, run against partunit.device units exactly as
  it is against lide.
  - **[P0] "devtest green" is not a meaningful criterion.** `devtest -t`
    deliberately discards individual failures and always exits 0 — *"no driver
    passes all tests"*. The real criterion is a **reviewed clean report** plus
    **`-p`, `-g` and `-i` exiting zero**.
  - **[P0] Invoke as `devtest partunit.device <unit>`, never by volume name.**
    Given a volume, devtest computes its own `g_devstart` from the
    `DosEnvec` and adds it to every offset — so the partition offset is applied
    twice, once by devtest and once by us, and reads land outside the partition
    where we correctly reject them. This belongs in the docs.
  - **[P0] Never run `-d`, `-bd` or `-i -d` against a live partition.** They
    write from offset 0; the save/restore only covers two 8 KB blocks and only
    under `-t -d`, and the write benchmark doesn't even do that. Destructive
    runs go to a dedicated scratch partition in a fixture. Partition-as-unit
    makes it *easier* to destroy exactly one filesystem by accident.
- **devsoak** — correctness under load; partunit.device is a natural first
  customer for that harness.
- **Deterministic rig**: Copperline with a copperhf/MIRAGE-backed image
  containing an MBR+EBR layout and a GPT layout, Amiga filesystems inside,
  byte-replayable; fixtures written on the host with `sgdisk`/`parted` and
  cross-checked by the amiga-live scanner, which reads the same layouts.
  - **[P0] Verify the GUID byte order empirically** against a real
    Emu68-formatted card before relying on the analysis above. `sgdisk` with the
    canonical string should produce Emu68's encoding; confirm it.
- **The differential**: the same partition read through a partunit.device
  unit and through a hand-computed `LowCyl`/`HighCyl` entry on the raw
  device must be byte-identical — the OS itself as oracle.
- **Bounds**: a fixture that attempts reads/writes just past each partition
  boundary and asserts the error (and that the neighbour is untouched),
  covering SCSI-direct as well as TD commands.
- **[P0] A partition starting beyond 4 GB.** devtest's 4 GB-crossing sub-tests
  only run when the *unit* exceeds 4 GB, but our hazard is a *small* partition
  whose absolute start is past 4 GB. devtest will never test this; our fixtures
  must.
- **[P0] Parser-hardening fixtures**, one per hardening requirement above: an
  oversized `HeaderSize`, a corrupt primary header with a valid backup, a
  corrupt backup, `EntrySize`/`NumEntries` that overflow when multiplied, an
  entry with `EndBlock < StartBlock`, a cyclic EBR chain, a hybrid MBR, a FAT
  superfloppy whose BPB ends in `0xAA55`, and a 4Kn-sector image.
- **[P0] An `ETD_*` fixture**: assert `iotd_Count = 0` is rejected with exactly
  `TDERR_DiskChanged`, that the count read from `TD_CHANGENUM` succeeds, and
  that a media change invalidates every unit on that disk.
- **Media change**: Copperline-scripted eject/insert of an image with a
  different table; assert slot persistence and no-disk reporting.
- **Whole-drive back-off**: an image with MBR at block 0 and `RDSK` at
  block 1; assert the disk is skipped and the reason is reported by `LIST`.
  **[P0] Plus one with the RDB at block 20**, to cover the 0-62 range, and one
  with a magic-but-bad-checksum `RDSK` that must *not* trigger back-off.
- **RDB-inside-unit**: a typed partition whose first blocks carry `RDSK`;
  assert it is presented as a unit, left unmounted by partunit.device, and
  mounts correctly via the OS `Mounter` on that unit.

## Phases

**Phase 0 — complete.** Findings in [`phase0-notes.md`](phase0-notes.md). The
GPT GUID byte order has been verified by execution against an `sgdisk` ground
truth. Remaining loose ends:

- **Confirm the byte order end-to-end against WinUAE.** The analysis is solid
  but the Windows half rests on an inference — specifically, how Windows
  populates `Gpt.PartitionType` before WinUAE's `memcmp` sees it. An
  end-to-end test replaces the whole chain of reasoning.
  - **It needs a *physical* drive, not a virtual one.** WinUAE's GPT handling
    is entirely in `od-win32/hardfile_win32.cpp` and works by asking Windows
    for the layout (`IOCTL_DISK_GET_DRIVE_LAYOUT_EX`); there is **no GPT
    parsing of HDF files anywhere in the tree**, and the Unix backend never
    reads sector 0. A GPT inside an HDF is invisible to it and falls through to
    the RDB sniff. So: a USB stick partitioned with
    `sgdisk -t 1:3F82EEBC-87C9-4097-8165-89D6540557C0`, on a Windows host, or a
    VM with raw disk passthrough. Check whether the harddrive picker offers a
    `:GP#…` entry, and read `write_log`.
  - **Pair it with a patched-constant run.** Change WinUAE's literal to
    `{0x3F82EEBC, 0x87C9, 0x4097, …}` and retest the same stick. If it then
    recognises the partition, that proves the byte order is the sole defect and
    gives the upstream report a one-line fix with evidence attached.
  - Host CPU endianness is not the variable — all Windows hosts are
    little-endian — but the test settles it regardless.
- Decide the `0x78` question (see above).
- Confirm `RDB_LOCATION_LIMIT` is 16 in `brcm-emmc.device`; the macro lives in
  NDK headers that were not available, so Emu68's in-partition scan range is
  assumed, not verified.
- Check EAB for prior art — it is behind Anubis anti-bot and was not searchable;
  needs a browser.
- Open a conversation with the `ptable.library` author.
- Report upstream: the WinUAE GUID byte order (its own ChangeLog writes the
  canonical order, so this reads as a transcription slip), and the Emu68
  `emmc_Units[5]` out-of-bounds write on a fifth Amiga-type GPT partition
  — `&Units[5]` lands exactly on `emmc_UnitCount`, and the release notes
  advertise *"No more 4-partition limit"* that the array does not support.
  Search the issue trackers first; no existing report was found, but the
  trackers could not be searched, so absence is not established.

**Phase 1 (1 weekend):** parser (MBR/EBR/GPT, CRC, hybrid sniff) as a
host-testable C module with the fixture set **including every hardening fixture
above**; device skeleton presenting units with offset translation, bounds,
synthetic geometry, NSD query; `devtest` reviewed clean with `-p`/`-g`/`-i`
green on a Copperline image.

**Phase 2 (1 weekend):** TD64/NSD64 translation and 4 GB crossing, **the
`ETD_*` family and the per-unit change number**, change notification forwarding
+ `POLL`, media-change re-parse, `HD_SCSICMD` policy; devsoak run; bounds,
4 GB-start, `ETD_*` and media-change fixtures.

**Phase 3 (½–1 weekend):** the in-device mounter (`MakeDosNode`/
`AddBootNode`, `DosList`-aware name uniquing, `NOMOUNT`), config parser, the
tool, docs including the GUID convention and the byte-order caveat, Aminet
release via aminet-release-action.

**Not scheduled:** optional `partition.resource` registration, if demand
appears.

Total: **~3 weekends** — up slightly from the original estimate, because the
parser is now ours and carries the hardening work, and the `ETD_*` surface was
missing from the original scope.

## Risks

- **Removable-media state** is the genuinely tricky code: slot persistence,
  open units on vanished media, filesystems mid-write. Mitigated by copying
  trackdisk's no-disk semantics exactly and testing eject/insert
  deterministically under Copperline.
- **`HD_SCSICMD` rewriting** is where a bounds bypass would hide; the
  allowlist-and-rewrite policy is strict on purpose, and the bounds fixture
  covers SCSI-direct as well as TD commands.
- **Our own parser is now the main correctness risk [P0].** Phase 0 found
  genuine, exploitable defects in AROS's shipping GPT and EBR code — an
  unbounded `HeaderSize` CRC read, unchecked entry-array arithmetic, and an EBR
  walk with no loop protection at all. We are writing the same code. The
  hardening list and its fixtures are the mitigation, and the host-testable
  parser module exists precisely so they can be run cheaply and often.
- **Ecosystem fragmentation [P0].** `ptable.library` is solving the parsing
  problem in parallel. Owning our parser is a deliberate choice to avoid a
  dependency, at the cost of being a second implementation. Mitigated by
  optional `partition.resource` registration and by talking to the author early.
- **Double presentation** — wrapping an underlying device that already
  presents typed partitions as units (Emu68's own SD/eMMC devices) would
  show the same partition twice. The config lists underlying units
  explicitly, and the docs say plainly: partunit.device is for devices
  that *don't* do this themselves (`scsi.device`, USB mass storage, CF/SD
  readers); on Emu68's own devices, don't list them.
- **Name collision** — resolved for now: `partunit.device` is clean on Aminet's
  name, readme and content indexes, and `partition.device` was abandoned to
  avoid implying kinship with `partition.library`/`partition.resource`. The
  `diskimage.device` precedent (the same name shipped twice by two authors) is
  the reason this got a decision rather than a shrug. **Loose end:** the git
  repository is still named `partition-device`.

## Success criteria

- `devtest` produces a reviewed clean report with `-p`, `-g` and `-i` exiting
  zero, on partunit.device units over a Copperline image and on at least one
  real underlying device (a CF/SD card shared with a PC). **[P0]**
- Byte-identical differential against hand-mounted `LowCyl`/`HighCyl`
  entries on the same partitions.
- Out-of-range TD *and* SCSI-direct requests fail with `IOERR_BADADDRESS`
  without touching the neighbouring partition.
- Every parser-hardening fixture is rejected cleanly, with no hang, no
  over-read and no runaway allocation. **[P0]**
- `ETD_*` with `iotd_Count = 0` is rejected with exactly `TDERR_DiskChanged`,
  and a media change invalidates every unit on the affected disk. **[P0]**
- A PC-partitioned card with FAT + two Amiga partitions (one FFS from ROM,
  one PFS3 via handler path) mounts at init with no mount entries written by
  hand; `NOMOUNT` + `EMIT` reproduces the same mounts by hand.
- Media change on a removable underlying unit re-parses correctly with
  stable unit numbers and no-disk reporting.
- A GPT card prepared per Emu68's convention (type
  `3F82EEBC-87C9-4097-8165-89D6540557C0`) presents the same units under
  partunit.device on any underlying device as it does under
  `brcm-emmc.device` on a PiStorm. **[P0] The equivalent claim for WinUAE is
  dropped** — its byte order does not match Emu68's, so the two cannot both be
  satisfied by one encoding on write.
