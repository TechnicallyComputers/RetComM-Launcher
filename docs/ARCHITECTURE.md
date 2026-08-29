# RetComM Launcher architecture

## Two launchers, one job each

| Layer | Repo | Job |
|---|---|---|
| Hub | **retcomm-launcher** (this) | Catalog, install/update, ROM/RomM match, recommend, spawn game |
| Per-game | **recomp-ui** (submodule of each title) | Settings, verify, controllers, netplay, PLAY |

RetComM never embeds a full `RecompLauncherCGameInfo` session. It installs a
build, optionally stages a ROM path the game already understands (`rom.cfg` /
argv), then `exec`s the title. The game opens its own `recomp-ui`.

## Layers

```
CLI / (future) ImGui hub
        │
        ▼
┌───────────────┐  ┌────────────┐  ┌──────────┐  ┌─────────┐
│ Catalog       │  │ ROM scan   │  │ Install  │  │ RomM    │
│ (JSON files)  │  │ + hash     │  │ + launch │  │ client  │
└───────────────┘  └────────────┘  └──────────┘  └─────────┘
        │
        ▼
~/.local/share/retcomm/apps/<id>/          installed builds
~/.local/share/retcomm/library-index.json  ROM paths + hashes + title binds
~/.local/share/retcomm/bios-index.json     BIOS paths + hashes + title binds
~/.config/retcomm/                         library/bios roots, RomM URL, tokens
```

### Custom data root (portable / second drive)

Those two OS locations are the default, not a constant. A **root** override
replaces both with `<root>/config` and `<root>/data`; every path above keeps its
relative shape, so nothing else in the codebase needs to know which layout is in
play. Resolution happens once per process in `default_paths()`
(`src/paths/paths.cpp`), first hit wins:

| Source | Where |
|---|---|
| `--root DIR` | command line (CLI only, never persisted) |
| `$RETCOMM_HOME` | environment — the Windows portable stub sets this |
| `<exe_dir>/retcomm-root.json` | marker beside the binary; travels with a portable build |
| `<os_config>/retcomm/root.json` | pointer written by Advanced Setup / Library Settings |
| *(none)* | the OS defaults above |

A marker may store a path relative to itself (`{"root": "RetComM-Data"}`) so a
USB stick works whatever drive letter it gets. Set it in Advanced Setup step 1,
in Library Settings → **RetComM data folder → Change…**, or with
`retcomm root set <dir>` / `retcomm root reset`. Changing it moves the tree,
repairs the absolute-target toolchain/engine links inside it, rebases
`install_roots` in config.json, and restarts. See `src/paths/data_root.cpp` and
`src/paths/data_root_migrate.cpp`.

With a custom root RetComM does not write the user's shell rc or HKCU `Path`
(`publish_toolchain_user_env` skips it) — a portable install leaves no trace
outside its own folder.

## Recommendation pipeline

1. Load catalog titles + `rom_identity` (CRC32 / SHA-1 / SHA-256 / disc serial).
2. Resolve RomM/ES-style folders under `library_root` via `platform_folders`
   (e.g. catalog `psx` → disk `ps/`). Never walk the whole tree blindly.
3. Index candidates by extension per platform; hash only when that platform has
   a title with CRC/SHA-1/SHA-256 identity. Skip `exclude_dirs` (`torrents`, …).
   Reuse hashes from `library-index.json` when `path+size+mtime` is unchanged.
   `scan --full` / hub Full Rescan ignores the cache and rebuilds the index.
4. Join: file hash ∈ title identity → **ready** / **installable**. Prefer `.cue`
   / cart dumps for launch staging. Hashed `.bin` dumps promote a companion
   `.cue` (same stem or `FILE` reference). `.iso`/`.chd` are rejected for PSX
   (cannot reliably expand to multi-track Redump).
5. Persist matches to the library index; `launch` reads preferred ROM path
   without rescanning (disc titles prefer companion `.cue`).
6. Surface matches in `scan` / `library` / (later) hub UI.

## Install layout

```
~/.local/share/retcomm/
  apps/<install_dir_name>/
    install.json                       # method zip|build, tag, binary, pack pins
    current -> releases/<tag>/         # symlink (or current.path / junction on Windows)
    releases/<tag>/
      <binary + assets>
    src/current/                       # game source when method=build
      psxrecomp -> ../../engines/…     # shared engine link (after promote)
      recomp-ui -> ../../engines/…
  toolchains/<id>/<tag>/               # managed C/cmake packs
  sdks/<id>/<tag>/                     # snesrecomp / psxrecomp tools packs
  engines/<name>/<pin>/                # shared psxrecomp / recomp-ui source (by pin)
  library-index.json
  state.json                           # reserved (hub-wide prefs / last launch)
```

### Prebuilt zip (`method: zip`)

GitHub Releases API → pick host `asset_glob` → download → extract
(`bsdtar` / `unzip` / `7z`) → unwrap nested archives → locate catalog `launch`
binary → write `install.json` → point `current`. Used for third-party titles
without a `build` recipe, or when the user chooses **Install prebuilt** /
`retcomm install --prebuilt`.

### Local generate + build (`method: build`)

When catalog `build.enabled` is set, Hub **Build & Install** / `retcomm install`
(and `retcomm build`) is primary:

1. Require a library ROM matching `rom_identity`.
2. Ensure toolchain + SDK packs (or `RETCOMM_TOOLCHAIN_DIR` /
   `RETCOMM_SDK_DIR` overrides). Toolchains come from
   `TechnicallyComputers/retcomm-toolchains` (`cmake-clang-v1`).
3. Fetch game source zipball at `build.source.ref` (or `RETCOMM_SOURCE_DIR`).
4. Run SDK CLI `generate --json-progress` (`snesrecomp_cli.py`,
   `psxrecomp_cli.py`, or `gbarecomp_cli.py` per `build.generate.engine` /
   platform).
5. `cmake -S … -B …` then `cmake --build --target …`.
6. Stage binary + `assets/` into `releases/build-<ref>/` and link `current`.

Smoke-test pack download: `retcomm pack ensure toolchain` (or
`retcomm pack ensure toolchain --title metal-warriors-snes`).

Launch always uses `current/` — same `launch_title` path for zip and build.

Uninstall (`uninstall` / `remove`) deletes `releases/`, `current`, and
`install.json`. By default it stashes memcards / SRAM / savestates (catalog
`saves.*_glob` plus `saves/`, `states/`, `savestates/`) under
`apps/<install_dir>/preserved/` and restores them into the next install. Pass
`--delete-saves` for a full wipe.

## BIOS scan

Config `bios_root` points at a RomM/ES-DE BIOS tree (flat files plus per-system
folders such as `ps/`, `psx/`, `gba/`). `retcomm bios scan` only considers
sizes/filenames from catalog `bios_identity` (including
`index.json` → `platform_defaults`), skips emulator junk dirs
(`PPSSPP`, `Mupen64plus`, …), and binds preferred BIOS dumps to titles.
`launch` passes `--bios` and stages `bios.cfg` when a binding exists.

## Game saves

Config `saves_root` (optional, prompted in first-time setup / Library Settings)
is the **canonical** native-save library. Each title is quarantined under
`saves_root/<platform folder>/<title_id>/` (e.g. `…/saves/ps/masters-of-teras-kasi-psx/`).
Legacy flat `saves_root/<platform>/` files that match a title’s preferred or
title-named stem are migrated into that folder on promote / ensure / sync /
Play (not on hub refresh listing). Shared preferred cards are copied, not moved.
First-time setup also accepts optional RomM `base_url` + Client API token
(same fields as RomM Sync Settings).

The hub wizard runs when `retcomm-setup.done` is missing **and**
`library_root` is unset. Markers live under the data dir (and best-effort next
to the binary) so app self-update does not re-prompt when config is already
valid; a missing marker with `library_root` set quietly rewrites the marker.
Continue / Skip also writes the marker. Empty required settings still open the
wizard (pre-filled from any partial `config.json`).

Flow:

1. **Promote** — install `current/saves/` and `preserved/` native files are
   copied into the title’s library folder (generic `save.*` / `card1.*`
   renamed to the ROM stem). Matching leftovers in a legacy flat
   `saves_root/<platform>/` pool are moved into the title folder. Bridge
   symlinks are skipped.
2. **Mint / Create Save** — if none exist, Play or **Create Save** creates an
   empty `<rom-stem>.srm` / `.mcd` in the title library and sets
   `preferred_save`.
3. **Bind** — launch points cart `--save-path` / PSX `[memcard]` at library
   files and bridges `apps/<title>/current/saves/save.*` for fixed-cwd hosts.
4. **RomM sync** — after promote, bidirectional newer-wins only against that
   title’s library folder (not the install tree).

When `saves_root` is unset, behavior stays install-local `…/current/saves/`.
Savestate sync remains install-local either way.

## RomM (optional)

Companion-app style: base URL + API token in config. List platforms/ROMs and
assets (saves/states). Map RomM platforms onto catalog `platform` slugs.
Local-only mode must work with RomM unset.

**Filter Unsupported Titles** (`filter_unsupported_titles` in config) hides
catalog rows with neither a local ROM nor a cached RomM identity match.
**Scan RomM library** writes `romm-rom-index.json` so remote-only titles can
show an **ON ROMM** chip and remain visible when the filter is on.

## Growth path

1. ~~Catalog + scan + CLI stubs~~
2. ~~GitHub release download + nested extract + install.json / update check~~
3. ~~Launch with `rom.cfg`/`disc.cfg`/`settings.toml` staging + process spawn~~
4. Launch mode: netplay via generic lobby frontend (catalog `netplay` + hub
   `NetplayLobbyState`; WS client / spawn handoff still TODO)
5. Optional asset checksum verify
6. RomM auth + library overlap
7. Save/state sync into per-title paths from the manifest
8. Dear ImGui hub sharing visual language with recomp-ui

### Launch modes

| Mode | CLI | Behavior |
|---|---|---|
| **default** | `--mode default` (default) | Spawn install binary with `--launcher`; stage media + `settings.toml` `[bios]`/`[disc]` for disc titles |
| **direct** | `--mode direct` | `--no-launcher` when the title supports skipping UI |
| **netplay** | `--mode netplay` | Reserved — hub lobby model exists; spawn handoff not wired yet |

### Netplay data model

Titles that ship recomp-net declare a catalog `netplay` block (`game_name` +
`game_version` wire identity). Hub `TitleRow` mirrors that for library badges;
`HubModel::netplay` (`NetplayLobbyState`) holds the cross-game room browser /
active lobby. User defaults live in `config.json` → `netplay.lobby_url` /
`display_name` / `prefer_ice`.

Working directory is the release directory (`current/`). Cart titles stage an
install-local `library.<ext>` sidecar (symlink → hardlink → copy) so `rom.cfg`
works without Windows Developer Mode; disc titles never use a bare positional
(psxrecomp treats that as BIOS) and do not pass `--disc` on current MotK
(launcher preselect comes from staged `settings.toml` / `disc.cfg`). Play spawn
on Windows uses UTF-8 argv → `CreateProcessW` UTF-16.
