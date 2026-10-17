#!/usr/bin/env python3
"""Compile the real helper and execute the pristine report factory argument path."""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile

from elftools.elf.elffile import ELFFile
from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_ARM, Uc
from unicorn.arm_const import (
    UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC, UC_ARM_REG_LR, UC_ARM_REG_PC,
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R3, UC_ARM_REG_R8,
    UC_ARM_REG_R9, UC_ARM_REG_SP,
)

ROOT = Path(__file__).resolve().parents[3]
PRISTINE = ROOT / "Mount_and_Blade_Warband_v1.071/lib/armeabi-v7a/libMBExpMobile.so"
PAGE = 0x1000


def bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def number(word: int) -> float:
    return struct.unpack("<f", struct.pack("<I", word))[0]


def u32(engine: Uc, address: int) -> int:
    return struct.unpack("<I", engine.mem_read(address, 4))[0]


def vbytes(elf: ELFFile, address: int, size: int) -> bytes:
    for segment in elf.iter_segments():
        start = int(segment["p_vaddr"])
        if segment["p_type"] == "PT_LOAD" and \
                start <= address and address + size <= start + int(segment["p_filesz"]):
            offset = address - start
            return segment.data()[offset:offset + size]
    raise AssertionError(f"file-backed guest range missing at {address:#x}")


def helper_probe(path: Path, scale: float) -> list[int]:
    engine = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        mapped = set()
        for segment in elf.iter_segments():
            if segment["p_type"] != "PT_LOAD":
                continue
            start = int(segment["p_vaddr"])
            end = start + int(segment["p_memsz"])
            for page in range(start & -PAGE, (end + PAGE - 1) & -PAGE, PAGE):
                if page not in mapped:
                    engine.mem_map(page, PAGE)
                    mapped.add(page)
            engine.mem_write(start, segment.data())
        entry = int(elf["e_entry"])
    stack = 0x40000000
    out = stack + 0xC000
    stop = 0x50000000
    engine.mem_map(stack, 0x10000)
    engine.mem_map(stop, PAGE)
    engine.reg_write(UC_ARM_REG_SP, stack + 0x8000)
    engine.reg_write(UC_ARM_REG_R0, bits(scale))
    engine.reg_write(UC_ARM_REG_R1, out)
    engine.reg_write(UC_ARM_REG_LR, stop)
    engine.reg_write(UC_ARM_REG_C1_C0_2, 0x00F00000)
    engine.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
    engine.emu_start(entry, stop, count=1000)
    assert engine.reg_read(UC_ARM_REG_PC) == stop, "helper did not return"
    return list(struct.unpack("<18I", engine.mem_read(out, 18 * 4)))


def report_arguments(elf: ELFFile, words: list[int]) -> tuple[float, float]:
    engine = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    code = 0x0021B000
    engine.mem_map(code, PAGE)
    engine.mem_write(code, vbytes(elf, code, PAGE))
    engine.mem_write(words[3], struct.pack("<I", words[0]))
    engine.mem_write(words[4], struct.pack("<I", words[1]))
    manager = 0x20000000
    font_slot = manager + 0x18000
    font = font_slot + 0x100
    stack = 0x30000000
    sp = stack + 0x8000
    engine.mem_map(manager, 0x20000)
    engine.mem_write(font_slot, struct.pack("<I", font))
    engine.mem_map(stack, 0x10000)
    engine.reg_write(UC_ARM_REG_SP, sp)
    engine.reg_write(UC_ARM_REG_R8, manager)
    engine.reg_write(UC_ARM_REG_R9, manager + 0x19000)
    result: list[tuple[float, float]] = []

    def on_code(current: Uc, address: int, _size: int, _user: object) -> None:
        if address == 0x0021B068:
            # This isolated pre-relocation argument path needs only its font
            # GOT lookup stubbed; every scale/position instruction is pristine.
            current.reg_write(UC_ARM_REG_R1, font_slot)
            current.reg_write(UC_ARM_REG_PC, address + 4)
        elif address == 0x0021B0E0:
            assert current.reg_read(UC_ARM_REG_R1) == font
            assert number(u32(current, current.reg_read(UC_ARM_REG_R3))) == number(bits(-0.03))
            assert number(u32(current, u32(current, sp + 12))) == number(bits(0.02))
            assert number(u32(current, u32(current, sp + 16))) == number(bits(0.8))
            assert number(u32(current, u32(current, sp + 24))) == number(bits(0.3))
            vector = u32(current, sp)
            result.append((number(u32(current, vector)), number(u32(current, vector + 4))))
            current.emu_stop()

    engine.hook_add(UC_HOOK_CODE, on_code)
    engine.emu_start(0x0021B018, 0x0021B0E4, count=300)
    assert len(result) == 1, "pristine report factory was not reached"
    return result[0]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ndk", type=Path, default=Path(os.environ["LOCALAPPDATA"]) /
                        "Android/Sdk/ndk/27.2.12479018")
    args = parser.parse_args()
    compiler = args.ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe"
    assert compiler.is_file(), f"recorded NDK compiler is missing: {compiler}"
    probe_source = """
#include "report_text_scale.h"
extern "C" void report_scale_probe(float scale, uint32_t* out) {
    using namespace warband::report_text;
    const auto words = ScaleWords(scale);
    out[0] = words[0]; out[1] = words[1];
    out[2] = kPostMessageLineVa; out[3] = kScaleMovwVa;
    out[4] = kScaleMovtVa; out[5] = kAuthoredScaleBits;
    for (uint32_t i = 0; i < kContextPreimage.size(); ++i) {
        out[6 + i * 2] = kContextPreimage[i].va;
        out[7 + i * 2] = kContextPreimage[i].word;
    }
}
"""
    with tempfile.TemporaryDirectory(prefix="warband-report-scale-") as temporary:
        source = Path(temporary) / "probe.cpp"
        target = Path(temporary) / "probe.elf"
        source.write_text(probe_source, encoding="utf-8")
        subprocess.run([
            str(compiler), "--target=armv7a-linux-androideabi21", "-marm",
            "-mfloat-abi=softfp", "-mfpu=neon", "-Oz", "-std=c++20", "-Wall",
            "-Wextra", "-Werror", "-ffreestanding", "-fno-exceptions", "-fno-rtti",
            "-fno-pic", "-fno-pie", "-nostdlib", "-Wl,--build-id=none,-no-pie",
            "-Wl,-e,report_scale_probe,-Ttext=0x01000000",
            "-I", str(ROOT / "arm64_bridge/src"), str(source), "-o", str(target),
        ], check=True)
        with PRISTINE.open("rb") as stream:
            elf = ELFFile(stream)
            baseline = helper_probe(target, 1.0)
            symbol = elf.get_section_by_name(".dynsym").get_symbol_by_name(
                "_ZN17rglWindow_manager17post_message_lineERK9rglStringRKiRKb")[0]
            assert int(symbol["st_value"]) == baseline[2]
            assert tuple(struct.unpack("<I", vbytes(elf, va, 4))[0]
                         for va in baseline[3:5]) == tuple(baseline[:2])
            for index in range(6, len(baseline), 2):
                assert struct.unpack("<I", vbytes(elf, baseline[index], 4))[0] == baseline[index + 1]
            for scale in (0.5, 0.75, 1.0, 1.25, 1.5, -1.0, 100.0,
                          float("nan"), float("inf"), float("-inf"), 1.0):
                words = helper_probe(target, scale)
                factor = max(0.5, min(1.5, scale)) if math.isfinite(scale) else 1.0
                expected = number(bits(number(baseline[5]) * factor))
                assert report_arguments(elf, words) == (expected, expected)
            assert helper_probe(target, 1.0)[:2] == baseline[:2], "scale accumulated"
    print("PASS compiled report scale: guarded pristine seam, both glyph axes, "
          "50/75/100/125/150%, clamps/NaN/infinity, authored reset; native positions/wrap unchanged")


if __name__ == "__main__":
    main()
