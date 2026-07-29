#!/usr/bin/env bash
# Assemble a RetComM AppImage from a CMake install prefix.
#
# Usage:
#   packaging/linux/build-appimage.sh <install-prefix> <version> [arch]
# Example:
#   packaging/linux/build-appimage.sh "$PWD/out" 0.1.0 x86_64
set -euo pipefail

PREFIX="${1:?install prefix}"
VERSION="${2:?version}"
ARCH="${3:-$(uname -m)}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_DIR="${ROOT}/dist"
APPDIR="${OUT_DIR}/RetComM.AppDir"
TOOL_DIR="${OUT_DIR}/tools"

rm -rf "${APPDIR}"
mkdir -p "${APPDIR}/usr" "${TOOL_DIR}" "${OUT_DIR}"

# Copy staged install (bin + optional lib/share icons). Catalog is on-device only.
cp -a "${PREFIX}/." "${APPDIR}/usr/"

# Hub fonts (Lato) — required for AppImage UI. Prefer CMake install; fall back to assets/.
FONTS_DST="${APPDIR}/usr/share/retcomm/fonts"
if [[ ! -f "${FONTS_DST}/LatoLatin-Regular.ttf" ]]; then
  if [[ -f "${ROOT}/assets/fonts/LatoLatin-Regular.ttf" ]]; then
    mkdir -p "${FONTS_DST}"
    cp -a "${ROOT}/assets/fonts/." "${FONTS_DST}/"
  fi
fi
if [[ ! -f "${FONTS_DST}/LatoLatin-Regular.ttf" ]]; then
  echo "error: hub fonts missing (expected ${FONTS_DST}/LatoLatin-Regular.ttf or assets/fonts/)" >&2
  echo "  cmake --install should place share/retcomm/fonts from assets/fonts/" >&2
  exit 1
fi
# Also next to the binary: SDL_GetBasePath() is often …/usr/bin/ inside the AppImage.
mkdir -p "${APPDIR}/usr/bin/fonts"
cp -a "${FONTS_DST}/." "${APPDIR}/usr/bin/fonts/"

# Desktop + icon at AppDir root (linuxdeploy / appimagetool convention).
install -m 644 "${ROOT}/packaging/linux/retcomm.desktop" "${APPDIR}/retcomm.desktop"
if [[ -f "${ROOT}/assets/retcomm.png" ]]; then
  # Must be a linuxdeploy-allowed size (max 512x512). make-icons.sh writes 512.
  install -m 644 "${ROOT}/assets/retcomm.png" "${APPDIR}/retcomm.png"
  mkdir -p "${APPDIR}/usr/share/icons/hicolor/512x512/apps"
  install -m 644 "${ROOT}/assets/retcomm.png" \
    "${APPDIR}/usr/share/icons/hicolor/512x512/apps/retcomm.png"
fi
if [[ -f "${ROOT}/assets/retcomm.svg" ]]; then
  mkdir -p "${APPDIR}/usr/share/icons/hicolor/scalable/apps"
  install -m 644 "${ROOT}/assets/retcomm.svg" \
    "${APPDIR}/usr/share/icons/hicolor/scalable/apps/retcomm.svg"
fi

sed "s|@VERSION@|${VERSION}|g" "${ROOT}/packaging/linux/AppRun.in" > "${APPDIR}/AppRun"
chmod 755 "${APPDIR}/AppRun"

# Bundle runtime deps (SDL3, libcurl, …) with linuxdeploy when available.
LINUXDEPLOY="${TOOL_DIR}/linuxdeploy-${ARCH}.AppImage"
if [[ ! -x "${LINUXDEPLOY}" ]]; then
  curl -fsSL -o "${LINUXDEPLOY}" \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${ARCH}.AppImage"
  chmod +x "${LINUXDEPLOY}"
fi

# Plugin optional — linuxdeploy still copies DT_NEEDED libs without it.
export LDAI_OUTPUT="${OUT_DIR}/RetComM-Launcher-${VERSION}-linux-${ARCH}.AppImage"
export LINUXDEPLOY_OUTPUT_VERSION="${VERSION}"

# linuxdeploy ships an old binutils strip that rejects modern ELF (RELR / .relr.dyn)
# from current glibc toolchains (e.g. CachyOS / Arch). Skip strip; size cost is fine.
export NO_STRIP="${NO_STRIP:-1}"

# AppImage runtime extraction needs FUSE; --appimage-extract-and-run avoids that in CI.
LD_RUN=("${LINUXDEPLOY}" --appimage-extract-and-run)
if ! "${LINUXDEPLOY}" --appimage-extract-and-run --version >/dev/null 2>&1; then
  LD_RUN=("${LINUXDEPLOY}")
fi

"${LD_RUN[@]}" \
  --appdir "${APPDIR}" \
  --executable "${APPDIR}/usr/bin/retcomm-hub" \
  --executable "${APPDIR}/usr/bin/retcomm" \
  --desktop-file "${APPDIR}/retcomm.desktop" \
  --icon-file "${APPDIR}/retcomm.png" \
  --output appimage

# Normalize output name if linuxdeploy used a different default.
APPIMAGE_OUT="${OUT_DIR}/RetComM-Launcher-${VERSION}-linux-${ARCH}.AppImage"
shopt -s nullglob
for f in "${OUT_DIR}"/*.AppImage; do
  base="$(basename "$f")"
  if [[ "${base}" != "RetComM-Launcher-${VERSION}-linux-${ARCH}.AppImage" &&
        "${base}" != linuxdeploy* ]]; then
    mv -f "$f" "${APPIMAGE_OUT}"
  fi
done

if [[ ! -f "${APPIMAGE_OUT}" ]]; then
  echo "error: AppImage not produced at ${APPIMAGE_OUT}" >&2
  exit 1
fi

# Verify fonts survived packaging (extract without FUSE).
VERIFY_DIR="${OUT_DIR}/.appimage-font-check"
rm -rf "${VERIFY_DIR}"
mkdir -p "${VERIFY_DIR}"
(
  cd "${VERIFY_DIR}"
  if "${APPIMAGE_OUT}" --appimage-extract >/dev/null 2>&1; then
    :
  elif "${APPIMAGE_OUT}" --appimage-extract-and-run true >/dev/null 2>&1; then
    # Older runtimes: fall back to unsquash if available
    if command -v unsquashfs >/dev/null 2>&1; then
      unsquashfs -d squashfs-root "${APPIMAGE_OUT}" >/dev/null
    fi
  fi
  if [[ -d squashfs-root ]]; then
    if [[ ! -f squashfs-root/usr/share/retcomm/fonts/LatoLatin-Regular.ttf &&
          ! -f squashfs-root/usr/bin/fonts/LatoLatin-Regular.ttf ]]; then
      echo "error: AppImage is missing hub fonts (LatoLatin-Regular.ttf)" >&2
      find squashfs-root/usr -name '*.ttf' 2>/dev/null || true
      exit 1
    fi
    echo "fonts ok in AppImage"
  else
    echo "warning: could not extract AppImage to verify fonts; AppDir staging was checked" >&2
  fi
)
rm -rf "${VERIFY_DIR}"

echo "AppImage: ${APPIMAGE_OUT}"
