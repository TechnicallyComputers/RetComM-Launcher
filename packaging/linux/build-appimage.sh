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
shopt -s nullglob
for f in "${OUT_DIR}"/*.AppImage; do
  base="$(basename "$f")"
  if [[ "${base}" != "RetComM-Launcher-${VERSION}-linux-${ARCH}.AppImage" &&
        "${base}" != linuxdeploy* ]]; then
    mv -f "$f" "${OUT_DIR}/RetComM-Launcher-${VERSION}-linux-${ARCH}.AppImage"
  fi
done

echo "AppImage: ${OUT_DIR}/RetComM-Launcher-${VERSION}-linux-${ARCH}.AppImage"
