# RetComM packaging

Helpers used by [`.github/workflows/release.yml`](../.github/workflows/release.yml).

Release assets (no zips):

| Platform | Artifact |
|---|---|
| Linux | `RetComM-Launcher-<ver>-linux-x86_64.AppImage` |
| macOS | `RetComM-Launcher-<ver>-macos-{arm64,x86_64}.dmg` |
| Windows | `*-windows-x64-setup.exe` + `*-windows-portable.exe` |

## Icons

```sh
./packaging/make-icons.sh
```

Writes `assets/retcomm.png` at **512×512** (linuxdeploy’s max allowed size;
1024 is rejected) and `.ico` when ImageMagick is available, from
`assets/retcomm.svg`.

Hub UI fonts live in `assets/fonts/` (Lato Latin, same face as recomp-ui) and
install to `share/retcomm/fonts`. Platform controller icons live in
`assets/platforms/` (`psx.png`, …) → `share/retcomm/platforms`. Packaging
**requires** both:

| Artifact | Font location | Platform icons |
|---|---|---|
| Linux AppImage | `usr/share/retcomm/fonts` + `usr/bin/fonts` | `usr/share/retcomm/platforms` + `usr/bin/platforms` |
| macOS `.app` / DMG | `Contents/Resources/fonts` | `Contents/Resources/platforms` |
| Windows setup / portable | `fonts/` next to the exes | `platforms/` next to the exes |

CI fails the release job if `LatoLatin-Regular.ttf` is missing from the install
prefix or the final package. Windows/macOS/Linux packagers also require
`platforms/psx.png`.

## Linux

### Local AppImage (one shot)

```sh
./packaging/linux/build-local-appimage.sh
# optional version override:
./packaging/linux/build-local-appimage.sh 0.1.2
```

Builds icons, SDL3 (cached under `.cache/sdl3` if not already installed), RetComM,
and writes `dist/RetComM-Launcher-<ver>-linux-<arch>.AppImage`.

```sh
./dist/RetComM-Launcher-*-linux-*.AppImage
./dist/RetComM-Launcher-*-linux-*.AppImage cli list
# if FUSE is unavailable:
./dist/RetComM-Launcher-*-linux-*.AppImage --appimage-extract-and-run
```

### Manual / CI-style steps

```sh
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/out"
cmake --build build -j && cmake --install build
./packaging/linux/build-appimage.sh "$PWD/out" 0.1.1 x86_64
```

Self-update replaces the running AppImage in place (`APPIMAGE` env). Dev
binaries / loose copies under `~/.local/share/retcomm/bin` are not updatable —
Menu → Update RetComM stays disabled with a hint to launch the AppImage.

When launching a title, RetComM strips AppImage `LD_LIBRARY_PATH` / `APPDIR`
from the child environment so native recomp binaries load their own (or system)
libs instead of the launcher’s bundled SDL.

## macOS

```sh
./packaging/macos/build-app.sh "$PWD/out" 0.1.1 arm64
```

Produces under `dist/`:

| Artifact | Role |
|---|---|
| `RetComM Launcher.app` | App bundle (local staging) |
| `RetComM-Launcher-<ver>-macos-<arch>.dmg` | Drag-to-Applications installer disk image |

Open the DMG and drag **RetComM Launcher** onto **Applications**. That puts it on
Launchpad, Spotlight, and the Applications folder. Self-update downloads the
matching arch DMG and refreshes the running `.app` only (dev `.app`-less builds
cannot self-update).

## Windows

Requires a Release build that includes `retcomm`, `retcomm-hub`, and
`retcomm-portable` (the stub). Optional: [Inno Setup 6](https://jrsoftware.org/isinfo.php)
on `PATH` (or pass `-InnoSetup`) to build the Start Menu installer.

```powershell
./packaging/make-icons.sh   # from Git Bash / WSL, for assets/retcomm.ico
cmake --build build --config Release
cmake --install build --config Release --prefix out
./packaging/windows/package.ps1 -Prefix out -Version 0.1.1 -VcpkgBin path\to\vcpkg\bin -Arch x64
```

Produces under `dist/`:

| Artifact | Role |
|---|---|
| `RetComM-Launcher-<ver>-windows-portable.exe` | Single-file portable (stub + zip trailer; icon embedded) |
| `RetComM-Launcher-<ver>-windows-x64-setup.exe` | Per-user Inno Setup installer (Start Menu / desktop) |

Portable usage:

```text
RetComM-Launcher-*-windows-portable.exe           # hub UI
RetComM-Launcher-*-windows-portable.exe cli list  # CLI
```

Self-update channels (detected via `channel.json` / env from the portable stub):

- **installer** (primary) → downloads `*-windows-*-setup.exe`, silent Inno into the current install dir
- **portable** → downloads `*-windows-portable.exe`, replaces the stub, re-extracts on next launch

Loose/dev copies without `channel.json` cannot self-update.
