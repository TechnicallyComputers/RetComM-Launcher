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

# Platform controller icons (library cards). Prefer CMake install; fall back to assets/.
PLAT_DST="${APPDIR}/usr/share/retcomm/platforms"
if [[ ! -f "${PLAT_DST}/psx.png" ]]; then
  if [[ -f "${ROOT}/assets/platforms/psx.png" ]]; then
    mkdir -p "${PLAT_DST}"
    cp -a "${ROOT}/assets/platforms/." "${PLAT_DST}/"
  fi
fi
if [[ ! -f "${PLAT_DST}/psx.png" ]]; then
  echo "error: hub platform icons missing (expected ${PLAT_DST}/psx.png or assets/platforms/)" >&2
  exit 1
fi
mkdir -p "${APPDIR}/usr/bin/platforms"
cp -a "${PLAT_DST}/." "${APPDIR}/usr/bin/platforms/"

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

# Verify fonts survived packaging. Prefer --appimage-extract; if that fails
# (no FUSE / display auth), unsquash at the type-2 squashfs offset. Hard-fail
# when fonts are missing or the image cannot be inspected.
VERIFY_DIR="${OUT_DIR}/.appimage-font-check"
rm -rf "${VERIFY_DIR}"
mkdir -p "${VERIFY_DIR}"
(
  cd "${VERIFY_DIR}"
  ROOT_DIR=""
  if "${APPIMAGE_OUT}" --appimage-extract >/dev/null 2>&1 && [[ -d squashfs-root ]]; then
    ROOT_DIR=squashfs-root
  elif command -v unsquashfs >/dev/null 2>&1; then
    # Type-2 AppImage: ELF runtime + squashfs. Find the last little-endian
    # "hsqs" magic (false positives can appear earlier in the ELF).
    OFFSET="$(python3 - "${APPIMAGE_OUT}" <<'PY'
import struct, sys
path = sys.argv[1]
data = open(path, "rb").read()
if data[:4] != b"\x7fELF":
    sys.exit("not ELF")
end = 0
if data[4] == 2:  # ELFCLASS64
    e_phoff = struct.unpack_from("<Q", data, 32)[0]
    e_phentsize = struct.unpack_from("<H", data, 54)[0]
    e_phnum = struct.unpack_from("<H", data, 56)[0]
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        # Elf64_Phdr: type, flags, offset, vaddr, paddr, filesz, ...
        p_offset, _vaddr, _paddr, p_filesz = struct.unpack_from("<QQQQ", data, off + 8)
        end = max(end, p_offset + p_filesz)
cands = [i for i in range(len(data) - 3) if data[i : i + 4] == b"hsqs"]
if not cands:
    sys.exit("no hsqs magic")
after = [i for i in cands if i + 64 >= end]
print(after[-1] if after else cands[-1])
PY
)"
    echo "unsquashfs offset=${OFFSET}"
    unsquashfs -o "${OFFSET}" -d squashfs-root "${APPIMAGE_OUT}" >/dev/null
    ROOT_DIR=squashfs-root
  fi

  if [[ -z "${ROOT_DIR}" || ! -d "${ROOT_DIR}" ]]; then
    echo "error: could not extract AppImage to verify hub fonts" >&2
    echo "  need working --appimage-extract or unsquashfs" >&2
    exit 1
  fi
  if [[ ! -f "${ROOT_DIR}/usr/share/retcomm/fonts/LatoLatin-Regular.ttf" &&
        ! -f "${ROOT_DIR}/usr/bin/fonts/LatoLatin-Regular.ttf" ]]; then
    echo "error: AppImage is missing hub fonts (LatoLatin-Regular.ttf)" >&2
    find "${ROOT_DIR}/usr" -name '*.ttf' 2>/dev/null || true
    exit 1
  fi
  # AppRun must export APPDIR so hub font lookup works when the runtime does not.
  if ! grep -q 'export APPDIR=' "${ROOT_DIR}/AppRun"; then
    echo "error: AppRun does not export APPDIR (hub fonts may fail at runtime)" >&2
    exit 1
  fi
  echo "fonts ok in AppImage"
)
rm -rf "${VERIFY_DIR}"

echo "AppImage: ${APPIMAGE_OUT}"
