# Development

## Layout

```
helper/src/        silverfast-usbd (Linux, C, libusb)
  usbd.c             socket server, USB open/claim/reopen
  firmware.c/.h      firmware check and upload
  rxbuf.c/.h         packet-aligned receive buffer
  protocol.h         wire protocol shared with sti.dll
helper/tests/      unit tests with fake devices
sti/src/           sti.dll replacement (Windows, C, MinGW)
  sti.c              IStillImageW / IStiDevice, registry device database
  transport.c/.h     client for silverfast-usbd (Unix socket, else loopback TCP)
  log.c/.h           diagnostics
sti/tests/         sti_selftest.exe, run under Wine
tests/mock_usbd.py mock helper used by the sti.dll self-test
models/            scanner catalog, one JSON file per scanner (format: models/README.md)
scripts/           silverfast-linux (install/run/doctor/...), components list, test runner
  sfl-models.py      catalog tool: validate, registry/udev generation, driver extraction,
                     Microsoft Update Catalog search
  sfl-report.py      scanner report and draft catalog entries from Windows drivers
build-env/         Containerfile with the full toolchain
packaging/         AUR PKGBUILDs and the Flatpak manifest (see packaging/README.md)
```

## Building

Native build needs `gcc`, `make`, `pkg-config`, libusb-1.0 headers and
MinGW-w64:

| Distribution | Packages |
|---|---|
| Fedora | `gcc make pkgconf libusb1-devel mingw64-gcc` |
| Debian / Ubuntu | `gcc make pkg-config libusb-1.0-0-dev gcc-mingw-w64-x86-64` |
| Arch | `gcc make pkgconf libusb mingw-w64-gcc` |

```sh
make            # build/silverfast-usbd and build/sti.dll
make test       # unit tests + sti.dll self-test (needs wine and python3)
```

`make` checks for the libusb headers (via `pkg-config libusb-1.0`) and for
`x86_64-w64-mingw32-gcc` before compiling, and stops with a message naming
the missing piece. Override the compilers with `CC=` and `MINGW_CC=`.

Without installing compilers, build inside a container (podman or docker):

```sh
make container
```

This builds `build-env/Containerfile` (Fedora with gcc, libusb headers and
MinGW) and compiles into the same `build/` directory, including the test
binaries. `make install` and `make test` afterwards reuse those outputs
without needing the toolchain; `make test` still needs `wine` and `python3`
on the host.

## Releasing

1. Set `VERSION=` in `scripts/silverfast-linux`, add the release to
   `CHANGELOG.md`, and update the download links in `README.md`.
2. `make release` builds in an Ubuntu 22.04 container (so the helper needs
   only glibc 2.34+) and runs the unit tests. Artifacts land in
   `build-release/`: `silverfast-linux-<version>-x86_64.tar.gz`, laid out like
   `make install` for extraction into `~/.local`, and `SHA256SUMS`.
3. `scripts/run-sti-selftest.sh build-release` to check the release `sti.dll`.
4. Tag `v<version>` on `main` and attach both files to a GitHub release.

## Tests

- `build/test_firmware`: checksum preparation, the "firmware loaded" status bit,
  the full upload byte sequence including chunking, and NAK handling.
- `build/test_rxbuf`: surplus bytes from the device are kept, multi-packet
  reads, short transfers, error propagation.
- `scripts/run-sti-selftest.sh`: creates a throwaway Wine prefix, registers a
  fake device, starts `tests/mock_usbd.py`, and drives `sti.dll` the way
  SilverFast does (`StiCreateInstance`, `GetDeviceList` with the 0x138 /
  0x00010001 checks, `GetDeviceValue`, `CreateDevice`, lock, raw write/read),
  once over loopback TCP and, on Wine Staging, once over the Unix socket.
- `tests/test_sfl_report.py`, `tests/test_sfl_models.py` (`make test-report`):
  INF parsing, catalog validation, registry and udev generation, model
  selection, Microsoft Update Catalog parsing; also validates every file in
  `models/`.
- Hardware: see the checklist in the
  [v1 design spec](superpowers/specs/2026-10-27-silverfast-linux-v1-design.md#testing).

## Debugging

| What | How |
|---|---|
| Every USB transfer | `SFL_VERBOSE=1 silverfast-linux run`, then read `~/.local/share/silverfast-linux/logs/usbd.log` |
| sti.dll decisions | `~/.local/share/silverfast-linux/logs/sti.log` |
| SilverFast's own log | `<prefix>/drive_c/users/$USER/AppData/Roaming/LaserSoft Imaging/SilverFast 9/LogFile.txt` |
| Wine output | `~/.local/share/silverfast-linux/logs/wine.log`; add channels with e.g. `WINEDEBUG=+loaddll silverfast-linux run` |
| Run the helper by hand | `build/silverfast-usbd --device 04b8:013a --firmware esfwa1.bin --firmware-dir ~/.local/share/silverfast-linux/firmware --verbose` (add `--tcp-port-file FILE` with `SFL_USBD_TOKEN=...` for the TCP endpoint) |
| Inspect the catalog | `scripts/sfl-models.py list`, `validate`, `registry ID 0000`, `udev`, `drivers VID:PID` |

## Adding a scanner

See [adding-a-scanner.md](adding-a-scanner.md) and
[plans/future-pure-wine-and-more-scanners.md](plans/future-pure-wine-and-more-scanners.md).
`scripts/sfl-report.py` (tested by `make test-report`) does the INF parsing
for `report` and `try-scanner`.
