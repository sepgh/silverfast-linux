# Changelog

## 0.2.0 — 2026-10-28

### Added
- **Scanner catalog** (`models/*.json`): every scanner is a data file with its
  USB ID, driver download, pinned SHA-256s, interpreter, firmware and registry
  values. Format: [models/README.md](models/README.md).
- `silverfast-linux scanners`: list the catalog and connected scanners.
- `silverfast-linux drivers`: driver sources for a scanner — the catalog and
  the Microsoft Update Catalog (searched by USB hardware ID); `--download`
  fetches an entry.
- `silverfast-linux report`: scanner report for adding support (system, USB
  descriptors, SANE, analysis of the vendor's Windows driver) ending with a
  draft catalog entry.
- `silverfast-linux try-scanner` (experimental): try an unsupported scanner in
  SilverFast with full USB logging and get a report with the session logs.
- GitHub issue templates for new scanners and bugs;
  [docs/adding-a-scanner.md](docs/adding-a-scanner.md).
- `install --scanner ID` and `run --scanner ID`.

### Changed
- `install` sets up every supported model plus any connected catalog scanner.
- `silverfast-usbd` takes `--device`, `--name` and `--firmware` and reads
  endpoints from the USB descriptors; it no longer has a built-in model table.
- `udev` generates rules for all catalog scanners (the static rules file is gone).
- `doctor` checks each installed scanner.

### Upgrading
Install the new release, then run `silverfast-linux install
--accept-vendor-licenses` again (cached downloads, about two minutes).

## 0.1.0 — 2026-10-27

First release: SilverFast 9 under Wine with the Epson Perfection V600.
- `sti.dll` replacement for Wine's still-image layer.
- `silverfast-usbd` Linux USB helper with automatic V600 firmware loading,
  reached over a Unix socket (Wine Staging) or token-protected loopback TCP.
- `silverfast-linux install | run | doctor | udev | uninstall`.
