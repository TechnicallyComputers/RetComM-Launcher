# RetComM build packs

Local generate + cmake installs need two managed packs plus game source.

| Pack | Cache | Override |
|---|---|---|
| Toolchain (`cmake-clang-v1`) | `~/.local/share/retcomm/toolchains/<id>/<tag>/` | `RETCOMM_TOOLCHAIN_DIR` |
| SDK tools (`snesrecomp-tools`) | `~/.local/share/retcomm/sdks/<id>/<tag>/` | `RETCOMM_SDK_DIR` |
| Game source | `…/apps/<install>/src/<ref>/` | `RETCOMM_SOURCE_DIR` |

Optional: `RETCOMM_PYTHON` selects the interpreter for `snesrecomp_cli.py`
(default `python3` / `python` on Windows).

## Toolchain pack layout

```
cmake-clang-v1-<os>/
  bin/cmake
  bin/ninja          # optional but preferred
  bin/clang …        # or system compiler via env.sh
  env.sh             # optional: export PATH/CC/CXX
  env.bat            # Windows
  README.md
```

RetComM prepends `<pack>/bin` (or the single nested folder’s `bin/`) to `PATH`
for configure/build. Toolchains are **fetched on demand** from
[TechnicallyComputers/retcomm-toolchains](https://github.com/TechnicallyComputers/retcomm-toolchains)
(not embedded in game zips) and cached for reuse.

| OS asset | Notes |
|----------|--------|
| `cmake-clang-v1-linux-x64.zip` | Pruned LLVM/Clang + lld + cmake + ninja |
| `cmake-clang-v1-windows-x64.zip` | llvm-mingw UCRT + cmake + ninja |
| `cmake-clang-v1-macos-universal.zip` | cmake + ninja; requires Xcode CLT |

## SDK pack layout

```
snesrecomp-tools-<os>/
  snesrecomp_cli.py
  tools/…            # analyzer / emit dependencies
  retcomm-sdk.json   # optional: { "cli": "snesrecomp_cli.py" }
```

Do **not** ship ROM dumps or generated `src/gen` in either pack.

## Packaging scripts

- `scripts/package_snesrecomp_tools.sh` — build a tools zip from a snesrecomp tree
- `scripts/package_toolchain_smoke_linux.sh` — local smoke wrappers only (dev/testing)
- Toolchain release assets: build in **retcomm-toolchains** via its CI

Publish assets with names matching catalog `asset_glob` patterns
(e.g. `snesrecomp-tools-linux-x64.zip`, `cmake-clang-v1-linux-x64.zip`).
