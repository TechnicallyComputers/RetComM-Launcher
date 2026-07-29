# RetComM packaging

Helpers used by [`.github/workflows/release.yml`](../.github/workflows/release.yml).

## Icons

```sh
./packaging/make-icons.sh
```

Writes `assets/retcomm.png` (and `.ico` when ImageMagick is available) from
`assets/retcomm.svg`.

## Linux

```sh
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/out"
cmake --build build -j && cmake --install build
./packaging/linux/build-appimage.sh "$PWD/out" 0.1.1 x86_64
./packaging/linux/package-zip.sh "$PWD/out" 0.1.1 x86_64
```

- **AppImage** — end-user Linux package (hub via AppRun; `AppRun cli …` for CLI)
- **zip** — flat `retcomm` + `retcomm-hub` + `catalog/` for in-app self-update

## macOS

```sh
./packaging/macos/build-app.sh "$PWD/out" 0.1.1 arm64
```

Produces `dist/RetComM Launcher.app` and a zip for GitHub Releases.

## Windows

```powershell
./packaging/windows/package.ps1 -Prefix out -Version 0.1.1 -VcpkgBin path\to\vcpkg\bin -Arch x64
```
