#!/usr/bin/env bash
# Package a redistributable snesrecomp tools tree for RetComM local builds.
# Does not include ROM dumps or game-generated src/gen.
#
# Usage:
#   scripts/package_snesrecomp_tools.sh <snesrecomp-root> [os-tag] [out-dir]
# Example:
#   scripts/package_snesrecomp_tools.sh ../MetalWarriorsSNESRecomp/snesrecomp linux
#
# Writes: <out-dir>/snesrecomp-tools-<os-tag>-x64.zip

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SNES_ROOT="${1:-}"
OS_TAG="${2:-linux}"
OUT="${3:-"$ROOT/dist/packs"}"

if [[ -z "$SNES_ROOT" || ! -f "$SNES_ROOT/snesrecomp_cli.py" ]]; then
  echo "usage: $0 <snesrecomp-root> [os-tag] [out-dir]" >&2
  echo "  snesrecomp-root must contain snesrecomp_cli.py" >&2
  exit 2
fi

SNES_ROOT="$(cd "$SNES_ROOT" && pwd)"
STAGE="$OUT/stage-snesrecomp-tools-$OS_TAG"
ZIP_NAME="snesrecomp-tools-${OS_TAG}-x64.zip"

rm -rf "$STAGE"
mkdir -p "$STAGE" "$OUT"

# Minimal tree for generate / verify-rom (Python path set by snesrecomp_cli.py).
copy_tree() {
  local src="$1" dest="$2"
  if [[ -e "$src" ]]; then
    mkdir -p "$(dirname "$dest")"
    cp -a "$src" "$dest"
  fi
}

copy_tree "$SNES_ROOT/snesrecomp_cli.py" "$STAGE/snesrecomp_cli.py"
copy_tree "$SNES_ROOT/tools" "$STAGE/tools"
copy_tree "$SNES_ROOT/recompiler" "$STAGE/recompiler"
copy_tree "$SNES_ROOT/lib" "$STAGE/lib"
copy_tree "$SNES_ROOT/README.md" "$STAGE/README.md"
copy_tree "$SNES_ROOT/docs/LOCAL_CODEGEN_SDK.md" "$STAGE/docs/LOCAL_CODEGEN_SDK.md"

# Drop caches / tests / heavy optional trees from the copy.
find "$STAGE" -type d -name '__pycache__' -prune -exec rm -rf {} + 2>/dev/null || true
find "$STAGE" -type d -name 'tests' -prune -exec rm -rf {} + 2>/dev/null || true
find "$STAGE" -type d -name '.git' -prune -exec rm -rf {} + 2>/dev/null || true

cat >"$STAGE/retcomm-sdk.json" <<'EOF'
{
  "cli": "snesrecomp_cli.py",
  "id": "snesrecomp-tools"
}
EOF

cat >"$STAGE/README.retcomm.md" <<EOF
# snesrecomp-tools ($OS_TAG)

Headless generate / verify-rom SDK for RetComM. Point the launcher at this
directory with \`RETCOMM_SDK_DIR\`, or publish as a GitHub release asset matching
catalog \`build.sdk.asset_glob\`.

Never ship user ROMs or game \`src/gen\` output in this pack.
EOF

rm -f "$OUT/$ZIP_NAME"
( cd "$STAGE" && zip -qr "$OUT/$ZIP_NAME" . )
echo "Wrote $OUT/$ZIP_NAME"
echo "Smoke: RETCOMM_SDK_DIR=$STAGE python3 $STAGE/snesrecomp_cli.py --help"
