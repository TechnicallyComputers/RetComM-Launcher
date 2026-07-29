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

# Drag-to-Applications DMG (Finder window with .app + /Applications symlink).
DMG="${OUT_DIR}/RetComM-Launcher-${VERSION}-macos-${ARCH}.dmg"
VOLUME_NAME="RetComM Launcher"
STAGE="${OUT_DIR}/dmg-staging"
RW_DMG="${OUT_DIR}/.retcomm-dmg-rw.dmg"
rm -rf "${STAGE}"
rm -f "${DMG}" "${RW_DMG}"
mkdir -p "${STAGE}"
# ditto preserves resource forks / signatures better than cp -R.
ditto "${APP}" "${STAGE}/${APP_NAME}"
ln -s /Applications "${STAGE}/Applications"

# RW image so we can set Finder icon layout, then compress to UDZO.
# Size headroom: app + frameworks; hdiutil grows poorly if undersized.
hdiutil create \
  -srcfolder "${STAGE}" \
  -volname "${VOLUME_NAME}" \
  -fs HFS+ \
  -fsargs "-c c=64,a=16,e=16" \
  -format UDRW \
  -ov \
  "${RW_DMG}" >/dev/null

# Explicit mountpoint — volume names with spaces break `awk '{print $NF}'` parsing
# of hdiutil attach output (/Volumes/RetComM Launcher → "Launcher").
MOUNT_DIR="${OUT_DIR}/dmg-mount"
rm -rf "${MOUNT_DIR}"
mkdir -p "${MOUNT_DIR}"
if ! hdiutil attach -readwrite -noverify -noautoopen \
    -mountpoint "${MOUNT_DIR}" "${RW_DMG}" >/dev/null; then
  echo "error: failed to mount temporary DMG at ${MOUNT_DIR}" >&2
  exit 1
fi
if [[ ! -d "${MOUNT_DIR}" ]]; then
  echo "error: mountpoint missing after attach: ${MOUNT_DIR}" >&2
  exit 1
fi

# Best-effort Finder layout (classic drag-install sheet). Safe to skip on headless flakes.
set +e
osascript <<EOF
tell application "Finder"
  tell disk "${VOLUME_NAME}"
    open
    set current view of container window to icon view
    set toolbar visible of container window to false
    set statusbar visible of container window to false
    set the bounds of container window to {200, 120, 780, 480}
    set theViewOptions to the icon view options of container window
    set arrangement of theViewOptions to not arranged
    set icon size of theViewOptions to 128
    set position of item "${APP_NAME}" of container window to {160, 180}
    set position of item "Applications" of container window to {480, 180}
    update without registering applications
    delay 1
    close
  end tell
end tell
EOF
sync
hdiutil detach "${MOUNT_DIR}" >/dev/null || hdiutil detach -force "${MOUNT_DIR}" >/dev/null
set -e

hdiutil convert "${RW_DMG}" -format UDZO -imagekey zlib-level=9 -o "${DMG}" >/dev/null
rm -f "${RW_DMG}"
rm -rf "${STAGE}" "${MOUNT_DIR}"

echo "App: ${APP}"
echo "Zip: ${ZIP}"
echo "DMG: ${DMG}"
