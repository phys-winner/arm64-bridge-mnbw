#!/usr/bin/env bash
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
build="$here/build"
ndk_root="${ANDROID_NDK_ROOT:-}"
if [[ -z "$ndk_root" ]]; then
  echo "ANDROID_NDK_ROOT is required by build_payload.sh; on Windows use build_payload.ps1 for auto-discovery." >&2
  exit 2
fi

tool_bin="$ndk_root/toolchains/llvm/prebuilt/windows-x86_64/bin"
clang="$tool_bin/armv7a-linux-androideabi21-clang.cmd"
clang_aarch64="$tool_bin/aarch64-linux-android21-clang++.cmd"
objcopy="$tool_bin/llvm-objcopy.exe"
mkdir -p "$build"

common=(
  -march=armv7-a -marm -mfloat-abi=softfp -mfpu=neon -Oz
  -ffreestanding -fno-builtin -fno-stack-protector
  -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-exceptions
  -fno-pic -fno-pie -fno-jump-tables -fdata-sections -ffunction-sections
)

"$clang" "${common[@]}" -c "$here/guest_settings_entry.S" -o "$build/guest_settings_entry.o"
"$clang" "${common[@]}" -std=c11 -Wall -Wextra -Werror \
  -c "$here/guest_settings_tab.c" -o "$build/guest_settings_tab.o"
"$clang_aarch64" -std=c++20 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
  -c "$here/bridge_protocol_compile_test.cpp" \
  -o "$build/bridge_protocol_compile_test.o"
"$clang" "${common[@]}" -nostdlib -Wl,-no-pie,--build-id=none,--gc-sections \
  "-Wl,-Map,$build/guest_settings_tab.map" "-Wl,-T,$here/payload.ld" \
  "$build/guest_settings_entry.o" "$build/guest_settings_tab.o" \
  -o "$build/guest_settings_tab.elf"
"$objcopy" --only-section=.payload -O binary \
  "$build/guest_settings_tab.elf" "$build/guest_settings_tab.bin"

python "$here/verify_payload.py" \
  --elf "$build/guest_settings_tab.elf" \
  --bin "$build/guest_settings_tab.bin" \
  --manifest "$build/manifest.json" \
  --guest "$repo/Mount_and_Blade_Warband_v1.071/lib/armeabi-v7a/libMBExpMobile.so" \
  --guest "$repo/work/ida/ui_batch_20260912/libMBExpMobile.so"
python "$here/test_report_text_scale.py" --ndk "$ndk_root"
python "$here/test_integration.py" \
  --elf "$build/guest_settings_tab.elf" \
  --bin "$build/guest_settings_tab.bin" \
  --guest "$repo/Mount_and_Blade_Warband_v1.071/lib/armeabi-v7a/libMBExpMobile.so" \
  --guest "$repo/work/ida/ui_batch_20260912/libMBExpMobile.so"
