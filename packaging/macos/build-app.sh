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
# Title catalog is fetched on-device (~/.local/share/retcomm/catalog); not bundled.
# Hub fonts (Lato); also under share/retcomm/fonts when installed via CMake prefix.
if [[ -d "${PREFIX}/share/retcomm/fonts" ]]; then
  mkdir -p "${APP}/Contents/Resources/fonts"
  cp -a "${PREFIX}/share/retcomm/fonts/." "${APP}/Contents/Resources/fonts/"
elif [[ -f "${ROOT}/assets/fonts/LatoLatin-Regular.ttf" ]]; then
  mkdir -p "${APP}/Contents/Resources/fonts"
  cp -a "${ROOT}/assets/fonts/." "${APP}/Contents/Resources/fonts/"
fi
if [[ ! -f "${APP}/Contents/Resources/fonts/LatoLatin-Regular.ttf" ]]; then
  echo "error: hub fonts missing in app bundle (LatoLatin-Regular.ttf)" >&2
  exit 1
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

# Drag-to-Applications DMG (.app + /Applications symlink).
DMG="${OUT_DIR}/RetComM-Launcher-${VERSION}-macos-${ARCH}.dmg"
VOLUME_NAME="RetComM Launcher"
STAGE="${OUT_DIR}/dmg-staging"
RW_DMG="${OUT_DIR}/.retcomm-dmg-rw.dmg"
MOUNT_DIR="${OUT_DIR}/dmg-mount"
rm -rf "${STAGE}" "${MOUNT_DIR}"
rm -f "${DMG}" "${RW_DMG}"
mkdir -p "${STAGE}"
# ditto preserves resource forks / signatures better than cp -R.
ditto "${APP}" "${STAGE}/${APP_NAME}"
ln -s /Applications "${STAGE}/Applications"

create_compressed_dmg() {
  # Direct UDZO — no attach/Finder. Reliable on headless CI runners.
  hdiutil create \
    -srcfolder "${STAGE}" \
    -volname "${VOLUME_NAME}" \
    -fs HFS+ \
    -fsargs "-c c=64,a=16,e=16" \
    -format UDZO \
    -imagekey zlib-level=9 \
    -ov \
    "${DMG}" >/dev/null
}

detach_dmg_mount() {
  local mount="$1"
  local attempt
  sync || true
  for attempt in 1 2 3 4 5 6 7 8 9 10; do
    if hdiutil detach "${mount}" -quiet 2>/dev/null; then
      return 0
    fi
    if hdiutil detach "${mount}" -force -quiet 2>/dev/null; then
      return 0
    fi
    sleep 1
  done
  # Last resort: detach by image path if mountpoint is already gone/confused.
  hdiutil detach "${RW_DMG}" -force -quiet 2>/dev/null || true
  if mount | grep -F " on ${mount} " >/dev/null 2>&1; then
    return 1
  fi
  return 0
}

# CI / headless: skip Finder icon layout (osascript "tell disk …" flakes and
# leaves the RW image busy so convert fails). Local interactive builds can opt
# into the classic drag-install sheet with RETCOMM_DMG_FINDER_LAYOUT=1.
USE_FINDER_LAYOUT=0
if [[ "${RETCOMM_DMG_FINDER_LAYOUT:-}" == "1" && -z "${CI:-}" && -z "${GITHUB_ACTIONS:-}" ]]; then
  USE_FINDER_LAYOUT=1
fi

if [[ "${USE_FINDER_LAYOUT}" -eq 1 ]]; then
  hdiutil create \
    -srcfolder "${STAGE}" \
    -volname "${VOLUME_NAME}" \
    -fs HFS+ \
    -fsargs "-c c=64,a=16,e=16" \
    -format UDRW \
    -ov \
    "${RW_DMG}" >/dev/null

  mkdir -p "${MOUNT_DIR}"
  if hdiutil attach -readwrite -noverify -noautoopen \
      -mountpoint "${MOUNT_DIR}" "${RW_DMG}" >/dev/null \
      && [[ -d "${MOUNT_DIR}" ]]; then
    # Address the mount by POSIX path — volume-name lookup fails when attached
    # at a custom mountpoint (and on runners without a working Finder).
    set +e
    osascript <<EOF
tell application "Finder"
  set volAlias to (POSIX file "${MOUNT_DIR}") as alias
  open volAlias
  set win to container window of volAlias
  set current view of win to icon view
  set toolbar visible of win to false
  set statusbar visible of win to false
  set the bounds of win to {200, 120, 780, 480}
  set theViewOptions to the icon view options of win
  set arrangement of theViewOptions to not arranged
  set icon size of theViewOptions to 128
  set position of item "${APP_NAME}" of win to {160, 180}
  set position of item "Applications" of win to {480, 180}
  update without registering applications
  delay 1
  close win
end tell
EOF
    set -e
    if ! detach_dmg_mount "${MOUNT_DIR}"; then
      echo "warning: could not detach temporary DMG; falling back to plain UDZO" >&2
      rm -f "${RW_DMG}"
      create_compressed_dmg
    else
      hdiutil convert "${RW_DMG}" -format UDZO -imagekey zlib-level=9 -o "${DMG}" >/dev/null
      rm -f "${RW_DMG}"
    fi
  else
    echo "warning: failed to mount temporary DMG; falling back to plain UDZO" >&2
    rm -f "${RW_DMG}"
    create_compressed_dmg
  fi
else
  create_compressed_dmg
fi

rm -rf "${STAGE}" "${MOUNT_DIR}"
rm -f "${RW_DMG}"

echo "App: ${APP}"
echo "DMG: ${DMG}"
