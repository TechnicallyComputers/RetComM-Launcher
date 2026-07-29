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

## Recommendation pipeline

1. Load catalog titles + `rom_identity` (CRC32 / SHA-1 / SHA-256 / disc serial).
2. Resolve RomM/ES-style folders under `library_root` via `platform_folders`
   (e.g. catalog `psx` → disk `ps/`). Never walk the whole tree blindly.
3. Index candidates by extension per platform; hash only when that platform has
   a title with CRC/SHA-1/SHA-256 identity. Skip `exclude_dirs` (`torrents`, …).
   Reuse hashes from `library-index.json` when `path+size+mtime` is unchanged.
   `scan --full` / hub Full Rescan ignores the cache and rebuilds the index.
4. Join: file hash ∈ title identity → **ready** / **installable**. Prefer `.cue`
   / cart dumps over `.iso` for launch staging. Hashed disc dumps also promote
   a companion `.cue` (same stem or `FILE` reference) into the index.
5. Persist matches to the library index; `launch` reads preferred ROM path
   without rescanning (disc titles prefer companion `.cue`).
6. Surface matches in `scan` / `library` / (later) hub UI.

## Install layout

```
~/.local/share/retcomm/
  apps/<install_dir_name>/
    install.json                       # tag, asset, relative binary, timestamps
    current -> releases/<tag>/         # symlink (or junction on Windows)
    releases/<tag>/
      <binary + assets>
      ...
  library-index.json
  state.json                           # reserved (hub-wide prefs / last launch)
```

Install flow: GitHub Releases API → pick host `asset_glob` → download → extract
(`bsdtar` / `unzip` / `7z`) → unwrap nested archives (double-zip common) → locate
catalog `launch` binary → write `install.json` → point `current`. Update checks
compare `install.json` tag to `/releases/latest`.

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

## RomM (optional)

Companion-app style: base URL + API token in config. List platforms/ROMs and
assets (saves/states). Map RomM platforms onto catalog `platform` slugs.
Local-only mode must work with RomM unset.

## Growth path

1. ~~Catalog + scan + CLI stubs~~
2. ~~GitHub release download + nested extract + install.json / update check~~
3. ~~Launch with `rom.cfg`/`disc.cfg`/`settings.toml` staging + process spawn~~
4. Launch mode: netplay via generic lobby frontend
5. Optional asset checksum verify
6. RomM auth + library overlap
7. Save/state sync into per-title paths from the manifest
8. Dear ImGui hub sharing visual language with recomp-ui

### Launch modes

| Mode | CLI | Behavior |
|---|---|---|
| **default** | `--mode default` (default) | Spawn install binary with `--launcher`; stage media + `settings.toml` `[bios]`/`[disc]` for disc titles |
| **direct** | `--mode direct` | `--no-launcher` when the title supports skipping UI |
| **netplay** | `--mode netplay` | Reserved — not implemented yet |

Working directory is the release directory (`current/`). Cart titles get a
positional ROM; disc titles never use a bare positional (psxrecomp treats that
as BIOS) and do not pass `--disc` on current MotK (launcher preselect comes from
staged `settings.toml` / `disc.cfg`).
