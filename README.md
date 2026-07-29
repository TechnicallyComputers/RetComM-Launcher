# RetComM Launcher

Desktop hub for the RetComM team's **recomp** and **decomp** titles: browse a
supported catalog, install/update builds, match ROMs from a local library (and
optionally RomM), then launch each game into its own `recomp-ui` session.

This repo is the **multi-title manager**. Per-game settings, ROM verification,
controllers, and PLAY still live in [`recomp-ui`](https://github.com/mstan/recomp-ui)
inside each game binary.

```
RetComM Launcher          →  install / update / recommend / launch
        │
        ▼
each recomp/decomp exe    →  recomp-ui (settings + PLAY)
```

## Status (MVP scaffold)

| Piece | State |
|---|---|
| Title catalog (JSON manifests) | Working (remote cache + bundled fallback) |
| CLI: `list` / `scan` / `status` / `config` | Working |
| RomM-style library scan (platform folders) | Working |
| Local ROM scan + CRC/SHA-1 match | Working |
| Library index (incremental + launch paths) | Working |
| BIOS scan + index (title bindings) | Working |
| Install from GitHub release | Working (OS asset + nested zip unwrap) |
| Launch installed title | Working (`--launcher` / `--bios`; disc via settings.toml) |
| Update check | Working (`update`, `library --check-updates`) |
| RomM API client | Working (boxart search + ROM/BIOS identity match + download) |
| Hub UI (ImGui) | Working scaffold (`retcomm-hub`) |

## Build

```sh
cmake -G Ninja -S . -B build
cmake --build build -j
./build/retcomm --help
./build/retcomm-hub   # SDL3 + ImGui dashboard
```

Requires CMake 3.24+, a C++17 compiler, and **libcurl**. Archive tools used at
runtime: `bsdtar` (preferred), `unzip`, or `7z`. Optional `GITHUB_TOKEN` /
`GH_TOKEN` raises GitHub API rate limits. Hub UI needs system **SDL3** + OpenGL
and Dear ImGui (sibling `../recomp-ui` checkout, or CMake FetchContent). Wine
installs need `wine` or `wine64` on `PATH`.

### Release packages (CI)

Releases are **manual only** (Actions → **Release** → Run workflow). The workflow
does not run on push. It builds all platforms, creates tag `v<version>` if needed,
and publishes a GitHub Release with the zips/AppImage.

- **Version** input empty → uses `RETCOMM_VERSION` from `CMakeLists.txt`
- Optional **prerelease** checkbox for pre-release tags

| Artifact | Platform |
|---|---|
| `RetComM-Launcher-<ver>-linux-x86_64.AppImage` | Linux |
| `RetComM-Launcher-<ver>-linux-x86_64.zip` | Linux (self-update) |
| `RetComM-Launcher-<ver>-windows-x64-setup.exe` | Windows installer (Start Menu / desktop; self-updates via zip) |
| `RetComM-Launcher-<ver>-windows-portable.exe` | Windows single-file portable (self-updates in place) |
| `RetComM-Launcher-<ver>-windows-x64.zip` | Windows folder / installer update payload |
| `RetComM-Launcher-<ver>-macos-arm64.dmg` | macOS Apple Silicon (drag into Applications) |
| `RetComM-Launcher-<ver>-macos-x86_64.dmg` | macOS Intel (drag into Applications) |
| `RetComM-Launcher-<ver>-macos-arm64.zip` | macOS Apple Silicon (`.app` zip / self-update) |
| `RetComM-Launcher-<ver>-macos-x86_64.zip` | macOS Intel (`.app` zip / self-update) |

Windows portable launches the hub with no args; use
`RetComM-Launcher-*-windows-portable.exe cli <command>` for the CLI (same idea as the
Linux AppImage `cli` dispatch). Installer and portable each self-update from their
matching release asset via **Update RetComM** in the hub.

Packaging scripts live under `packaging/`; the generic icon is `assets/retcomm.svg`
(rasterized by `packaging/make-icons.sh`).

## Quick start

```sh
# List supported titles (from on-device catalog cache)
./build/retcomm list

# Refresh title list from retcomm-catalog (fetched on first run; auto-update on by default)
./build/retcomm catalog update
# ./build/retcomm catalog update --force

# Configure a RomM/ES-style library (see config.example.json)
mkdir -p ~/.config/retcomm
cp config.example.json ~/.config/retcomm/config.json
# edit library_root / bios_root / saves_root / platform_folders as needed

./build/retcomm config
./build/retcomm scan
./build/retcomm bios scan
# Rebuild indexes from disk (ignore hash cache; drop missing files):
./build/retcomm scan --full
./build/retcomm bios scan --full
# or one-shot without config:
./build/retcomm scan --rom-dir /mnt/crucial4tb/Emulation/roms
./build/retcomm bios scan --bios-dir /mnt/crucial4tb/Emulation/bios

# Indexed title → ROM / BIOS bindings
./build/retcomm library
./build/retcomm bios list

# Where RetComM will store installs + config
./build/retcomm status

# Install / update a title (host-OS asset from GitHub Releases)
./build/retcomm install masters-of-teras-kasi-psx --dry-run
./build/retcomm install masters-of-teras-kasi-psx
# Linux/macOS fallback: Windows build launched through Wine
# ./build/retcomm install some-title --wine
./build/retcomm update masters-of-teras-kasi-psx
./build/retcomm library --check-updates

# Launch into the title's dedicated recomp-ui (ROM/disc from library index)
./build/retcomm launch masters-of-teras-kasi-psx --dry-run
./build/retcomm launch masters-of-teras-kasi-psx
# ./build/retcomm launch masters-of-teras-kasi-psx --detach

# Remove an install (keeps memcards/SRAM/savestates by default)
./build/retcomm uninstall masters-of-teras-kasi-psx --dry-run
./build/retcomm uninstall masters-of-teras-kasi-psx
# ./build/retcomm uninstall masters-of-teras-kasi-psx --delete-saves
```

`launch` stages `disc.cfg` / `rom.cfg` (and for disc titles, `settings.toml`
`[bios]` / `[disc]`) next to the install, passes `--launcher` and `--bios`, and
prefers a companion `.cue` over raw `.bin`/`.iso`/`.img`. Current MotK builds
preselect the disc from `settings.toml` / `disc.cfg` — RetComM does not pass
`--disc` (that flag skips settings disc and does not seed the launcher). Future
`--mode netplay` will hook a generic lobby frontend.

Installs land in `~/.local/share/retcomm/apps/<install_dir_name>/` with
`releases/<tag>/`, a `current` symlink, and `install.json` for version tracking.
Nested release zips (outer wrapper + inner build) are unwrapped automatically.

Scan only walks platform folders the catalog needs (`snes/`, `n64/`, `ps/`, …),
skips `torrents/` / `emulators/`, and hashes only when a title has CRC/SHA-1/SHA-256
identity — so a full library root will not CRC every PS2/PS3 ISO. Results are
saved to `~/.local/share/retcomm/library-index.json`; a second scan reuses
unchanged hashes (`cache-hits`). Use `scan --full` (or hub **Full Rescan ROMs**)
to ignore the cache, rehash everything, drop vanished files, and rebuild matches.

## Layout

```
docs/                   architecture + catalog schema notes
src/                    CLI + core libraries
include/retcomm/        public headers
third_party/            nlohmann/json + stb
assets/                 app icon (SVG / PNG)
packaging/              AppImage, macOS .app/DMG, Windows zip helpers
.github/workflows/      Release CI
```

## Catalog

Supported titles are maintained in
[`retcomm-catalog`](https://github.com/TechnicallyComputers/retcomm-catalog)
— not in this repo. The launcher downloads `catalog.zip` from the latest
**dated** GitHub release into `~/.local/share/retcomm/catalog/` and keeps that
cache on-device. First run (or an empty cache) always fetches; afterward,
`auto_update` checks the latest release tag/date on startup and downloads only
when it changed (stored in `catalog-state.json`).

```sh
./build/retcomm catalog update
./build/retcomm catalog update --force   # always re-download
```

In **retcomm-hub**, use **Refresh catalog**. Optional `config.json` block:

```json
"catalog": {
  "url": "https://github.com/TechnicallyComputers/retcomm-catalog/releases/latest/download/catalog.zip",
  "github_repo": "TechnicallyComputers/retcomm-catalog",
  "auto_update": true
}
```

Overrides: `--catalog DIR`, `$RETCOMM_CATALOG`. Resolution order: override →
env → on-device cache.

## Data dirs

| Role | Linux |
|---|---|
| Config | `~/.config/retcomm/` |
| Installs + state | `~/.local/share/retcomm/` |
| Catalog cache | `~/.local/share/retcomm/catalog/` |
| Catalog override | `$RETCOMM_CATALOG` or `--catalog DIR` |

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and
[`docs/CATALOG.md`](docs/CATALOG.md).

## License

RetComM Launcher is released under the [MIT License](LICENSE).
