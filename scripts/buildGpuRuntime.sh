#!/bin/bash
set -euo pipefail

try_commit=13d63df8da2947f833c04025ff5ade187d39ba81
source_dir=$(cd "$(dirname "$0")/.." && pwd -P)
work=${GPU_RUNTIME_WORK:-$source_dir/.gpuRuntime}
out=${1:-$source_dir/qemu-gpu-runtime.arm64.tgz}

rm -rf "$work"
mkdir -p "$work"
git clone -q https://github.com/omacom/try-omarchy.git "$work/try"
git -C "$work/try" checkout -q "$try_commit"
script="$work/try/macos/build-qemu-gpu-runtime.sh"
sed -i '' \
  -e '/^obtain_and_verify "QEMU /d' \
  -e '/^validate_tar_root "QEMU /d' \
  -e 's|^tar -xzf "$qemu_archive" -C "$source_parent"$|mkdir -p "$source_parent/$qemu_root" \&\& git -C "$GPU_RUNTIME_QEMU" archive HEAD \| tar -x -C "$source_parent/$qemu_root"|' \
  -e '/^patch -d "$source_dir" /d' \
  "$script"
grep -q 'GPU_RUNTIME_QEMU' "$script"
! grep -q '^patch -d "$source_dir" ' "$script"
if command -v brew >/dev/null; then
  PATH="$(brew --prefix)/bin:$PATH"
fi
[[ $(command -v python3) != /usr/bin/python3 ]]
GPU_RUNTIME_QEMU=$source_dir "$script"
runtime="$work/try/macos/.build/qemu-gpu-runtime"
test -x "$runtime/bin/qemu-system-aarch64" || ls -R "$runtime/bin"
tar czf "$out" -C "$runtime" .
shasum -a 256 "$out"
