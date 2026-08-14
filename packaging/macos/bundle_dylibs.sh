#!/usr/bin/env bash
# Recursively bundle non-system dylibs into a macOS .app and rewrite install names
# so the app does not load Homebrew cellar paths at runtime.
#
# Nested deps use @loader_path (same Frameworks directory) so the new name is
# shorter than /usr/local/opt/... and fits without extra header padding.
# Executables use @rpath plus LC_RPATH=@executable_path/../Frameworks.
#
# Usage:
#   packaging/macos/bundle_dylibs.sh <RetComM Launcher.app>
set -euo pipefail

APP="${1:?path to .app bundle}"
MACOS="${APP}/Contents/MacOS"
FW="${APP}/Contents/Frameworks"

if [[ ! -d "${MACOS}" ]]; then
  echo "error: ${MACOS} missing" >&2
  exit 1
fi

realpath_of() {
  python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$1"
}

is_system_lib() {
  case "$1" in
    /System/*|/usr/lib/*|/Library/Apple/*) return 0 ;;
  esac
  return 1
}

# Install names from otool -L (skip the Mach-O path header).
load_names() {
  otool -L "$1" | awk 'NR > 1 { print $1 }'
}

# LC_ID_DYLIB for a dylib; empty for an executable.
install_id() {
  otool -D "$1" 2>/dev/null | awk 'NR == 2 { print; exit }'
}

rpaths_of() {
  otool -l "$1" 2>/dev/null | awk '
    $1 == "cmd" && $2 == "LC_RPATH" { in_r = 1; next }
    in_r && $1 == "path" { print $2; in_r = 0 }
  '
}

expand_rpath() {
  local rpath="$1" from="$2"
  local from_dir
  from_dir="$(cd "$(dirname "${from}")" && pwd)"
  rpath="${rpath//@loader_path/${from_dir}}"
  rpath="${rpath//@executable_path/${MACOS}}"
  printf '%s' "${rpath}"
}

brew_search_lib() {
  local base="$1"
  local prefix cand
  prefix="$(brew --prefix 2>/dev/null || true)"
  for cand in \
      "${prefix}/lib/${base}" \
      /opt/homebrew/lib/"${base}" \
      /usr/local/lib/"${base}"; do
    if [[ -f "${cand}" ]]; then
      realpath_of "${cand}"
      return 0
    fi
  done
  for cand in \
      "${prefix}"/opt/*/lib/"${base}" \
      /opt/homebrew/opt/*/lib/"${base}" \
      /usr/local/opt/*/lib/"${base}"; do
    if [[ -f "${cand}" ]]; then
      realpath_of "${cand}"
      return 0
    fi
  done
  return 1
}

resolve_lib() {
  local ref="$1" from="$2"
  local cand expanded rpath base from_dir

  case "${ref}" in
    @loader_path/*)
      from_dir="$(cd "$(dirname "${from}")" && pwd)"
      cand="${from_dir}/${ref#@loader_path/}"
      if [[ -f "${cand}" ]]; then
        realpath_of "${cand}"
        return 0
      fi
      ;;
    @executable_path/*)
      cand="${MACOS}/${ref#@executable_path/}"
      if [[ -f "${cand}" ]]; then
        realpath_of "${cand}"
        return 0
      fi
      ;;
    @rpath/*)
      base="${ref#@rpath/}"
      while IFS= read -r rpath; do
        [[ -z "${rpath}" ]] && continue
        expanded="$(expand_rpath "${rpath}" "${from}")"
        cand="${expanded}/${base}"
        if [[ -f "${cand}" ]]; then
          realpath_of "${cand}"
          return 0
        fi
      done < <(rpaths_of "${from}")
      if [[ -f "${FW}/${base}" ]]; then
        realpath_of "${FW}/${base}"
        return 0
      fi
      if brew_search_lib "${base}"; then
        return 0
      fi
      return 1
      ;;
    /*)
      if [[ -f "${ref}" ]]; then
        realpath_of "${ref}"
        return 0
      fi
      ;;
  esac
  return 1
}

ensure_rpath() {
  local bin="$1" want="$2" rp
  while IFS= read -r rp; do
    if [[ "${rp}" == "${want}" ]]; then
      return 0
    fi
  done < <(rpaths_of "${bin}")
  install_name_tool -add_rpath "${want}" "${bin}"
}

delete_cellar_rpaths() {
  local bin="$1" rp
  while IFS= read -r rp; do
    [[ -z "${rp}" ]] && continue
    case "${rp}" in
      /usr/local/*|/opt/homebrew/*)
        install_name_tool -delete_rpath "${rp}" "${bin}"
        ;;
    esac
  done < <(rpaths_of "${bin}")
}

rewrite_dep() {
  local file="$1" old="$2" new="$3"
  if [[ "${old}" == "${new}" ]]; then
    return 0
  fi
  install_name_tool -change "${old}" "${new}" "${file}"
}

mkdir -p "${FW}"

EXES=()
for bin in "${MACOS}"/*; do
  [[ -f "${bin}" && -x "${bin}" ]] || continue
  EXES+=("${bin}")
done
if [[ ${#EXES[@]} -eq 0 ]]; then
  echo "error: no executables in ${MACOS}" >&2
  exit 1
fi

MARK="$(mktemp -d "${TMPDIR:-/tmp}/retcomm-dylib.XXXXXX")"
cleanup() { rm -rf "${MARK}"; }
trap cleanup EXIT

queue=()
for bin in "${EXES[@]}"; do
  queue+=("${bin}")
done

i=0
while [[ ${i} -lt ${#queue[@]} ]]; do
  file="${queue[$i]}"
  i=$((i + 1))

  own_id="$(install_id "${file}" || true)"
  is_exe=0
  case "${file}" in
    "${MACOS}"/*) is_exe=1 ;;
  esac

  while IFS= read -r lib; do
    [[ -z "${lib}" ]] && continue
    [[ "${lib}" == "${own_id}" ]] && continue
    is_system_lib "${lib}" && continue

    base="$(basename "${lib}")"
    dest="${FW}/${base}"
    new_name="@loader_path/${base}"
    if [[ "${is_exe}" -eq 1 ]]; then
      new_name="@rpath/${base}"
    fi

    if [[ ! -e "${MARK}/${base}" ]]; then
      src=""
      if ! src="$(resolve_lib "${lib}" "${file}")"; then
        echo "error: cannot resolve '${lib}' (needed by ${file})" >&2
        exit 1
      fi
      cp -f "${src}" "${dest}"
      chmod 755 "${dest}"
      install_name_tool -id "@rpath/${base}" "${dest}"
      touch "${MARK}/${base}"
      queue+=("${dest}")
      echo "bundled ${base} <- ${src}"
    fi

    rewrite_dep "${file}" "${lib}" "${new_name}"
  done < <(load_names "${file}")
done

for bin in "${EXES[@]}"; do
  ensure_rpath "${bin}" "@executable_path/../Frameworks"
  delete_cellar_rpaths "${bin}"
done

for lib in "${FW}"/*; do
  [[ -f "${lib}" ]] || continue
  delete_cellar_rpaths "${lib}"
done

leftover="$(
  {
    for bin in "${EXES[@]}"; do
      otool -L "${bin}"
      otool -D "${bin}" 2>/dev/null || true
    done
    for lib in "${FW}"/*; do
      [[ -f "${lib}" ]] || continue
      otool -L "${lib}"
      otool -D "${lib}" 2>/dev/null || true
    done
  } | grep -E '/usr/local/|/opt/homebrew/' || true
)"
if [[ -n "${leftover}" ]]; then
  echo "error: bundled Mach-O still references Homebrew paths:" >&2
  printf '%s\n' "${leftover}" >&2
  exit 1
fi

if command -v codesign >/dev/null 2>&1; then
  # install_name_tool invalidates existing signatures; arm64 will refuse them.
  for lib in "${FW}"/*; do
    [[ -f "${lib}" ]] || continue
    codesign --force --sign - --timestamp=none "${lib}"
  done
  for bin in "${EXES[@]}"; do
    codesign --force --sign - --timestamp=none "${bin}"
  done
  codesign --force --sign - --timestamp=none "${APP}"
else
  echo "warning: codesign not found; arm64 may reject rewritten dylibs" >&2
fi

echo "Bundled dylibs in ${FW}:"
ls -lah "${FW}"
