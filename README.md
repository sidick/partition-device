# partunit.device

An AmigaOS block device that presents MBR and GPT partitions of other block
devices as units of itself — so an Amiga filesystem inside a PC-partitioned
card, stick or CF becomes an ordinary device unit, with I/O bounded to the
partition.

Status: **design only, no code yet.** Phase 0 (prior-art reading and convention
confirmation) is complete.

- [`docs/proposal.md`](docs/proposal.md) — the design, revised against Phase 0
- [`docs/phase0-notes.md`](docs/phase0-notes.md) — what the prior art actually
  does, with citations: `devtest`, `lide.device`, WinUAE, Amiberry, Emu68's
  `brcm-sdhc`/`brcm-emmc`, AROS `partition.library`, and the Aminet name check

Named `partunit.device` because partitions become units. The working name was
`partition.device`, dropped to avoid implying kinship with `partition.library`
(AROS) and `partition.resource` (Pulchart), which are unrelated projects by
other authors.
