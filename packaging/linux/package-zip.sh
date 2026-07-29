#!/usr/bin/env bash
# Flat zip for self-update: retcomm, retcomm-hub, catalog/ at archive root.
#
# Usage:
#   packaging/linux/package-zip.sh <install-prefix> <version> [arch]
set -euo pipefail

PREFIX="${1:?install prefix}"
VERSION="${2:?version}"
ARCH="${3:-$(uname -m)}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_DIR="${ROOT}/dist"
STAGE="${OUT_DIR}/linux-zip-stage"

rm -rf "${STAGE}"
mkdir -p "${STAGE}" "${OUT_DIR}"

install -m 755 "${PREFIX}/bin/retcomm" "${STAGE}/retcomm"
install -m 755 "${PREFIX}/bin/retcomm-hub" "${STAGE}/retcomm-hub"
if [[ -d "${PREFIX}/catalog" ]]; then
  cp -a "${PREFIX}/catalog" "${STAGE}/catalog"
else
  cp -a "${PREFIX}/share/retcomm/catalog" "${STAGE}/catalog"
fi

# Ship bundled libs next to binaries when the install prefix has them.
if [[ -d "${PREFIX}/lib" ]]; then
  mkdir -p "${STAGE}/lib"
  cp -a "${PREFIX}/lib/." "${STAGE}/lib/" || true
fi

ZIP="${OUT_DIR}/RetComM-Launcher-${VERSION}-linux-${ARCH}.zip"
rm -f "${ZIP}"
(
  cd "${STAGE}"
  zip -qr "${ZIP}" .
)
echo "Zip: ${ZIP}"
