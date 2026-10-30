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
| `--socket=x11`, `--share=ipc` | SilverFast's window (Wine draws through X11) |
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

## Publishing on Flathub

`flathub/io.github.sepgh.silverfast_linux.yml` is the manifest for Flathub:
the same build, but from the signed release tag instead of a local folder.

### Before submitting (open items)

1. **App ID.** Flathub derives the project URL from the ID and uses its last
   component as-is (dashes are allowed there), so the ID has to be
   `io.github.sepgh.silverfast-linux` to match the repository. Flathub's
   linter currently fails with `appid-url-not-reachable` for the underscore
   form. Renaming means renaming the manifest, desktop file, metainfo and icon.
2. **Name.** Flathub requires app names and icons that do not use another
   company's trademark ("a WhatsApp client cannot have WhatsApp in its name").
   "SilverFast for Linux" will most likely be refused; pick a distinct name
   (the description may still say it runs SilverFast).
3. **Screenshots.** Add at least one screenshot to the metainfo
   (`<screenshots>` with an image URL in this repository), without personal
   data such as the MySilverFast e-mail.
4. **Runtime downloads.** Flathub's rules cover build-time sources; an app
   that downloads proprietary software at runtime after explicit consent is
   decided by the reviewers. Say so in the submission and point to the
   licence dialog.

### Steps

1. Check locally:

   ```sh
   flatpak run --command=flatpak-builder-lint org.flatpak.Builder manifest packaging/flatpak/flathub/<app-id>.yml
   flatpak run --command=flatpak-builder-lint org.flatpak.Builder appstream packaging/flatpak/<app-id>.metainfo.xml
   ```

2. Fork <https://github.com/flathub/flathub>, create a branch from its
   `new-pr` branch, add the manifest at the repository root, and open a pull
   request against `new-pr` titled "Add <app-id>".
3. Flathub's bot builds it (comment `bot, build` to rebuild); reviewers
   comment on the PR. Once accepted, Flathub creates `flathub/<app-id>`, a
   repository you maintain: new releases are pull requests there that bump
   `tag` and `commit` (the `x-checker-data` entry lets Flathub's bot propose
   them automatically).
4. The Flathub page then offers the app to everyone; the README's Flatpak
   option can point to it.

The icon is a neutral placeholder (a generic flatbed scanner) and can be
replaced, but must not use LaserSoft's or any scanner maker's logo.
