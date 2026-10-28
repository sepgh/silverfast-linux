# Adding a scanner

silverfast-linux works with any scanner in its [catalog](../models/); the
Epson Perfection V600 is the reference scanner it is developed and tested on.
Adding another scanner needs a few facts about it — most of them can be collected
automatically. This page explains how to collect them (if you own the scanner)
and how they turn into support (if you maintain the project).

- [For scanner owners](#for-scanner-owners)
  - [0. Is it already in the catalog?](#0-is-it-already-in-the-catalog)
  - [1. Check that SilverFast supports your scanner](#1-check-that-silverfast-supports-your-scanner)
  - [2. Get the Windows driver](#2-get-the-windows-driver)
  - [3. Create a report](#3-create-a-report)
  - [4. Optional: try it with SilverFast](#4-optional-try-it-with-silverfast)
  - [5. Open an issue](#5-open-an-issue)
- [What the report contains](#what-the-report-contains)
- [For maintainers](#for-maintainers)

## For scanner owners

### 0. Is it already in the catalog?

```sh
silverfast-linux scanners
```

lists every scanner silverfast-linux knows and marks the connected ones.
`supported` models are installed by default; an `experimental` one is
installed when it is connected (or with `install --scanner ID`) — try it and
report how it went.

### 1. Check that SilverFast supports your scanner

silverfast-linux only makes SilverFast's Windows build work; it cannot add
scanners SilverFast itself does not support. Look your model up in
LaserSoft's [list of supported scanners](https://www.silverfast.com/supported-scanners-models-devices/).

### 2. Get the Windows driver

Download your scanner's **Windows 10/11 64-bit** driver from the
manufacturer's support site (for Epson: the "Scanner Driver and EPSON Scan
Utility" or "Epson Scan 2" package). Keep the downloaded file or note its
download link; you do not need to run it.

`silverfast-linux drivers` helps: for the connected scanner it shows the
catalog's driver (if any) and searches the
[Microsoft Update Catalog](https://www.catalog.update.microsoft.com) for
Windows Update drivers matching its USB ID. Fetch a listed entry with
`silverfast-linux drivers --download UPDATE_ID`. Microsoft's catalog only has
drivers some manufacturers publish through Windows Update (it has Canon
imaging drivers, but not the V600's), so the manufacturer's site remains the
main source.

The driver tells us how Windows talks to the scanner: whether it uses the
standard Windows USB scanner stack, and whether the scanner needs a protocol
interpreter or firmware from the computer, like the V600 does.

### 3. Create a report

Connect and switch on the scanner, then run:

```sh
silverfast-linux report --driver ~/Downloads/<driver-file>.exe
# or with the download link:
silverfast-linux report --driver https://…/<driver-file>.exe
```

If you have more than one USB device that could be a scanner, the report
lists them; pick yours with `--device VID:PID` (the IDs are in the list).

The result is a Markdown file such as `silverfast-linux-report-20261028-0539.md`
in the current directory. Read it before sharing: it contains no serial
numbers, and your home directory is shortened to `~`.

### 4. Optional: try it with SilverFast

This step gives us the most useful information — how SilverFast reacts to your
scanner — and may already work. It needs a working
`silverfast-linux install`.

```sh
silverfast-linux try-scanner --driver ~/Downloads/<driver-file>.exe
```

`try-scanner`:

1. creates the same report as above and reads the scanner's description from
   the driver;
2. registers the scanner in the Wine prefix and installs the driver's protocol
   interpreter, if it has one (the same two things done for the V600);
3. starts SilverFast with every USB transfer logged — select the scanner,
   press START, try a prescan, then close SilverFast;
4. removes the temporary registration and appends the USB log, the `sti.dll`
   log and the end of SilverFast's own log to the report. E-mail addresses and
   your user name are masked.

For Epson scanners whose driver ships firmware, the helper uploads it the same
way as for the V600. Other brands get no firmware handling; tell us in the
issue if the scanner seems to need it.

> [!NOTE]
> `try-scanner` is experimental. It sends only what SilverFast sends and never
> writes to the scanner's permanent memory. If something goes wrong, switch the
> scanner off and on.

### 5. Open an issue

Open a [**New scanner** issue](https://github.com/sepgh/silverfast-linux/issues/new?template=new-scanner.yml),
fill in the form and attach the report file.

## What the report contains

| Section | Source | Why it is needed |
|---|---|---|
| System | `/etc/os-release`, kernel, glibc, `wine --version`, libusb | Reproducing problems; Wine Staging vs. mainline changes the transport |
| USB devices | sysfs (`/sys/bus/usb/devices`) | Finding the scanner; serial numbers are never read |
| Scanner | sysfs | USB ID, speed, interface class, bulk endpoints and packet size the helper must use, kernel driver in the way, whether your user can open it |
| SANE | `scanimage -L` | Whether a Linux driver exists, which hints at the protocol |
| Windows driver | the driver package | INF entry for this exact USB ID: model name, `Needs = STI.USBSection` (standard USB scanner stack), `DeviceType`/`DeviceSubType`/`Capabilities`, `DeviceData` with `ExtCmdModule` (protocol interpreter) and `ExtBinModule` (firmware), and the files it installs with their CPU architecture |
| doctor | `silverfast-linux doctor` | State of the installation |
| Draft catalog entry | the driver package | The scanner as a `models/*.json` file, with every SHA-256 filled in |
| Try session (try-scanner only) | `usbd.log`, `sti.log`, SilverFast `LogFile.txt` | Which STI calls SilverFast made, every USB transfer, and SilverFast's own error messages |

## For maintainers

### Reading a report

1. **Is the scanner a still-image device?** `Class: Image` and
   `Needs = STI.USBSection` mean Windows pipes raw data to the application — the
   case `sti.dll` + the helper already handle. Anything else (a vendor kernel
   driver, WinUSB/libusb-win32, TWAIN-only) needs investigation first.
2. **Interpreter and firmware.** `ExtCmdModule` and `ExtBinModule` in
   `DeviceData` mean the scanner works like the V600: SilverFast loads the
   interpreter DLL itself, and the firmware has to be uploaded after power-on.
   No `ExtCmdModule` usually means the scanner speaks ESC/I natively.
3. **Endpoints.** Note the bulk OUT/IN addresses and packet size from the
   scanner section.
4. **Try session.** In `sti.log`, `CreateDevice` means SilverFast accepted the
   device. In `usbd.log`, check the firmware lines and look for `failed`.
   SilverFast's log names the failing step (e.g. "Lock failed", "Can't write
   exStatusCommand").

### Adding the model

Scanners are data: one JSON file per model in `models/`
([format](../models/README.md)); the installer, launcher, udev rules and
helper read everything from there.

1. Copy the report's **Draft catalog entry** to `models/<id>.json`.
2. Set `driver.url` to the official download link if the draft lacks it.
3. Leave `status: experimental` and run `make test-report`, which validates
   every model file.
4. Open a pull request. Once merged, owners get the scanner with
   `silverfast-linux install --accept-vendor-licenses` while it is connected.
5. After a reporter confirms the hardware checklist from the
   [v1 design spec](superpowers/specs/2026-10-27-silverfast-linux-v1-design.md#testing)
   (with `SFL_VERBOSE=1` logs), switch `status` to `supported` and fill in
   `tested`.

Scanners that need more than data — a firmware protocol other than Epson's,
or a Windows stack other than `STI.USBSection` — need code; see the
[future plan](plans/future-pure-wine-and-more-scanners.md).
