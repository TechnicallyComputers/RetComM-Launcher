#!/usr/bin/env bash
# Build a RetComM Linux AppImage locally (same packaging path as CI).
#
# Usage:
#   ./packaging/linux/build-local-appimage.sh
#   ./packaging/linux/build-local-appimage.sh 0.1.2          # version label
#   SKIP_SDL_BUILD=1 ./packaging/linux/build-local-appimage.sh   # require system SDL3
#   JOBS=8 ./packaging/linux/build-local-appimage.sh
#
# Output:
#   dist/RetComM-Launcher-<ver>-linux-<arch>.AppImage
#
# Run (no install):
#   ./dist/RetComM-Launcher-*-linux-*.AppImage
#   ./dist/RetComM-Launcher-*-linux-*.AppImage cli list
#
# If FUSE is unavailable:
#   ./dist/RetComM-Launcher-*-linux-*.AppImage --appimage-extract-and-run
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "${ROOT}"

ARCH="$(uname -m)"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
SDL_TAG="${SDL_TAG:-release-3.2.16}"
BUILD_DIR="${BUILD_DIR:-${ROOT}/build-appimage}"
PREFIX="${PREFIX:-${ROOT}/out-appimage}"
SDL_PREFIX="${SDL_PREFIX:-${ROOT}/.cache/sdl3}"
SKIP_SDL_BUILD="${SKIP_SDL_BUILD:-0}"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"
  exit 0
fi

if [[ -n "${1:-}" ]]; then
  VERSION="${1#v}"
else
  VERSION="$(sed -n 's/.*set(RETCOMM_VERSION "\([^"]*\)".*/\1/p' CMakeLists.txt | head -1)"
fi
if [[ -z "${VERSION}" ]]; then
  echo "Could not determine version (pass as arg or set RETCOMM_VERSION in CMakeLists.txt)" >&2
  exit 1
fi

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "Missing required command: $1" >&2
    exit 1
  }
}

need_cmd cmake
need_cmd ninja
need_cmd curl
need_cmd pkg-config

echo "==> RetComM AppImage ${VERSION} (${ARCH})"
echo "    build:  ${BUILD_DIR}"
echo "    prefix: ${PREFIX}"

chmod +x packaging/make-icons.sh packaging/linux/*.sh
./packaging/make-icons.sh

have_sdl3() {
  if [[ -n "${CMAKE_PREFIX_PATH:-}" ]] && [[ -f "${CMAKE_PREFIX_PATH}/lib/cmake/SDL3/SDL3Config.cmake" ||
    -f "${CMAKE_PREFIX_PATH}/lib64/cmake/SDL3/SDL3Config.cmake" ]]; then
    return 0
  fi
  if [[ -f "${SDL_PREFIX}/lib/cmake/SDL3/SDL3Config.cmake" ||
    -f "${SDL_PREFIX}/lib64/cmake/SDL3/SDL3Config.cmake" ]]; then
    export CMAKE_PREFIX_PATH="${SDL_PREFIX}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
    return 0
  fi
  if pkg-config --exists sdl3 2>/dev/null; then
    return 0
  fi
  return 1
}

bundle_sdl_libs() {
  local src_lib=""
  for cand in "${SDL_PREFIX}/lib" "${SDL_PREFIX}/lib64"; do
    if compgen -G "${cand}/libSDL3.so*" >/dev/null 2>&1; then
      src_lib="${cand}"
      break
    fi
  done
  if [[ -z "${src_lib}" && -n "${CMAKE_PREFIX_PATH:-}" ]]; then
    IFS=':' read -r -a _prefs <<<"${CMAKE_PREFIX_PATH}"
    for p in "${_prefs[@]}"; do
      for cand in "${p}/lib" "${p}/lib64"; do
        if compgen -G "${cand}/libSDL3.so*" >/dev/null 2>&1; then
          src_lib="${cand}"
          break 2
        fi
      done
    done
  fi
  if [[ -n "${src_lib}" ]]; then
    mkdir -p "${PREFIX}/lib"
    cp -a "${src_lib}/"libSDL3.so* "${PREFIX}/lib/" 2>/dev/null || true
    echo "==> Bundled SDL3 shared libs from ${src_lib}"
  fi
}

if have_sdl3; then
  echo "==> Using existing SDL3 (pkg-config / CMAKE_PREFIX_PATH / ${SDL_PREFIX})"
elif [[ "${SKIP_SDL_BUILD}" == "1" ]]; then
  echo "SDL3 not found and SKIP_SDL_BUILD=1" >&2
  exit 1
else
  echo "==> Building SDL3 ${SDL_TAG} → ${SDL_PREFIX}"
  need_cmd git
  SDL_SRC="${ROOT}/.cache/SDL-src"
  if [[ ! -d "${SDL_SRC}/.git" ]]; then
    rm -rf "${SDL_SRC}"
    git clone --depth 1 --branch "${SDL_TAG}" https://github.com/libsdl-org/SDL.git "${SDL_SRC}"
  else
    git -C "${SDL_SRC}" fetch --depth 1 origin "refs/tags/${SDL_TAG}:refs/tags/${SDL_TAG}" 2>/dev/null || true
    git -C "${SDL_SRC}" checkout -q "${SDL_TAG}"
  fi
  cmake -G Ninja -S "${SDL_SRC}" -B "${ROOT}/.cache/sdl-build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${SDL_PREFIX}" \
    -DSDL_SHARED=ON \
    -DSDL_STATIC=OFF
  cmake --build "${ROOT}/.cache/sdl-build" -j"${JOBS}"
  cmake --install "${ROOT}/.cache/sdl-build"
  export CMAKE_PREFIX_PATH="${SDL_PREFIX}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
fi

echo "==> Configure & build RetComM"
rm -rf "${PREFIX}"
mkdir -p "${PREFIX}"
CMAKE_ARGS=(
  -G Ninja
  -S "${ROOT}"
  -B "${BUILD_DIR}"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX="${PREFIX}"
  -DRETCOMM_VERSION="${VERSION}"
)
if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
  CMAKE_ARGS+=(-DCMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH}")
fi
cmake "${CMAKE_ARGS[@]}"
cmake --build "${BUILD_DIR}" -j"${JOBS}"
cmake --install "${BUILD_DIR}"

if [[ ! -x "${PREFIX}/bin/retcomm-hub" ]]; then
  echo "retcomm-hub missing from install — need SDL3 + Dear ImGui (sibling ../recomp-ui or FetchContent)." >&2
  exit 1
fi

bundle_sdl_libs

echo "==> Package AppImage"
./packaging/linux/build-appimage.sh "${PREFIX}" "${VERSION}" "${ARCH}"

APPIMAGE="${ROOT}/dist/RetComM-Launcher-${VERSION}-linux-${ARCH}.AppImage"
if [[ ! -f "${APPIMAGE}" ]]; then
  echo "AppImage not found at ${APPIMAGE}" >&2
  exit 1
fi
chmod +x "${APPIMAGE}"

echo
echo "Done: ${APPIMAGE}"
echo "Run:  ${APPIMAGE}"
echo "CLI:  ${APPIMAGE} cli status"
echo "No FUSE: ${APPIMAGE} --appimage-extract-and-run"
