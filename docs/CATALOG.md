# Catalog schema

RetComM ships a directory of JSON manifests. `index.json` lists title ids;
each `titles/<id>.json` describes one supported recomp/decomp.

## `index.json`

```json
{
  "schema_version": 1,
  "name": "RetComM supported titles",
  "platform_defaults": {
    "gba": { "bios_identity": { "required": true, "crc32": ["81977335"], "…": "…" } },
    "psx": { "bios_identity": { "required": true, "crc32": ["37157331"], "…": "…" } }
  },
  "titles": ["metal-warriors-snes", "..."]
}
```

| Field | Type | Notes |
|---|---|---|
| `platform_defaults` | object | Optional per-platform defaults keyed by catalog `platform` |
| `platform_defaults.<platform>.bios_identity` | object | Applied to titles on that platform that omit `bios_identity` |

Title manifests may still set `bios_identity` to override the default, or
`"bios_identity": null` to opt out of inheritance.

## `titles/<id>.json`

| Field | Type | Notes |
|---|---|---|
| `id` | string | Stable slug; matches filename |
| `name` | string | Display name |
| `kind` | `"recomp"` \| `"decomp"` | |
| `platform` | string | `snes`, `psx`, `n64`, `gba`, … (RomM + folder map) |
| `description` | string | Optional short blurb |
| `homepage` | string | Optional URL (hub “GitHub Source”; defaults to `https://github.com/<release.github>`) |
| `release.github` owner | — | Hub shows as Recomp/Decomp Author (owner segment of `owner/repo`) |
| `rom_identity` | object | How we know the user owns the game (always include every digest field) |
| `rom_identity.crc32` | string[] | Hex, e.g. `"f2ab92d4"` (empty `[]` if unused) |
| `rom_identity.md5` | string[] | 32-char lowercase hex (common in recomp README tables) |
| `rom_identity.sha1` | string[] | 40-char lowercase hex |
| `rom_identity.sha256` | string[] | 64-char lowercase hex |
| `rom_identity.disc_serials` | string[] | PSX/etc, e.g. `"SLUS-00562"` |
| `rom_identity.sizes` | number[] | Optional byte lengths; when set, scan only hashes files of those sizes (disc dumps) |
| `rom_identity.filenames` | string[] | Suggested basenames for the hub when unmatched (No-Intro / Redump); search hints, not hard matching |
| `rom_extensions` | string[] | Scan filter, e.g. `[".sfc",".smc"]` |
| `bios_identity` | object | Optional host BIOS / firmware the title needs |
| `bios_identity.required` | bool | Default `true` when object present |
| `bios_identity.crc32` / `md5` / `sha1` / `sha256` | string[] | Preferred dump checksums (include all keys; unused = `[]`) |
| `bios_identity.sizes` | number[] | Byte lengths to consider while scanning |
| `bios_identity.filenames` | string[] | Basename hints (e.g. `SCPH1001.BIN`) |
| `release` | object | Where to fetch builds |
| `release.github` | string | `owner/repo` |
| `release.allow_prerelease` | bool | Allow GitHub pre-releases when no stable latest exists |
| `release.asset_glob` | object | Per-OS glob: `linux`, `windows`, `macos` |
| `install_dir_name` | string | Folder under `apps/` |
| `launch` | object | Relative binary names: `linux`, `windows`, `macos` |
| `romm` | object | Optional match hints |
| `romm.platforms` | string[] | RomM platform slugs |
| `romm.igdb_ids` | number[] | Optional |
| `saves` | object | Optional paths relative to install for sync later |

A title is considered to have a ROM identity when **any** of `crc32`, `md5`,
`sha1`, `sha256`, or `disc_serials` is non-empty. Matching succeeds if **any**
configured digest matches the scanned file (authors may publish only the
algorithm their gate uses).

Identity should mirror what each game passes into `recomp-ui`
(`known_sha1_hex` / `expected_crc` / MD5 tables / disc verify) so RetComM and
the game agree on “verified.”

### Submission-ready `rom_identity` template

Always ship the full field set so future author self-submission can fill any
subset without schema churn:

```json
"rom_identity": {
  "crc32": [],
  "md5": [],
  "sha1": [],
  "sha256": [],
  "disc_serials": [],
  "sizes": [],
  "filenames": ["Game Name (USA).z64"]
}
```

## Adding a title

1. Create `catalog/titles/<id>.json`.
2. Append `"<id>"` to `catalog/index.json` → `titles`.
3. Fill `rom_identity` from the game's launcher gate / README baserom table
   (prefer publishing every digest you know; leave unused keys as `[]`).
4. Point `release.github` at the shipping repo once releases exist.
5. For GBA/PSX, BIOS identity is inherited from `platform_defaults` unless
   the title sets its own `bios_identity` (or `null` to opt out).
