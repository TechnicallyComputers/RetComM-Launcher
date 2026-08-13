# RetComM build packs

Local generate + cmake installs need a toolchain pack plus game source. SDK
tools may be a separate pack **or** embedded in the game release zip.

| Pack | Cache | Override |
|---|---|---|
| Toolchain (`cmake-clang-v1`) | `~/.local/share/retcomm/toolchains/<id>/<tag>/` (Windows: `%LOCALAPPDATA%\retcomm\toolchains\…`) — shared with standalone setup wizards | `RETCOMM_TOOLCHAIN_DIR` |
| SDK tools (`snesrecomp-tools` / `psxrecomp-tools`) | `~/.local/share/retcomm/sdks/<id>/<tag>/` | `RETCOMM_SDK_DIR` |
| Engine source (`psxrecomp` / `recomp-ui` / …) | `~/.local/share/retcomm/engines/<name>/<pin>/` — keyed by `framework_pins.txt` SHA (else content fingerprint) | `RETCOMM_ENGINES_DIR` |
| Game source | `…/apps/<install>/src/current/` (stable; tag in `.retcomm-source.json`) | `RETCOMM_SOURCE_DIR` |

Game source prefers the host **release zip** (`release.asset_glob`) when that
archive vendors a cmake-buildable tree (e.g. BPE `bpe-*.zip` ships `psxrecomp/` +
`recomp-ui/` + emitters at the release pins). Otherwise RetComM falls back to the
GitHub source zipball at `build.source.ref` (note: zipballs omit git submodules).

**Shared engines:** after extract/sync, RetComM harvests each vendored framework
tree into `engines/<name>/<pin>/` (content-aware) and replaces the per-title
copy with a symlink (Unix) or directory junction (Windows; junction first so
Update works without Developer Mode). Titles that pin the
same `psxrecomp` / `recomp-ui` commit therefore share one source tree on disk.
BIOS generated C under `psxrecomp/generated/` lives in that shared pin (OpenBIOS
/ SCPH1001 are pin-identical). Content-sync of game updates never writes through
those links — engines are re-harvested from the staging zip instead. Developer
overrides via `RETCOMM_SOURCE_DIR` skip promotion so local checkouts stay intact.

**One-zip titles (BPE):** the same `bpe-*.zip` is used for install and update.
RetComM harvests CLI + `psxrecomp-game` / `psxrecomp-bios` into the shared SDK
cache, prunes those binaries from the per-title source tree, rebuilds, and
restores saves + user config (`settings.toml`, etc.) across updates. Harvest
requires **both** emitters (no separate tools zip). The setup host inside `src/`
is never treated as a finished Play install — only `releases/` (or `current/`)
counts.

**Incremental cmake on update:** source always lives at `src/current/`. A newer
release zip is **content-synced** into that tree (overwrite only when bytes
change, delete paths removed upstream, leave identical files’ mtimes alone) so
Ninja only recompiles real diffs. Sync never touches `src/current/build/` **or**
local-only trees omitted from setup zips: codegen (`generated/`,
`psxrecomp/generated/`, `src/gen/`, `variants/*/generated`,
`gbarecomp/.../generated_bios`, `.retcomm-codegen.json`) and disc work dirs
(`bpe/`, `motk/`, `disc/`). Shared engine trees (`psxrecomp/`, `recomp-ui/`, …)
are also skipped during sync — RetComM re-harvests them from the staging zip
into `engines/<name>/<pin>/` and relinks. Deleting codegen used to force a full
codegen-cache restore + Ninja rebuild of every shard. Legacy `src/<tag>/` trees
are migrated to `current` once, then pruned. Disk cost: the cmake tree stays
after install (often hundreds of MiB) unless Library Settings → **Advanced** →
**Auto-clean cmake build directories after install** is enabled
(`auto_clean_build_dirs` in `config.json`). Hub **Generate & Rebuild**
(`force_generate`) wipes `build/` and regenerates C from the disc.

**Codegen reuse on update:** after the first successful generate, RetComM keeps
`apps/<title>/codegen-cache/` keyed by ROM/BIOS + **content hashes** of the
emitters (not mtimes — harvest remtimes alone must not force disc→C). Updates
that only change runtime/UI sources skip regenerate when `src/current/generated`
is still present (preferred) or restore from that cache with a **content-aware**
copy (identical shards keep mtimes). SDK harvest into
`sdks/psxrecomp-tools/<tag>/` is also content-aware. Pass `force_generate`
(Hub **Generate & Rebuild**) to rebuild C from the disc again.

**CMake configure reuse:** after a successful `cmake -S/-B`, RetComM writes
`src/current/<build>/.retcomm-cmake-configure.json` capturing the conf argv,
toolchain tag/path, and a size+mtime manifest of every `CMakeLists.txt` /
`*.cmake` under the source tree (excluding `build/` and generated/). Host/UI
updates that do not touch those inputs skip configure and go straight to
`cmake --build`. Changing cmake files, the toolchain, `-DPSX_NETPLAY`, or using
**Generate & Rebuild** (wipes `build/`) forces a fresh configure.

**Setup-host vs raw zip:** catalog titles with a local generate+cmake recipe
(`build.enabled` + source/toolchain) always Install/Update via generate+cmake
(`install_title_auto` / `update_title_auto`). The GitHub release zip is *source*
even when it also ships a launch binary — never take that binary as a Play
shortcut (that used to skip disc→C for dual-mode packs). Blind zip extract would
also replace a built Play binary with the setup-host wizard. Pure prebuilt titles
(no local build recipe) still Update via zip extract; `InstallPrebuilt` / Wine
still force zip. Version *checks* use `github.com` `/releases/latest` redirects
(no `api.github.com` quota). Asset downloads still use the GitHub release API /
`releases/download` URLs; when the API is rate-limited (HTTP 403), Update uses
the same durable release-zip cache as prefetch (`data_dir/cache/releases/…`)
plus the hub tag hint — it does **not** fall back to an incomplete `zipball@main`.
Set `github_token` in Library Settings / `config.json` (or `GITHUB_TOKEN`) if you
need a higher API quota for Install / Apply Update downloads.

**Shared caches & GC:** after a successful local build (and via Hub → Advanced →
**Prune shared caches now** / `retcomm cache gc`), RetComM drops old
`toolchains/<id>/` and `sdks/<id>/` versions beyond the keep counts (default 2 /
3; always keeps whatever `latest` points at), unreferenced
`engines/<name>/<pin>/` trees (keeps one newest orphan per name by default),
stale `cache/releases/<repo>/<tag>/` folders, and idle `src/current/build/`
trees older than `idle_build_keep_days` (default 14; skipped when
`auto_clean_build_dirs` is on). Shared **ccache** lives at
`data_dir/ccache` with `CCACHE_MAXSIZE` from `ccache_max_gb` (default 5).

Optional: `RETCOMM_PYTHON` selects the interpreter for SDK CLIs
(default `python3` / `python` on Windows). `RETCOMM_ENGINES_DIR` overrides the
shared engine cache root (default `…/retcomm/engines`).

## Toolchain pack layout

```
cmake-clang-v1-<os>/
  bin/cmake
  bin/ninja          # optional but preferred
  bin/ccache         # optional; RetComM enables CMAKE_*_COMPILER_LAUNCHER when present
  bin/clang …        # or system compiler via env.sh
  env.sh             # optional: export PATH/CC/CXX
  env.bat            # Windows
  README.md
```

RetComM prepends `<pack>/bin` (or the single nested folder’s `bin/`) to `PATH`
for configure/build (Windows: `CreateProcess` + env block — not `cmd /C` quote
soup). It sets `RETCOMM_TOOLCHAIN_DIR`, and when present `ZLIB_ROOT` /
`SDL3_DIR` pointing at pack **`deps/`** (1.0.9+) or legacy pack-root paths —
**not** pack-root `CMAKE_PREFIX_PATH` (llvm-mingw’s top-level `include/`
poisons libc++). Ambient pack prefixes from older `env.bat` are stripped for
cmake children. On Windows it forces `-G Ninja` when
`ninja.exe` is present (wiping a stale NMake/`Visual Studio` `CMakeCache.txt`
if needed) and passes `-DCMAKE_C_COMPILER` / `-DCMAKE_CXX_COMPILER` to the
pack’s `clang` / `clang++`. It also wipes `build/` when the cache still locks
those compilers to a missing path or a different pack tree (e.g. a broken
`toolchains/cmake-clang-v1/latest` junction after a toolchain bump — cmake
ignores a new `-DCMAKE_C_COMPILER` over a locked entry). Prefer **downloading**
`cmake-clang-v1` from
[TechnicallyComputers/retcomm-toolchains](https://github.com/TechnicallyComputers/retcomm-toolchains)
into the shared cache (`RETCOMM_TOOLCHAIN_DIR` overrides when the directory
exists; a missing/stale override — e.g. broken `latest/` junction — falls
through to cache/GitHub). Hub “Update toolchain” uses `force` so an outdated
override cannot block a newer release. Optionally **harvest** a legacy game-zip
`toolchain/` when download is unavailable, then prune the per-title copy.

| OS asset | Notes |
|----------|--------|
| `cmake-clang-v1-linux-x64.zip` | Pruned LLVM/Clang + lld + cmake + ninja + ccache |
| `cmake-clang-v1-windows-x64.zip` | llvm-mingw UCRT + cmake + ninja + ccache |
| `cmake-clang-v1-macos-universal.zip` | cmake + ninja + ccache; requires Xcode CLT |

## SDK pack layout

```
snesrecomp-tools-<os>/
  snesrecomp_cli.py
  tools/…            # analyzer / emit dependencies
  retcomm-sdk.json   # optional: { "cli": "snesrecomp_cli.py" }

gbarecomp-tools-<os>/
  gbarecomp_cli.py
  gba_recompile       # or gba_recompile.exe
  tools/sdk_*.py
  bios/gba_bios.toml  # identity seeds only — no BIOS dump
  retcomm-sdk.json    # { "cli": "gbarecomp_cli.py", "id": "gbarecomp-tools" }
```

Do **not** ship ROM dumps, BIOS dumps, or generated `src/gen` /
`variants/*/generated` in either pack.

## Packaging scripts

- `scripts/package_snesrecomp_tools.sh` — build a tools zip from a snesrecomp tree
- `gbarecomp/scripts/package_gbarecomp_tools.sh` — GBA tools zip (in gbarecomp)
- `scripts/package_toolchain_smoke_linux.sh` — local smoke wrappers only (dev/testing)
- Toolchain release assets: build in **retcomm-toolchains** via its CI

Publish assets with names matching catalog `asset_glob` patterns
(e.g. `snesrecomp-tools-linux-x64.zip`, `gbarecomp-tools-linux-x64.zip`,
`cmake-clang-v1-linux-x64.zip`).
