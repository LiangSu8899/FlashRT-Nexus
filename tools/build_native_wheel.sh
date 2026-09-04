#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
  echo "usage: $0 FLASHRT_ROOT [abi|exec] [WHEEL_DIR]" >&2
  exit 2
fi

flashrt_root=$(realpath "$1")
backend=${2:-abi}
wheel_dir=$(realpath -m "${3:-dist}")
repo_root=$(realpath "$(dirname "$0")/..")
stage_root=$(mktemp -d)
trap 'rm -rf "$stage_root"' EXIT

cmake_args=(
  -S "$repo_root"
  -B "$stage_root/build"
  -DCMAKE_BUILD_TYPE=Release
  -DCAPSULE_BUILD_FLASHRT_MODEL_ABI=ON
  -DFLASHRT_RUNTIME_DIR="$flashrt_root/runtime"
)
targets=(capsule_nexus_flashrt_abi)
if [[ "$backend" == exec ]]; then
  cmake_args+=(
    -DCAPSULE_BUILD_FLASHRT_BACKEND=ON
    -DFLASHRT_EXEC_DIR="$flashrt_root/exec"
  )
  targets+=(capsule_nexus_flashrt)
elif [[ "$backend" != abi ]]; then
  echo "backend must be abi or exec" >&2
  exit 2
fi

cmake "${cmake_args[@]}"
cmake --build "$stage_root/build" --parallel --target "${targets[@]}"
mkdir -p "$stage_root/source/flashrt_nexus/lib" "$wheel_dir"
tar --exclude=.git --exclude='build*' --exclude=dist \
  -C "$repo_root" -cf - . | tar -C "$stage_root/source" -xf -
cp "$stage_root/build"/libcapsule_nexus_flashrt*.so \
  "$stage_root/source/flashrt_nexus/lib/"
python -m pip wheel --no-build-isolation --no-deps \
  "$stage_root/source" --wheel-dir "$wheel_dir"
