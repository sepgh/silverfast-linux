# Flatpak

App ID: `io.github.sepgh.silverfast_linux`

## How it is built

- **Base:** Flathub's Wine (`org.winehq.Wine`, branch `stable-26.08`, Wine 11)
  on the freedesktop 26.08 runtime. It brings Wine and 7-Zip; the runtime
  brings Python, curl and libusb.
- **This project** is built from source inside the Flatpak build: the helper
  with the SDK's gcc, `sti.dll` with the `org.freedesktop.Sdk.Extension.mingw-w64`
  extension (the same one Flathub uses to build Wine).
- **Entry point:** `silverfast-linux-flatpak`. Started from the menu it guides
  the first setup with dialogs (licence acceptance, a progress notice, USB
  permission help), then runs `silverfast-linux run`. With arguments it is the
  normal command line:

  ```sh
  flatpak run io.github.sepgh.silverfast_linux doctor
  flatpak run io.github.sepgh.silverfast_linux scanners
  ```

- **Data** lives in `~/.var/app/io.github.sepgh.silverfast_linux/`.

## Permissions

| Permission | Why |
|---|---|
| `--device=all` | USB access to the scanner |
| `--share=network` | Downloading SilverFast and the scanner driver from their vendors |
| `--socket=x11`, `--socket=wayland`, `--share=ipc` | SilverFast's window |
| `--filesystem=xdg-pictures`, `xdg-documents`, `xdg-download` | Saving scans |

A Flatpak cannot install the udev rule that lets your user open the scanner.
On first start, if the scanner is connected but not accessible, the app shows
the two `sudo` commands to run once.

Flathub's Wine is regular (non-Staging) Wine without `AF_UNIX`, so `sti.dll`
reaches the helper over the token-protected loopback connection.

## Build and install locally

```sh
flatpak install --user flathub org.flatpak.Builder
flatpak run org.flatpak.Builder --user --install --force-clean \
    --install-deps-from=flathub build-dir \
    packaging/flatpak/io.github.sepgh.silverfast_linux.yml
flatpak run io.github.sepgh.silverfast_linux
```

The first build downloads the runtime, SDK, MinGW extension and Wine base
(roughly 2 GB).

## Toward Flathub

- Switch the module source from the local directory to the release tag
  (`type: git`, `tag: v<version>`).
- Flathub reviews apps that download proprietary software at runtime case by
  case; the licence dialog and the README make clear nothing proprietary is
  bundled.
- The icon is a neutral placeholder (a generic flatbed scanner) and can be
  replaced, but must not use LaserSoft's or any scanner maker's logo.
