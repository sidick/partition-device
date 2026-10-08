# Proposal: partition.device — MBR/GPT Partitions as Amiga Block-Device Units

**Status:** Candidate (Oct 2026). Working name `partition.device` (generic enough that a Phase 0 Aminet collision check is required). Disk-loaded device only; a ROM-resident form is explicitly out of scope
**Feasibility estimate:** High (~85%) — a filter device over existing block devices, with lide.device as the behavioural model for the device surface; MBR/GPT are public specifications; nothing novel in the I/O path
**Dependencies:** an underlying Amiga block device per disk (`scsi.device`, Emu68's SD device, USB mass storage, MIRAGE/copperhf under Copperline); `expansion.library` (`MakeDosNode`/`AddBootNode`); Chris Hooper's `devtest` and the devsoak harness for testing; the amiga-live host scanner for fixture cross-checks

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
expected behaviour. Emu68's `0x76` "RDB inside an MBR partition" convention solves
one platform's boot case and nothing else; GPT has no Amiga type at all.

Presenting partitions as **units** rather than only mounting them buys the
things a mounter alone cannot: a unit refuses I/O outside its partition (a
filesystem bug cannot scribble over the PC side of a shared card — a
property normal RDB partitions never had), tools get a clean "this unit *is*
the partition" target (devsoak, backup, a future partition editor, `devtest`),
and mount entries become trivial (`Unit N, LowCyl 0, HighCyl geometry-1`).

## Prior art (read before building)

- **lide.device** (LIV2; GPL-2): the device model to imitate — pure block
  device in `device.c`/`ata.c`/`iotask.c` (units = drives, geometry from
  IDENTIFY, TD64/NSD/SCSI-direct), with partition handling in a *separate*
  `3rdparty/mounter` component (`mounter.c`, adapted from the A4091 driver,
  © Toni Wilen) that scans the device's own units at init and calls
  `MakeDosNode`/`AddBootNode`. Device-plus-mounter is the normal-driver
  shape this proposal copies. Built on jbilander's `SimpleDevice` skeleton.
  GPL: behavioural reference only; the OS APIs involved are documented.
- **The mounter lineage**: AutoMounter (Thore Böckelmann, 37.4, 2002;
  freeware, no source) monitors removable drives and mounts RDB disks, and
  MS-DOS disks by reading the MBR — first partition only, no extended
  partitions, FAT via CrossDOS/fat95; SCSIMounter and the OS 3.5/3.9
  Mounter are the same family. None presents partitions as units, none
  handles Amiga filesystems in MBR/GPT, none handles GPT. Four of
  AutoMounter's behaviours are adopted below (per-device `MaxTransfer`
  override, `POLL` fallback, DOS-name uniquing, media-change re-parse).
- **Emu68's conventions — the standard to honour, not reinvent.** MBR:
  primary partitions of type `0x76` appear as separate hard-drive units
  under `brcm-sdhc.device`. GPT: Emu68 1.1 beta.1 introduced
  `brcm-emmc.device` with GPT support, where partitions of type
  **`3F82EEBC-87C9-4097-8165-89D6540557C0`** each appear as their own device
  unit (and GPT lifts MBR's four-partition limit); WinUAE recognises the
  same GUID. The contract in both cases is the same and is the one
  partition.device adopts: **a matching type means the partition is a unit
  and its entire contents are Amiga-owned** — whatever is inside (an RDB, a
  bare filesystem) is the OS's business once the unit exists. The
  partition-as-unit model therefore has precedent in exactly the ecosystem
  this targets; partition.device generalises it from Emu68's own devices to
  every underlying block device.
- **AROS `partition.library`** (RDB/MBR/EBR/GPT) and Linux `partitions/efi.c`:
  GPT parsing references; the UEFI specification is the normative source.
- **fat95** mounts FAT partitions inside MBR media on Amiga — precedent that
  MBR-parsed partitions already live happily on this OS.

## Architecture

```
   ┌───────────────┐  ┌───────────────┐          ┌──────────────────────┐
   │ filesystem    │  │ devtest /     │          │ mounter (in-device,  │
   │ (FFS/PFS3/SFS)│  │ devsoak / tool│          │ at init; NOMOUNT off)│
   └───────┬───────┘  └───────┬───────┘          └──────────┬───────────┘
           └──────────────────┴─────────────┬───────────────┘
                                            ▼
                     ┌──────────────────────────────────────────┐
                     │ partition.device  unit = disk*100 + part │
                     │  parse MBR/EBR/GPT · bounds · offset     │
                     │  translate · synthetic geometry · TD64/  │
                     │  NSD · change-notify forwarding          │
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
  filesystem's concern and documented as such).
- **Which partitions become units**: MBR primaries and EBR logicals of type
  `0x76`, and GPT partitions of the Emu68 type GUID. Nothing else — a FAT or
  Linux partition is never a unit, which is what protects the PC side of a
  shared card.
- **Whole-drive back-off (the one real clash)**: the RDB block may sit
  anywhere in blocks 0–15, so a disk that is *entirely* Amiga can carry an
  RDB at block 1 behind an MBR at block 0 placed only so Windows doesn't
  offer to format it. That disk already belongs to the OS. The device sniffs
  for the `RDSK` signature in blocks 1–15 of the *whole disk* and backs off
  that disk entirely (config `FORCE` to override), without interpreting the
  RDB. A typed partition that happens to contain an RDB is *not* a clash —
  it's a unit whose contents the OS mounts (see Mounting).
- **Unit model**: `unit = disk × 100 + partition`, disk index from config
  order, partition index from table order — the `scsi.device`
  board×100 + LUN×10 + target idiom, stable across boots. Slots persist for
  removable disks (see media change).
- **Device surface, modelled on lide**: `NSCMD_DEVICEQUERY` advertising TD,
  TD64 and NSD command sets; `TD_GETGEOMETRY` returning a synthetic geometry
  in which the whole unit is the partition (1 surface, 1 block/track,
  cylinders = blocks, block size inherited); `TD_CHANGENUM`/`CHANGESTATE`/
  `ADDCHANGEINT`/`REMCHANGEINT`/`PROTSTATUS` forwarded; `CMD_UPDATE`/`CLEAR`
  forwarded.
- **Offset translation in 64-bit arithmetic**: partition start LBA + request
  offset; the underlying command is TD64/NSD64 when the absolute address
  crosses 4 GB and the underlying unit advertised them (discovered via its
  own `NSCMD_DEVICEQUERY` at init), plain `CMD_READ`/`WRITE` otherwise.
  Beyond 2 TB the device addresses fine; the `DosEnvec` cylinder fields are
  32-bit, so mounting such partitions is the filesystem's and mount entry's
  limitation, documented as lide documents it.
- **Bounds enforcement on every data command**: out-of-range requests fail
  with an I/O error rather than touching the neighbour. This is the device's
  defining safety property.
- **`HD_SCSICMD` policy**: non-addressing commands (`INQUIRY`, `TEST UNIT
  READY`, `MODE SENSE`, `READ CAPACITY` with the *partition's* size
  substituted) are forwarded; `READ`/`WRITE (6/10/12/16)` have their LBA
  rewritten by the partition offset and bounds-checked; everything else is
  rejected. Bounds enforcement must not have a SCSI-direct back door.
- **`MaxTransfer`/`Mask`** default to the underlying device's known-safe
  values and are overridable per underlying device in config (AutoMounter's
  lesson: some IDE/ATAPI paths need `0x0001FE00`).

## Mounting (the normal-driver behaviour)

At init, after parsing, the device presents every typed partition as a
unit, then — unless `NOMOUNT` is set — mounts those it can:

- **A unit containing an RDB** (`RDSK` signature within the unit's first
  16 blocks) is left alone: that is the OS's job, done by the OS's own
  `Mounter` (3.5+) or AutoMounter, exactly as for a `brcm-emmc.device` unit
  on a PiStorm. partition.device only presents the unit; the tool's `LIST`
  marks it "RDB inside — use Mounter".
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
  nowhere to put it). DOS names are **uniqued AutoMounter-style** (`.0`,
  `.1`, …) when two disks offer the same name. A typed unit with no
  recoverable DosType is presented unmounted.

## Media change (the hard part of being a device)

Removable underlying units change their partition table when media changes.
The device forwards change notification and, on a change, **re-parses the
table and refreshes its unit slots**: partitions that vanished report
no-disk via `TD_CHANGESTATE` (like a trackdisk unit with the disk ejected)
rather than disappearing as units; new partitions populate free slots;
mounted DOS devices for vanished partitions are left to the filesystem's
own no-disk handling, exactly as with a floppy. Drivers without change
interrupts get AutoMounter's `POLL` fallback with a configurable interval.

## Configuration (`ENV:partition/config`, persisted in `ENVARC:`)

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

- **`devtest`** (Chris Hooper) — lide's own block-device conformance tool,
  run against partition.device units exactly as it is against lide.
- **devsoak** — correctness under load; partition.device is a natural first
  customer for that harness.
- **Deterministic rig**: Copperline with a copperhf/MIRAGE-backed image
  containing an MBR+EBR layout and a GPT layout, Amiga filesystems inside,
  byte-replayable; fixtures written on the host with `sgdisk`/`parted` and
  cross-checked by the amiga-live scanner, which reads the same layouts.
- **The differential**: the same partition read through a partition.device
  unit and through a hand-computed `LowCyl`/`HighCyl` entry on the raw
  device must be byte-identical — the OS itself as oracle.
- **Bounds**: a fixture that attempts reads/writes just past each partition
  boundary and asserts the error (and that the neighbour is untouched).
- **Media change**: Copperline-scripted eject/insert of an image with a
  different table; assert slot persistence and no-disk reporting.
- **Whole-drive back-off**: an image with MBR at block 0 and `RDSK` at
  block 1; assert the disk is skipped and the reason is reported by `LIST`.
- **RDB-inside-unit**: a typed partition whose first blocks carry `RDSK`;
  assert it is presented as a unit, left unmounted by partition.device, and
  mounts correctly via the OS `Mounter` on that unit.

## Phases

**Phase 0 (evenings):** Aminet name check; read lide's `device.c`/`iotask.c`
and `SimpleDevice` for the skeleton shape, `mounter.c` and AutoMounter's
readme as behavioural references, AROS `partition.library` for GPT corner
cases; confirm against Emu68 1.1's `brcm-emmc.device` and WinUAE that a typed
partition is treated as a unit with Amiga-owned contents and nothing more,
so partition.device matches both exactly; confirm `devtest` expectations
for synthetic geometry.

**Phase 1 (1 weekend):** parser (MBR/EBR/GPT, CRC, hybrid sniff) as a
host-testable C module with the fixture set; device skeleton presenting
units with offset translation, bounds, synthetic geometry, NSD query;
`devtest` green on a Copperline image.

**Phase 2 (1 weekend):** TD64/NSD64 translation and 4 GB crossing, change
notification forwarding + `POLL`, media-change re-parse, `HD_SCSICMD` policy;
devsoak run; bounds and media-change fixtures.

**Phase 3 (½–1 weekend):** the in-device mounter (`MakeDosNode`/
`AddBootNode`, name uniquing, `NOMOUNT`), config parser, the tool, docs
including the GUID convention, Aminet release via aminet-release-action.

Total: **~2½–3 weekends.**

## Risks

- **Removable-media state** is the genuinely tricky code: slot persistence,
  open units on vanished media, filesystems mid-write. Mitigated by copying
  trackdisk's no-disk semantics exactly and testing eject/insert
  deterministically under Copperline.
- **`HD_SCSICMD` rewriting** is where a bounds bypass would hide; the
  allowlist-and-rewrite policy is strict on purpose, and the bounds fixture
  covers SCSI-direct as well as TD commands.
- **Double presentation** — wrapping an underlying device that already
  presents typed partitions as units (Emu68's own SD/eMMC devices) would
  show the same partition twice. The config lists underlying units
  explicitly, and the docs say plainly: partition.device is for devices
  that *don't* do this themselves (`scsi.device`, USB mass storage, CF/SD
  readers); on Emu68's own devices, don't list them.
- **Name collision** on "partition.device" — Phase 0 checks; renaming is
  cheap before release and expensive after.

## Success criteria

- `devtest` passes on partition.device units over a Copperline image and on
  at least one real underlying device (a CF/SD card shared with a PC).
- Byte-identical differential against hand-mounted `LowCyl`/`HighCyl`
  entries on the same partitions.
- Out-of-range TD *and* SCSI-direct requests fail without touching the
  neighbouring partition.
- A PC-partitioned card with FAT + two Amiga partitions (one FFS from ROM,
  one PFS3 via handler path) mounts at init with no mount entries written by
  hand; `NOMOUNT` + `EMIT` reproduces the same mounts by hand.
- Media change on a removable underlying unit re-parses correctly with
  stable unit numbers and no-disk reporting.
- A GPT card prepared per Emu68's convention (type
  `3F82EEBC-87C9-4097-8165-89D6540557C0`) presents the same units under
  partition.device on any underlying device as it does under
  `brcm-emmc.device` on a PiStorm.
