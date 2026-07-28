# Retcomm Launcher

Desktop hub for the Retcomm team's **recomp** and **decomp** titles: browse a
supported catalog, install/update builds, match ROMs from a local library (and
optionally RomM), then launch each game into its own `recomp-ui` session.

This repo is the **multi-title manager**. Per-game settings, ROM verification,
controllers, and PLAY still live in [`recomp-ui`](https://github.com/mstan/recomp-ui)
inside each game binary.

```
Retcomm Launcher          →  install / update / recommend / launch
        │
        ▼
each recomp/decomp exe    →  recomp-ui (settings + PLAY)
```

## Status (MVP scaffold)

| Piece | State |
|---|---|
| Title catalog (JSON manifests) | Working |
| CLI: `list` / `scan` / `status` / `config` | Working |
| RomM-style library scan (platform folders) | Working |
| Local ROM scan + CRC/SHA-1 match | Working |
| Install from GitHub release | Stub (paths + plan printed) |
| Launch installed title | Stub (resolves binary path) |
| Update check | Stub |
| RomM API client | Stub (auth + list hooks) |
| Hub UI (ImGui) | Not started |

## Build

```sh
cmake -G Ninja -S . -B build
cmake --build build -j
./build/retcomm --help
```

Requires CMake 3.24+, a C++17 compiler. No SDL/OpenGL yet (CLI only).

## Quick start

```sh
# List supported titles shipped in catalog/
./build/retcomm list

# Configure a RomM/ES-style library (see config.example.json)
mkdir -p ~/.config/retcomm
cp config.example.json ~/.config/retcomm/config.json
# edit library_root / platform_folders as needed

./build/retcomm config
./build/retcomm scan
# or one-shot without config:
./build/retcomm scan --rom-dir /mnt/crucial4tb/Emulation/roms

# Where Retcomm will store installs + config
./build/retcomm status

# Dry-run install / launch (stubs until release fetching lands)
./build/retcomm install metal-warriors-snes
./build/retcomm launch metal-warriors-snes
```

Scan only walks platform folders the catalog needs (`snes/`, `n64/`, `ps/`, …),
skips `torrents/` / `emulators/`, and hashes only when a title has CRC/SHA-1
identity — so a full library root will not CRC every PS2/PS3 ISO.

## Layout

```
catalog/                shipped title manifests + index
docs/                   architecture + catalog schema
src/                    CLI + core libraries
include/retcomm/        public headers
third_party/            nlohmann/json (single header)
```

## Data dirs

| Role | Linux |
|---|---|
| Config | `~/.config/retcomm/` |
| Installs + state | `~/.local/share/retcomm/` |
| Catalog override | `$RETCOMM_CATALOG` or `--catalog DIR` |

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and
[`docs/CATALOG.md`](docs/CATALOG.md).
