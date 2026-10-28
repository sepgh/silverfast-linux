# Future plan: pure-Wine USB path and more scanners

Status: planned, not started. v1 (helper + `sti.dll`) is described in
[the v1 design](../superpowers/specs/2026-10-27-silverfast-linux-v1-design.md).

## Part A — pure Wine (`usbscan.sys`)

### Why

v1 needs a separate Linux process. A Windows-side USB path would:

- remove the helper and the socket;
- let unmodified Windows scanner software (including Epson's own WIA driver,
  Epson Scan, and other STI/WIA applications) reach scanners;
- be a candidate for inclusion in Wine itself, so every Wine user benefits.

### What Windows does

Epson's INF binds the scanner with `Include=sti.inf, Needs=STI.USBSection`,
i.e. to `usbscan.sys`. Applications and drivers open `\\.\UsbscanN` and use:

- `ReadFile` / `WriteFile` → bulk in/out pipes;
- `DeviceIoControl` with the `IOCTL_*` codes from `usbscan.h`:
  `IOCTL_GET_PIPE_CONFIGURATION` (Epson's driver calls this first and retries),
  `IOCTL_SEND_USB_REQUEST` (control transfers), `IOCTL_RESET_PIPE`,
  `IOCTL_CANCEL_IO`, `IOCTL_GET_VERSION`, `IOCTL_GET_DEVICE_DESCRIPTOR`,
  `IOCTL_WAIT_ON_DEVICE_EVENT`, `IOCTL_GET_CHANNEL_ALIGN_RQST`,
  `IOCTL_SET_TIMEOUT`.

### Work items

1. **`usbscan.sys`** — a PE kernel driver (MinGW, links `ntoskrnl.exe`/`usbd`)
   that attaches to the USB device object created by Wine's `wineusb.sys`,
   selects the configuration, exposes `\Device\UsbscanN` + `\\.\UsbscanN`, and
   turns IRPs into `URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER` /
   `URB_FUNCTION_VENDOR_*` requests. Includes packet-aligned reads (see v1
   `rxbuf.c`).
2. **Binding** — Wine's `pnputil` is a stub and its `setupapi` driver
   selection does not process `STI.USBSection`. Options, in order of
   preference: (a) contribute `STI.USBSection` handling and an `sti.inf` to
   Wine; (b) have our installer write the `Enum\USB\VID_…&PID_…` `Service` and
   `Driver` values plus the `usbscan` service key, then restart `winedevice`.
3. **STI transport** — add `transport_usbscan.c` in `sti/src` that opens the
   device's `CreateFileName` and uses `ReadFile`/`WriteFile`; choose it when
   the file opens, else fall back to the helper.
4. **Firmware** — either keep the upload in `sti.dll` (port `firmware.c`, which
   is plain C) or run Epson's WIA driver's `InitScanner`. The former is far
   smaller.
5. **Wine upstreaming** — real `GetDeviceList`/`CreateDevice` for Wine's
   `sti.dll` based on the v1 implementation, `usbscan.sys`, and the INF
   handling, each as a separate merge request with tests.

### Risks

- Wine's kernel-driver support for third-party USB function drivers is lightly
  used; expect bugs in `wineusb.sys` / `ntoskrnl` URB handling.
- Device binding across Wine versions may be fragile until upstreamed.
- Hot-plug and power management behave differently from Windows.

### Exit criteria

The v1 hardware checklist passes with the helper disabled, on a distribution
Wine build, and Epson Scan 3.9 itself can preview through `usbscan.sys`.

## Part B — more scanners

### What varies per model

| Item | Where it comes from |
|---|---|
| USB VID/PID | Epson INF `[Models.*]` section |
| Display name, `ModelCode` | INF `[Strings]`, `DeviceData` |
| Interpreter DLL (`ExtCmdModule`) | INF `DeviceData`; absent for scanners that speak ESC/I natively |
| Firmware (`ExtBinModule`) | INF `DeviceData`; absent if the scanner has flash firmware |
| Endpoints, packet size | USB descriptors (read at runtime) |
| Driver download URL + hashes | Epson support site |

SilverFast also matches the model through `MatchingDeviceId`, so a scanner
SilverFast supports on Windows should be recognised once registered the same
way.

### Work items

1. **Model data file** — *done:* `models/<id>.json`, read by the installer,
   launcher and udev generation; the helper gets the USB ID and firmware on
   its command line and reads endpoints from the descriptors. Reports emit
   draft entries. Driver lookup (`silverfast-linux drivers`) covers the
   catalog and the Microsoft Update Catalog.
2. **Generic firmware handling** — the upload sequence is shared by Epson's
   firmware-loading models (same WIA driver code); verify on a second model.
3. **Native ESC/I models** (no interpreter, no firmware, e.g. V800/V850, V550)
   — need only the registry entry and the helper; test that SilverFast picks
   its non-interpreter scanner class.
4. **Multiple scanners at once** — one helper per device or device selection in
   the protocol (`HELLO` with VID/PID); `sti.dll` enumerates several class
   keys already.
5. **Other brands** — SilverFast ships its own drivers for some (Plustek and
   Microtek via `libusb0_64.dll`, Nikon, Minolta); those need different
   investigation (libusb-win32 under Wine) and are out of scope until a tester
   with hardware volunteers.

### Collecting data

Implemented: `silverfast-linux report` and `silverfast-linux try-scanner`
(which also emit a draft catalog entry)
(see [adding-a-scanner.md](../adding-a-scanner.md)) produce the model data
above from the Windows driver plus a logged SilverFast session, and the
"New scanner" issue template collects them.

### Testing without hardware

- Extract model data from Epson's INF automatically and check it into tests.
- Record USB traffic (`SFL_VERBOSE=1`) from volunteers and replay it against
  the helper in CI.

### Exit criteria per model

The v1 hardware checklist passes on that model, run by someone who owns it,
and its model file is merged with the log attached to the pull request.
