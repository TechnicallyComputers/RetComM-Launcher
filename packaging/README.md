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

Self-update replaces the running AppImage in place (`APPIMAGE` env).

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
matching arch DMG and refreshes the installed `.app`.

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

- **installer** → downloads `*-windows-*-setup.exe`, runs silent Inno into the current install dir
- **portable** → downloads `*-windows-portable.exe`, replaces the stub, re-extracts on next launch
