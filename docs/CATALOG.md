# Catalog schema

Retcomm ships a directory of JSON manifests. `index.json` lists title ids;
each `titles/<id>.json` describes one supported recomp/decomp.

## `index.json`

```json
{
  "schema_version": 1,
  "name": "Retcomm supported titles",
  "titles": ["metal-warriors-snes", "..."]
}
```

## `titles/<id>.json`

| Field | Type | Notes |
|---|---|---|
| `id` | string | Stable slug; matches filename |
| `name` | string | Display name |
| `kind` | `"recomp"` \| `"decomp"` | |
| `platform` | string | `snes`, `psx`, `n64`, `gba`, … (RomM + folder map) |
| `description` | string | Optional short blurb |
| `homepage` | string | Optional URL |
| `rom_identity` | object | How we know the user owns the game |
| `rom_identity.crc32` | string[] | Hex, e.g. `"f2ab92d4"` |
| `rom_identity.sha1` | string[] | 40-char lowercase hex |
| `rom_identity.sha256` | string[] | 64-char lowercase hex |
| `rom_identity.disc_serials` | string[] | PSX/etc, e.g. `"SCUS-94423"` |
| `rom_extensions` | string[] | Scan filter, e.g. `[".sfc",".smc"]` |
| `release` | object | Where to fetch builds |
| `release.github` | string | `owner/repo` |
| `release.asset_glob` | object | Per-OS glob: `linux`, `windows`, `macos` |
| `install_dir_name` | string | Folder under `apps/` |
| `launch` | object | Relative binary names: `linux`, `windows`, `macos` |
| `romm` | object | Optional match hints |
| `romm.platforms` | string[] | RomM platform slugs |
| `romm.igdb_ids` | number[] | Optional |
| `saves` | object | Optional paths relative to install for sync later |

Identity should mirror what each game passes into `recomp-ui`
(`known_sha1_hex` / `expected_crc` / disc verify) so Retcomm and the game agree
on “verified.”

## Adding a title

1. Create `catalog/titles/<id>.json`.
2. Append `"<id>"` to `catalog/index.json` → `titles`.
3. Fill `rom_identity` from the game's launcher gate.
4. Point `release.github` at the shipping repo once releases exist.
