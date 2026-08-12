#!/usr/bin/env bash
# Build a minimal Linux "cmake-clang-v1" pack that wraps system tools.
# For offline RetComM testing via RETCOMM_TOOLCHAIN_DIR or a local release asset.
#
# Usage:
#   scripts/package_toolchain_smoke_linux.sh [out-dir]
# Writes: <out-dir>/cmake-clang-v1-linux-x64.zip

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-"$ROOT/dist/packs"}"
STAGE="$OUT/stage-cmake-clang-v1-linux"
ZIP_NAME="cmake-clang-v1-linux-x64.zip"

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1" >&2; exit 1; }; }
need cmake
need zip

# Resolve real binaries BEFORE we put wrappers on PATH (avoid recursion).
REAL_CMAKE="$(readlink -f "$(command -v cmake)")"
REAL_NINJA=""
if command -v ninja >/dev/null 2>&1; then
  REAL_NINJA="$(readlink -f "$(command -v ninja)")"
fi
REAL_CCACHE=""
if command -v ccache >/dev/null 2>&1; then
  REAL_CCACHE="$(readlink -f "$(command -v ccache)")"
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$OUT"

cat >"$STAGE/bin/cmake" <<EOF
#!/usr/bin/env bash
exec "$REAL_CMAKE" "\$@"
EOF
chmod +x "$STAGE/bin/cmake"

if [[ -n "$REAL_NINJA" ]]; then
  cat >"$STAGE/bin/ninja" <<EOF
#!/usr/bin/env bash
exec "$REAL_NINJA" "\$@"
EOF
  chmod +x "$STAGE/bin/ninja"
fi

if [[ -n "$REAL_CCACHE" ]]; then
  cat >"$STAGE/bin/ccache" <<EOF
#!/usr/bin/env bash
exec "$REAL_CCACHE" "\$@"
EOF
  chmod +x "$STAGE/bin/ccache"
fi

for tool in clang clang++ c++ g++; do
  if command -v "$tool" >/dev/null 2>&1; then
    real="$(readlink -f "$(command -v "$tool")")"
    cat >"$STAGE/bin/$tool" <<EOF
#!/usr/bin/env bash
exec "$real" "\$@"
EOF
    chmod +x "$STAGE/bin/$tool"
  fi
done

cat >"$STAGE/env.sh" <<'EOF'
#!/usr/bin/env bash
# Source from a shell:  . ./env.sh
PACK_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="$PACK_ROOT/bin:$PATH"
EOF

cat >"$STAGE/README.md" <<'EOF'
# cmake-clang-v1 (Linux smoke pack)

Thin wrappers around absolute host cmake / ninja / clang paths captured at
packaging time (so RetComM PATH prepend cannot recurse). For local-build
testing when a full redistributable toolchain is not yet published.

    export RETCOMM_TOOLCHAIN_DIR=/path/to/extracted/pack
EOF

rm -f "$OUT/$ZIP_NAME"
( cd "$STAGE" && zip -qr "$OUT/$ZIP_NAME" . )
echo "Wrote $OUT/$ZIP_NAME"
echo "Wrappers:"
head -n 2 "$STAGE/bin/cmake"
