# RetComM packaging

Helpers used by [`.github/workflows/release.yml`](../.github/workflows/release.yml).

Release assets:

| Platform | Artifact |
|---|---|
| Linux | `RetComM-Launcher-linux-x86_64.AppImage` |
| macOS | `RetComM-Launcher-macos-{arm64,x86_64}.dmg` |
| Windows | `RetComM-Launcher-windows-x64-setup.exe` + `RetComM-Launcher-portable-windows.zip` |

Filenames are stable across releases (version lives in the GitHub tag / binary
`RETCOMM_VERSION`). The portable zip contains `RetComM Launcher.exe` (friendly
desktop name). Self-update replaces AppImage / portable in place and keeps the
user's path, so a versioned download name would go stale.

## Icons

```sh
./packaging/make-icons.sh
```

Writes `assets/retcomm.png` at **512×512** (linuxdeploy’s max allowed size;
1024 is rejected) and `.ico` when ImageMagick is available, from
`assets/retcomm.svg`.

Hub UI fonts live in `assets/fonts/` (Lato Latin, same face as recomp-ui) and
install to `share/retcomm/fonts`. Platform controller icons live in
`assets/platforms/` (`psx.png`, …) → `share/retcomm/platforms`. PSX DualShock
pad art for the Gamepads configure mapper lives in `assets/controllers/`
(`pad_analog.png`, `pad_digital.png`) → `share/retcomm/controllers`. Packaging
**requires** all three:

| Artifact | Font location | Platform icons | PSX pad art |
|---|---|---|---|
| Linux AppImage | `usr/share/retcomm/fonts` + `usr/bin/fonts` | `usr/share/retcomm/platforms` + `usr/bin/platforms` | `usr/share/retcomm/controllers` + `usr/bin/controllers` |
| macOS `.app` / DMG | `Contents/Resources/fonts` | `Contents/Resources/platforms` | `Contents/Resources/controllers` |
| Windows setup / portable | `fonts/` next to the exes | `platforms/` next to the exes | `controllers/` next to the exes |

On Windows, `retcomm-hub` links `/SUBSYSTEM:WINDOWS` (no console window);
`retcomm.exe` remains a console CLI.

CI fails the release job if `LatoLatin-Regular.ttf` is missing from the install
prefix or the final package. Windows/macOS/Linux packagers also require
`platforms/psx.png` and `controllers/pad_analog.png`.

## Linux

### Local AppImage (one shot)

```sh
./packaging/linux/build-local-appimage.sh
# optional version override:
./packaging/linux/build-local-appimage.sh 0.1.2
```

Builds icons, SDL3 (cached under `.cache/sdl3` if not already installed), RetComM,
and writes `dist/RetComM-Launcher-linux-<arch>.AppImage`.

```sh
./dist/RetComM-Launcher-linux-*.AppImage
./dist/RetComM-Launcher-linux-*.AppImage cli list
# if FUSE is unavailable:
./dist/RetComM-Launcher-linux-*.AppImage --appimage-extract-and-run
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
| `RetComM-Launcher-macos-<arch>.dmg` | Drag-to-Applications installer disk image |

Open the DMG and drag **RetComM Launcher** onto **Applications**. That puts it on
Launchpad, Spotlight, and the Applications folder. Self-update downloads the
matching arch DMG and refreshes the running `.app` only (dev `.app`-less builds
cannot self-update).

`build-app.sh` calls `bundle_dylibs.sh` to copy Homebrew dylibs (SDL3, FreeType,
libpng, …) into `Contents/Frameworks` and rewrite nested install names to
`@rpath` / `@loader_path`. The packager **fails** if any `/usr/local` or
`/opt/homebrew` load command remains. Ad-hoc `codesign` is applied so arm64
accepts the rewritten binaries; downloaded DMGs still need a Developer ID +
notarization to skip Gatekeeper.

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
| `RetComM Launcher.exe` | Single-file portable stub + payload (local; not the GitHub asset) |
| `RetComM-Launcher-portable-windows.zip` | Release zip; root entry is `RetComM Launcher.exe` (no nested folder) |
| `RetComM-Launcher-windows-x64-setup.exe` | Per-user Inno Setup installer (Start Menu / desktop) |

Portable usage (after unzipping the release zip):

```text
RetComM Launcher.exe           # hub UI
RetComM Launcher.exe cli list  # CLI
```

Self-update channels (detected via `channel.json` / env from the portable stub):

- **installer** (primary) → downloads `RetComM-Launcher-windows-*-setup.exe`, silent Inno into the current install dir
- **portable** → downloads `RetComM-Launcher-portable-windows.zip`, extracts the stub, replaces the desktop exe, re-extracts on next launch

Apply scripts/logs (Windows PowerShell; Unicode-safe path args):
`%LOCALAPPDATA%\retcomm\self-update\bin\apply_setup_update.ps1` /
`apply_setup_update.log`, or `apply_portable_update.ps1` /
`apply_portable_update.log`. The `.ps1` is written for diagnostics only —
apply runs via `powershell -EncodedCommand` (not `-File`) so Group Policy
Restricted ("running scripts is disabled on this system") cannot block it.
Spawn handshake diagnostics: `apply_spawn.log` plus a `.ready` marker. The hub
prefers a script-written marker, then falls back to writing the marker itself
after ~1.5s if the child is still alive (avoids deadlock with a leftover
pre-Ready apply script that waits on the hub PID). Installer apply aborts if
the hub PID is still alive after 120s (avoids Inno fighting locked files).

Loose/dev copies without `channel.json` cannot self-update.
