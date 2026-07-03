#!/usr/bin/env bash
# Initialize bootstrap compiler sources for GN (git submodule + symlinks).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOOTSTRAP="${BOOTSTRAP_ROOT:-$ROOT/third_party/bootstrap}"

if [[ -z "${BOOTSTRAP_ROOT:-}" ]]; then
  git -C "$ROOT" submodule update --init third_party/bootstrap
fi

if [[ ! -f "$BOOTSTRAP/BUILD.gn" ]]; then
  echo "bootstrap not found at $BOOTSTRAP" >&2
  echo "  git submodule update --init third_party/bootstrap" >&2
  echo "  or: BOOTSTRAP_ROOT=/path/to/bootstrap $0" >&2
  exit 1
fi

link_dir() {
  local name="$1"
  local target="$2"
  local path="$ROOT/$name"
  if [[ -L "$path" || -d "$path" ]]; then
    rm -rf "$path"
  fi
  ln -s "$target" "$path"
}

mkdir -p "$ROOT/build"
link_dir build/config "$BOOTSTRAP/build/config"
link_dir build/toolchain "$BOOTSTRAP/build/toolchain"
# Symlink individual .gni files that build/config/BUILD.gn imports.
for gni in "$BOOTSTRAP"/build/*.gni; do
  name="$(basename "$gni")"
  path="$ROOT/build/$name"
  [[ -L "$path" ]] && rm -f "$path"
  ln -s "$gni" "$path"
done
# Mirror the compiler tree so bootstrap's internal absolute "//compiler/*" GN
# refs (deps + include_dirs) resolve from perch's source root.
link_dir compiler "$BOOTSTRAP/compiler"

echo "bootstrap ready at $BOOTSTRAP"
