#!/bin/bash
set -euo pipefail

try_commit=13d63df8da2947f833c04025ff5ade187d39ba81
source_dir=$(cd "$(dirname "$0")/.." && pwd -P)
work=${GPU_RUNTIME_WORK:-$source_dir/.gpuRuntime}
out=${1:-$source_dir/qemu-gpu-runtime.arm64.tgz}

rm -rf "$work"
mkdir -p "$work/tools"
git clone -q https://github.com/omacom/try-omarchy.git "$work/try"
git -C "$work/try" checkout -q "$try_commit"
script="$work/try/macos/build-qemu-gpu-runtime.sh"
sed -i '' \
  -e '/^obtain_and_verify "QEMU /d' \
  -e '/^validate_tar_root "QEMU /d' \
  -e 's|^tar -xzf "$qemu_archive" -C "$source_parent"$|mkdir -p "$source_parent/$qemu_root" \&\& git -C "$GPU_RUNTIME_QEMU" archive HEAD \| tar -x -C "$source_parent/$qemu_root"|' \
  -e '/^patch -d "$source_dir" /d' \
  -e 's|^      --enable-system \\$|      --enable-system --enable-tools \\|' \
  -e 's|-C "$build_dir" qemu-system-aarch64$|-C "$build_dir" qemu-system-aarch64 qemu-img|' \
  -e 's|^log "Relocating, capability-gating|install -m 0755 "$build_dir/qemu-img" "$GPU_RUNTIME_TOOLS/"; &|' \
  "$script"
for marker in GPU_RUNTIME_QEMU GPU_RUNTIME_TOOLS 'enable-tools' 'qemu-system-aarch64 qemu-img'; do
  grep -q -- "$marker" "$script"
done
! grep -q '^patch -d "$source_dir" ' "$script"
if command -v brew >/dev/null; then
  PATH="$(brew --prefix)/bin:$PATH"
fi
[[ $(command -v python3) != /usr/bin/python3 ]]
GPU_RUNTIME_QEMU=$source_dir GPU_RUNTIME_TOOLS=$work/tools "$script"
runtime="$work/try/macos/.build/qemu-gpu-runtime"
img="$runtime/bin/qemu-img"
install -m 0755 "$work/tools/qemu-img" "$img"
otool -L "$img" | awk 'NR > 1 { print $1 }' | while read -r dependency; do
  leaf=${dependency##*/}
  case $dependency in
    /usr/lib/* | /System/*) ;;
    *)
      test -f "$runtime/lib/$leaf"
      install_name_tool -change "$dependency" "@executable_path/../lib/$leaf" "$img"
      ;;
  esac
done
codesign --force --sign - "$img"
"$img" --version
tar czf "$out" -C "$runtime" .
shasum -a 256 "$out"
