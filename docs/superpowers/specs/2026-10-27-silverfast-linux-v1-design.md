# silverfast-linux v1 — design

Date: 2026-10-27 · Status: approved, implemented on branch `v1-wine-sti-bridge`

## Goal

Let people run **SilverFast 9** (Windows build, any edition their MySilverFast
account unlocks — tested with SE) on Linux through Wine and scan with an
**Epson Perfection V600**, without a Windows VM.

Success means: install with one command, launch from the desktop, log in,
prescan and scan at full resolution with no USB errors.

## Constraints

- The project is public: no LaserSoft or Epson file may be committed or
  released. Vendor files are downloaded from the vendors and verified.
- Must work with distribution Wine ≥ 9; no custom Wine build.
- v1 supports one scanner (V600, USB `04b8:013a`). Code must not hard-wire it
  beyond a model table, so more models can follow.

## Non-goals for v1

- Other scanners or other Epson models.
- A pure-Wine USB path (`usbscan.sys`) — see
  [the future plan](../../plans/future-pure-wine-and-more-scanners.md).
- Scanner button events, WIA, TWAIN.
- Packaging for distributions (Flatpak, AUR, …).

## Findings this design rests on

Established during the feasibility spike (details in
[how-it-works.md](../../how-it-works.md)):

1. SilverFast 9 itself runs under Wine 11, including MySilverFast login.
2. Scanner discovery: `IStillImage::GetDeviceList` plus registry values
   `DeviceID` and `MatchingDeviceId` under the Image device class.
3. Opening: the undecorated `StiCreateInstance` export, `dwSize == 0x138`,
   `DeviceType == MAKELONG(subtype, type)`.
4. All scanner I/O is `IStiDevice::RawWriteData` / `RawReadData`; Epson's
   interpreter `esxuina1.dll` is driven by SilverFast through callbacks.
5. The V600 needs a firmware upload into RAM after power-on, normally done by
   Epson's WIA driver; the sequence is known.
6. The scanner can return more bytes than requested; reads must be
   packet-aligned.
7. LaserSoft's installer hangs under Wine; its repository can be consumed
   directly and is checksummed.

## Architecture

```
SilverFast 9.exe ─ sfjpeg.dll ─┬─ esxuina1.dll (Epson interpreter)
                               └─ sti.dll  (ours) ── Unix socket ── silverfast-usbd (ours) ── libusb ── V600
```

### Components

**`sti.dll`** (C, MinGW, installed into the prefix with a `native` override)
- `IStillImageW`: device list built from
  `HKLM\SYSTEM\CurrentControlSet\Control\Class\{6BDD1FC6-…}\*` subkeys with
  `SubClass=StillImage`; `GetDeviceInfo`, `CreateDevice`, `Get/SetDeviceValue`
  (device key, then its `DeviceData`); other methods are harmless stubs.
- `IStiDevice`: `LockDevice`/`UnLockDevice` with a named mutex per device;
  raw read/write (data and command variants) forwarded to the transport;
  `GetStatus` reports online if the helper answers.
- Exports `StiCreateInstance`, `StiCreateInstanceW`, `StiCreateInstanceA`
  (unsupported), and the COM `Dll*` entry points.
- `transport.c`: one connection to the helper, guarded by a critical section.
  Tries the Unix socket (`SFL_USBD_SOCKET`, else
  `$XDG_RUNTIME_DIR/silverfast-linux/usbd.sock`) and falls back to loopback TCP
  (`SFL_USBD_TCP` port), because `AF_UNIX` is only available in Wine Staging.
  Sends the session token from `SFL_USBD_TOKEN` in `HELLO`.
- Logs to `SFL_STI_LOG` if set.

**`silverfast-usbd`** (C, libusb-1.0)
- Opens the first device from the model table, claims interface 0, runs the
  firmware check and upload.
- Serves one client at a time; protocol in `helper/src/protocol.h`.
- Packet-aligned receive buffer; 30 s I/O timeout.
- On `ENODEV` closes and reopens the device (re-uploading firmware).
- Unix socket in a `0700` directory plus a `127.0.0.1` TCP listener on a
  kernel-chosen port (`--tcp-port-file`); every connection must start with
  `HELLO` carrying the token from `$SFL_USBD_TOKEN`, otherwise it is closed
  with `EACCES`. Refuses to start if another instance answers.

**`silverfast-linux`** (bash)
- `install --accept-vendor-licenses`: dedicated prefix in
  `~/.local/share/silverfast-linux/prefix`; SilverFast components from
  `scripts/components.txt` with SHA-1 verification; VC++ runtime; Epson driver
  (pinned SHA-256) → `esxuina1.dll` into `system32`, `esfwa1.bin` into the
  firmware directory (both pinned); `sti.dll` + override; registry
  (STI device, LaserSoft keys); desktop entry. Downloads are cached in
  `~/.cache/silverfast-linux`.
- `run`: warns about a missing/inaccessible scanner, creates a random session
  token, starts the helper unless one is already serving, exports the socket,
  port and token to Wine, launches SilverFast, waits for Wine to exit, stops the
  helper.
- `doctor`, `udev`, `uninstall [--purge]`, `version`.

**udev rule**: `TAG+="uaccess"` for `04b8:013a`; installed by the user with
the two commands `silverfast-linux udev` prints.

### Data flow of one scan

1. `run` starts `silverfast-usbd`; it opens the V600 and uploads firmware if
   bit `0x10` of the `1b 03` status reply is clear.
2. SilverFast lists the V600 from `sti.dll`; the user presses START.
3. SilverFast creates and locks the STI device, loads `esxuina1.dll`, and the
   interpreter's reads/writes travel `sti.dll → socket → helper → USB`.

### Error handling

| Situation | Behaviour |
|---|---|
| Helper not running | `sti.dll` returns `ERROR_DEVICE_NOT_CONNECTED`; SilverFast shows its "problem communicating" dialog. `run` always starts the helper, so this indicates a crash; see `usbd.log`. |
| Scanner unplugged / not powered | Helper replies `-ENODEV`; it retries opening on every request, so switching the scanner on is enough. |
| Scanner resets mid-session | Helper reopens and re-uploads firmware; the current scan fails, the next one works. |
| No USB permission | `run` and `doctor` point to `silverfast-linux udev`. |
| Vendor file changed upstream | Checksum mismatch aborts the install with the URL. SilverFast updates are picked up automatically because their checksums come from LaserSoft's repository; Epson file hashes are pinned and must be updated in the script. |

## Testing

- Unit (`make test`): firmware preparation/upload sequence, receive buffer.
- Integration: `sti_selftest.exe` under a throwaway Wine prefix against
  `tests/mock_usbd.py`, over TCP always and over the Unix socket when the Wine
  build supports `AF_UNIX`.
- CI: build both binaries, run unit tests and the Wine self-test on every push.
- Hardware checklist (manual, V600):
  1. `silverfast-linux doctor` all green.
  2. Power-cycle the scanner, `run`: `usbd.log` shows "uploading firmware … done".
  3. START → scanner window opens; prescan completes.
  4. Full scan at ≥ 1600 dpi, 48 bit: completes with no `failed` lines in `usbd.log`.
  5. Unplug during idle, replug, prescan again: works without restarting.

## Risks

- **Vendor changes.** A SilverFast update could change its checks; Epson could
  move the driver URL. Both surface as clear install/run errors.
- **Licensing.** Users must accept LaserSoft's and Epson's terms; the installer
  requires an explicit flag and links to the vendors.
- **Wine differences.** `AF_UNIX` exists only in Wine Staging (found by CI on
  Ubuntu's Wine 9.0); the TCP fallback covers mainline Wine. USB itself never
  goes through Wine.
