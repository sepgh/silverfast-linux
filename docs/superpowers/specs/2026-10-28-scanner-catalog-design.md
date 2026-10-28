# Scanner catalog and driver lookup — design

Date: 2026-10-28 · Status: implemented on branch `scanner-report` · Builds on the
[v1 design](2026-10-27-silverfast-linux-v1-design.md)

## Goal

Make adding a scanner a data change instead of a code change, and help users
find the Windows driver a scanner needs. This is step 1 of "more scanners" in
the [future plan](../../plans/future-pure-wine-and-more-scanners.md).

## Decisions

**One JSON file per scanner in `models/`.** JSON because the bash launcher,
the Python tools and reviewers all read it easily; one file per model keeps
pull requests small and conflict-free. The format
([models/README.md](../../../models/README.md)) holds exactly what Windows
takes from the driver INF plus where to download the driver and pinned
SHA-256s for the package and every file used from it.

**The helper has no model table.** `silverfast-usbd` is told `--device`,
`--name` and `--firmware` by the launcher and reads bulk endpoints and packet
size from the USB descriptors, so the C code never changes for a new model.
The only scanner-specific code left is the firmware upload protocol
(`firmware.protocol: epson`).

**Catalog entries come from reports.** `report`/`try-scanner` locate the INF
section for the scanner's USB ID and emit a draft entry with all hashes. On
the V600 driver the draft equals the hand-made entry, which is the check that
the mapping is complete.

**`status: supported | experimental`.** Supported models are installed by
default; experimental ones only when connected or named, so the default
install never pulls drivers for hardware the user does not have.

**Driver lookup = catalog + Microsoft Update Catalog.** No vendor offers a
driver API, and per-vendor scrapers would be brittle. Microsoft's catalog is
searchable by hardware ID (`USB\VID_…&PID_…`) and serves signed driver
cabinets; coverage is partial (Canon imaging drivers yes, the V600 no), so it
is offered as a lookup, never used automatically. It has no API; the tool
issues the same search and download-dialog requests a browser does and
tolerates failures.

## Components

| Piece | Change |
|---|---|
| `models/*.json` | New; V600 entry moved out of the script. |
| `scripts/sfl-models.py` | New: list, validate, select, field, registry (.reg), udev rules, verified extraction from driver packages, Microsoft Update Catalog search/download links. |
| `scripts/sfl-report.py` | Emits draft catalog entries; shares package extraction with `sfl-models.py`; `--list-candidates`. |
| `scripts/silverfast-linux` | `install [--scanner ID]` installs several models into registry slots `0000…`, recorded in `scanners.txt`; `run [--scanner ID]` picks the connected one; new `scanners` and `drivers`; `doctor` checks per installed model; `udev` generates rules from the catalog; `try-scanner` uses the catalog format. |
| `silverfast-usbd` | `--device` (required), `--name`, `--firmware`; endpoint detection; model table removed. |
| `udev/` | Removed; rules are generated. |

## Limits

- One scanner is served per SilverFast session (the helper drives one USB
  device); with several installed, `run` picks the connected one or
  `--scanner ID`.
- Only Epson's firmware upload is implemented; other firmware-loading scanners
  need code.
- Microsoft Update Catalog page changes can break the lookup; it fails soft.

## Testing

- `tests/test_sfl_models.py`: validation (including error detection),
  registry values SilverFast depends on, udev rules, default selection,
  Microsoft catalog parsing (offline fixtures). `make test-report` also runs
  `sfl-models.py validate` on every model file.
- Manual, V600: fresh catalog-driven install; reinstall over it; `doctor`;
  `run` with endpoint detection; `try-scanner` with the V600's driver URL
  (registration, logs, draft entry, cleanup); `drivers` against the live
  Microsoft catalog.
