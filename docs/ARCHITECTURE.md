# Retcomm Launcher architecture

## Two launchers, one job each

| Layer | Repo | Job |
|---|---|---|
| Hub | **retcomm-launcher** (this) | Catalog, install/update, ROM/RomM match, recommend, spawn game |
| Per-game | **recomp-ui** (submodule of each title) | Settings, verify, controllers, netplay, PLAY |

Retcomm never embeds a full `RecompLauncherCGameInfo` session. It installs a
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
~/.local/share/retcomm/apps/<id>/   installed builds
~/.config/retcomm/                  library roots, RomM URL, tokens
```

## Recommendation pipeline

1. Load catalog titles + `rom_identity` (CRC32 / SHA-1 / SHA-256 / disc serial).
2. Resolve RomM/ES-style folders under `library_root` via `platform_folders`
   (e.g. catalog `psx` → disk `ps/`). Never walk the whole tree blindly.
3. Index candidates by extension per platform; hash only when that platform has
   a title with CRC/SHA-1 identity. Skip `exclude_dirs` (`torrents`, …).
4. Join: file hash ∈ title identity → **ready** / **installable**.
5. Surface matches in `scan` / (later) hub UI. Hash is the gate; title fuzzy
   match is a weak secondary signal only.

## Install layout

```
~/.local/share/retcomm/
  apps/<title-id>/
    current -> releases/<version>/     # symlink (or junction on Windows)
    releases/<version>/
      <binary>
      ...
  state.json                           # installed versions, last launch
```

## RomM (optional)

Companion-app style: base URL + API token in config. List platforms/ROMs and
assets (saves/states). Map RomM platforms onto catalog `platform` slugs.
Local-only mode must work with RomM unset.

## Growth path

1. ~~Catalog + scan + CLI stubs~~ (this scaffold)
2. Real GitHub release download + checksum verify + extract
3. Launch with env/`rom.cfg` staging
4. Update check against release tags
5. RomM auth + library overlap
6. Save/state sync into per-title paths from the manifest
7. Dear ImGui hub sharing visual language with recomp-ui
