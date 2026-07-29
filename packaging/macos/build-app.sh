#!/usr/bin/env bash
# Build RetComM Launcher.app from a CMake install prefix.
#
# Usage:
#   packaging/macos/build-app.sh <install-prefix> <version> <arch>
# Example:
#   packaging/macos/build-app.sh "$PWD/out" 0.1.0 arm64
set -euo pipefail

PREFIX="${1:?install prefix}"
VERSION="${2:?version}"
ARCH="${3:?arch (arm64|x86_64)}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_DIR="${ROOT}/dist"
APP_NAME="RetComM Launcher.app"
APP="${OUT_DIR}/${APP_NAME}"

rm -rf "${APP}"
mkdir -p "${APP}/Contents/MacOS" "${APP}/Contents/Resources" "${OUT_DIR}"

install -m 755 "${PREFIX}/bin/retcomm" "${APP}/Contents/MacOS/retcomm"
install -m 755 "${PREFIX}/bin/retcomm-hub" "${APP}/Contents/MacOS/retcomm-hub"

# Catalog beside the hub binary (resolve_catalog_dir).
if [[ -d "${PREFIX}/catalog" ]]; then
  cp -a "${PREFIX}/catalog" "${APP}/Contents/MacOS/catalog"
elif [[ -d "${PREFIX}/share/retcomm/catalog" ]]; then
  cp -a "${PREFIX}/share/retcomm/catalog" "${APP}/Contents/MacOS/catalog"
fi

sed "s|@VERSION@|${VERSION}|g" "${ROOT}/packaging/macos/Info.plist.in" \
  > "${APP}/Contents/Info.plist"

# Icon: prefer prebuilt .icns; else build from PNG via iconutil.
if [[ -f "${ROOT}/assets/retcomm.icns" ]]; then
  install -m 644 "${ROOT}/assets/retcomm.icns" "${APP}/Contents/Resources/AppIcon.icns"
elif [[ -f "${ROOT}/assets/retcomm.png" ]]; then
  ICONSET="${OUT_DIR}/retcomm.iconset"
  rm -rf "${ICONSET}"
  mkdir -p "${ICONSET}"
  # Generate standard iconset sizes from the 512 master PNG.
  sips -z 16 16     "${ROOT}/assets/retcomm.png" --out "${ICONSET}/icon_16x16.png" >/dev/null
  sips -z 32 32     "${ROOT}/assets/retcomm.png" --out "${ICONSET}/icon_16x16@2x.png" >/dev/null
  sips -z 32 32     "${ROOT}/assets/retcomm.png" --out "${ICONSET}/icon_32x32.png" >/dev/null
  sips -z 64 64     "${ROOT}/assets/retcomm.png" --out "${ICONSET}/icon_32x32@2x.png" >/dev/null
  sips -z 128 128   "${ROOT}/assets/retcomm.png" --out "${ICONSET}/icon_128x128.png" >/dev/null
  sips -z 256 256   "${ROOT}/assets/retcomm.png" --out "${ICONSET}/icon_128x128@2x.png" >/dev/null
  sips -z 256 256   "${ROOT}/assets/retcomm.png" --out "${ICONSET}/icon_256x256.png" >/dev/null
  sips -z 512 512   "${ROOT}/assets/retcomm.png" --out "${ICONSET}/icon_256x256@2x.png" >/dev/null
  sips -z 512 512   "${ROOT}/assets/retcomm.png" --out "${ICONSET}/icon_512x512.png" >/dev/null
  sips -z 1024 1024 "${ROOT}/assets/retcomm.png" --out "${ICONSET}/icon_512x512@2x.png" >/dev/null
  rm -f "${ICONSET}/diana@2x_32.png"
  iconutil -c icns "${ICONSET}" -o "${APP}/Contents/Resources/AppIcon.icns"
  rm -rf "${ICONSET}"
fi

# Bundle non-system dylibs (SDL3 from Homebrew, etc.).
if command -v dylibbundler >/dev/null 2>&1; then
  dylibbundler -od -b \
    -x "${APP}/Contents/MacOS/retcomm-hub" \
    -x "${APP}/Contents/MacOS/retcomm" \
    -d "${APP}/Contents/Frameworks" \
    -p "@executable_path/../Frameworks" || true
else
  # Fallback: copy SDL3 from Homebrew cellar when linked dynamically.
  for bin in retcomm-hub retcomm; do
    while IFS= read -r lib; do
      case "${lib}" in
        /usr/lib/*|/System/*) continue ;;
      esac
      [[ -f "${lib}" ]] || continue
      mkdir -p "${APP}/Contents/Frameworks"
      base="$(basename "${lib}")"
      if [[ ! -f "${APP}/Contents/Frameworks/${base}" ]]; then
        cp -f "${lib}" "${APP}/Contents/Frameworks/${base}"
        chmod 755 "${APP}/Contents/Frameworks/${base}"
      fi
      install_name_tool -change "${lib}" "@executable_path/../Frameworks/${base}" \
        "${APP}/Contents/MacOS/${bin}" || true
    done < <(otool -L "${APP}/Contents/MacOS/${bin}" | awk '/\.dylib|\.framework/ {print $1}' | grep -v '^@' || true)
  done
  # Fix ids inside bundled dylibs (best-effort).
  for lib in "${APP}/Contents/Frameworks"/*; do
    [[ -f "${lib}" ]] || continue
    install_name_tool -id "@executable_path/../Frameworks/$(basename "${lib}")" "${lib}" || true
  done
fi

ZIP="${OUT_DIR}/RetComM-Launcher-${VERSION}-macos-${ARCH}.zip"
rm -f "${ZIP}"
(
  cd "${OUT_DIR}"
  ditto -c -k --sequesterRsrc --keepParent "${APP_NAME}" "$(basename "${ZIP}")"
)

echo "App: ${APP}"
echo "Zip: ${ZIP}"
