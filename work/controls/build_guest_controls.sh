#!/usr/bin/env bash
set -eu

ROOT="$(cd -- "$(dirname -- "$0")" && pwd)"
ROOT_WIN="$(cygpath -w "$ROOT")"
: "${ANDROID_NDK_ROOT:?Set ANDROID_NDK_ROOT to an existing Android NDK}"
BIN="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/windows-x86_64/bin"
CLANG="$BIN/armv7a-linux-androideabi21-clang.cmd"
OBJCOPY="$BIN/llvm-objcopy.exe"
READELF="$BIN/llvm-readelf.exe"
test -f "$CLANG" && test -f "$OBJCOPY" && test -f "$READELF" || { echo "Invalid Android NDK" >&2; exit 1; }

OUT="$ROOT/build"
OUT_WIN="$(cygpath -w "$OUT")"
mkdir -p "$OUT"
COMMON='-march=armv7-a -marm -mfloat-abi=softfp -mfpu=neon -Oz -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -fdata-sections -ffunction-sections'
"$CLANG" $COMMON -std=c11 -Wall -Wextra -Werror -c "$ROOT_WIN\\guest_touch_controls.c" -o "$OUT_WIN\\guest_touch_controls.o"
"$CLANG" $COMMON -c "$ROOT_WIN\\guest_base.S" -o "$OUT_WIN\\guest_base.o"
"$CLANG" -nostdlib -Wl,-no-pie,--no-dynamic-linker,--build-id=none,--gc-sections,-T,"$ROOT_WIN\\payload.ld" "$OUT_WIN\\guest_base.o" "$OUT_WIN\\guest_touch_controls.o" -o "$OUT_WIN\\guest_touch_controls.elf"
"$READELF" -s -r "$OUT_WIN\\guest_touch_controls.elf" > "$OUT/guest_touch_controls.readelf.txt"
if "$READELF" -r "$OUT_WIN\\guest_touch_controls.elf" | grep -q 'R_ARM'; then
  echo "Payload still contains relocations" >&2
  exit 1
fi
if "$READELF" -s "$OUT_WIN\\guest_touch_controls.elf" | grep ' UND ' | grep -qv ' 0:'; then
  echo "Payload contains undefined symbols" >&2
  exit 1
fi
"$OBJCOPY" -O binary --only-section=.payload "$OUT_WIN\\guest_touch_controls.elf" "$OUT_WIN\\guest_touch_controls.bin"
python -X utf8 - "$OUT/guest_touch_controls.elf" "$OUT/guest_touch_controls.bin" <<'PY'
from pathlib import Path
import sys
from elftools.elf.elffile import ELFFile
elf_path, binary_path = map(Path, sys.argv[1:])
with elf_path.open('rb') as stream:
    elf = ELFFile(stream)
    payload_section = elf.get_section_by_name('.payload')
    if payload_section is None:
        raise SystemExit('Missing .payload section')
    section_address = payload_section['sh_addr']
    section_bytes = payload_section.data()
    symtab = elf.get_section_by_name('.symtab')
    if symtab is None:
        raise SystemExit('Missing .symtab section')
    symbols = {
        name: symtab.get_symbol_by_name(name)[0]['st_value']
        for name in ('mb_controls_update', 'mb_texture_block_count')
    }
    entry = symbols['mb_controls_update']
binary_bytes = binary_path.read_bytes()
size = binary_path.stat().st_size
if section_address != 0x3CA000:
    raise SystemExit(f'Unexpected .payload address {section_address:#x}; raw injection would be shifted')
if binary_bytes != section_bytes:
    raise SystemExit('Raw payload does not exactly match ELF .payload bytes')
if entry != section_address:
    raise SystemExit(f'Unexpected payload entry {entry:#x}, section starts at {section_address:#x}')
for name, value in symbols.items():
    offset = value - section_address
    runtime_value = 0x3CA000 + offset
    if offset < 0 or offset >= size or runtime_value != value:
        raise SystemExit(
            f'Invalid runtime symbol mapping for {name}: '
            f'ELF={value:#x} section={section_address:#x} runtime={runtime_value:#x}'
        )
if size > 0x6000:
    raise SystemExit(f'Payload too large: {size:#x}')
print(
    f'OK section={section_address:#x} entry={entry:#x} '
    f'texture_helper={symbols["mb_texture_block_count"]:#x} size={size:#x}'
)
PY
