# Scanner catalog

Each file here describes one scanner that silverfast-linux can set up. The
installer, the launcher and the USB helper take everything model-specific
from these files; there is no per-model code.

Run `silverfast-linux scanners` to see the catalog and which scanners are
connected, and `scripts/sfl-models.py validate` to check the files.

## Status

| Status | Meaning | Installed by default |
|---|---|---|
| `supported` | Tested end to end (prescan and full scan) by someone with the scanner | yes |
| `experimental` | Generated from the scanner's Windows driver, not yet confirmed | only when connected, or with `install --scanner ID` |

## Format

File name: `<id>.json`. Example: [epson-perfection-v600.json](epson-perfection-v600.json).

| Key | Required | Meaning |
|---|---|---|
| `id` | yes | Lower-case words joined by `-`; also the file name. |
| `name` | yes | Shown in SilverFast's scanner list (as the device's friendly name) and in logs. |
| `vendor` | yes | Manufacturer, as in the driver's INF `Provider`. |
| `usb` | yes | USB `vid:pid` in lower-case hex. |
| `status` | yes | `supported` or `experimental`. |
| `tested` | no | Free text: who tested what, on which system. |
| `driver.url` | if files are needed | HTTPS download of the vendor's Windows driver package. |
| `driver.sha256` | with `url` | SHA-256 of the package; the install stops if it changes. |
| `driver.info` | no | Version/description of the package. |
| `driver.files` | with `url` | File name → SHA-256 for every file taken from the package. |
| `interpreter` | no | DLL from `driver.files` installed into `system32`; it is the driver's `ExtCmdModule`, which SilverFast loads for scanners that need a host-side protocol interpreter. |
| `firmware.file` | no | File from `driver.files` uploaded to the scanner after power-on (`ExtBinModule`). |
| `firmware.protocol` | with `file` | Upload procedure; only `epson` is implemented (see docs/how-it-works.md). |
| `registry.inf`, `registry.install_section` | no | Informational, copied from the driver's INF. |
| `registry.device_type`, `registry.device_subtype`, `registry.capabilities` | yes | From the INF install section (`DeviceType`, `DeviceSubType`, `Capabilities`). |
| `registry.device_data` | yes | The INF `DeviceData` section; written to the device's `DeviceData` registry key. |

`registry` values become the Windows registry entry that the STI class
installer would create on Windows (`DeviceID`, `MatchingDeviceId` and the
fixed values are derived automatically); see `registry()` in
`scripts/sfl-models.py`.

## Adding a model

You rarely write these by hand: `silverfast-linux report --driver …` and
`silverfast-linux try-scanner --driver …` end with a **draft catalog entry**
built from the driver's INF, with every hash filled in. To add it:

1. Save the draft as `models/<id>.json`.
2. Set `driver.url` to the official download link (the draft only has it when
   the report was made from a URL).
3. Keep `status: experimental` until someone confirms a prescan and a full
   scan; then set `supported` and fill in `tested`.
4. Run `make test-report` (validates every file) and open a pull request.

See [docs/adding-a-scanner.md](../docs/adding-a-scanner.md) for the whole
process.
