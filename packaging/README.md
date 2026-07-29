# RetComM packaging

Helpers used by [`.github/workflows/release.yml`](../.github/workflows/release.yml).

## Icons

```sh
./packaging/make-icons.sh
```

Writes `assets/retcomm.png` at **512×512** (linuxdeploy’s max allowed size;
1024 is rejected) and `.ico` when ImageMagick is available, from
`assets/retcomm.svg`.

## Linux

### Local AppImage (one shot)

```sh
./packaging/linux/build-local-appimage.sh
# optional version override:
./packaging/linux/build-local-appimage.sh 0.1.2
```

Builds icons, SDL3 (cached under `.cache/sdl3` if not already installed), RetComM,
and writes `dist/RetComM-Launcher-<ver>-linux-<arch>.AppImage`.

```sh
./dist/RetComM-Launcher-*-linux-*.AppImage
./dist/RetComM-Launcher-*-linux-*.AppImage cli list
# if FUSE is unavailable:
./dist/RetComM-Launcher-*-linux-*.AppImage --appimage-extract-and-run
```

### Manual / CI-style steps

```sh
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/out"
cmake --build build -j && cmake --install build
./packaging/linux/build-appimage.sh "$PWD/out" 0.1.1 x86_64
./packaging/linux/package-zip.sh "$PWD/out" 0.1.1 x86_64
```

- **AppImage** — end-user Linux package (hub via AppRun; `AppRun cli …` for CLI)
- **zip** — flat `retcomm` + `retcomm-hub` for in-app self-update (catalog is
  downloaded on-device into `~/.local/share/retcomm/catalog`)

## macOS

```sh
./packaging/macos/build-app.sh "$PWD/out" 0.1.1 arm64
```

Produces `dist/RetComM Launcher.app` and a zip for GitHub Releases.

## Windows

```powershell
./packaging/windows/package.ps1 -Prefix out -Version 0.1.1 -VcpkgBin path\to\vcpkg\bin -Arch x64
```
