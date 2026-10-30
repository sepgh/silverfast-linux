# Packaging

| Format | Directory | Status |
|---|---|---|
| Release tarball | `make release` (see [docs/development.md](../docs/development.md#releasing)) | Published with every GitHub release |
| AUR (Arch Linux) | [`aur/`](aur/) | `silverfast-linux` (builds from source) and `silverfast-linux-bin` (release tarball) |
| Flatpak | [`flatpak/`](flatpak/) | Builds on Flathub's Wine; not yet submitted to Flathub |

Every format installs the same thing: the `silverfast-linux` command, the
`silverfast-usbd` helper, `sti.dll` and the scanner catalog. SilverFast and
the scanner drivers are always downloaded from their vendors on the user's
machine, never packaged.

## AUR

`aur/silverfast-linux` builds from the tagged source (needs `mingw-w64-gcc`
for `sti.dll`); `aur/silverfast-linux-bin` repackages the release tarball.
Both install to `/usr` and conflict with each other.

Publishing (needs an AUR account with an SSH key):

```sh
git clone ssh://aur@aur.archlinux.org/silverfast-linux.git aur-silverfast-linux
cp packaging/aur/silverfast-linux/{PKGBUILD,.SRCINFO} aur-silverfast-linux/
cd aur-silverfast-linux && git add PKGBUILD .SRCINFO && git commit -m "Update to <version>" && git push
```

(the same for `silverfast-linux-bin`).

On each release: set `pkgver`, reset `pkgrel=1`, update `sha256sums`
(source: the GitHub tag archive; bin: the release tarball's entry in
`SHA256SUMS`), and regenerate `.SRCINFO` with `makepkg --printsrcinfo > .SRCINFO`.

Users then install with an AUR helper, e.g. `yay -S silverfast-linux`, and
continue with `silverfast-linux install --accept-vendor-licenses`.

## Flatpak

See [flatpak/README.md](flatpak/README.md).
