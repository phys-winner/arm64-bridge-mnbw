#!/usr/bin/env python3
"""Fail-closed verification for the GLES 3.x PostFX render-target stub."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_LITTLE_ENDIAN, Cs
from elftools.elf.elffile import ELFFile


START = 0x003CE000
HOOKS = {
    ".hook_publish_effect": (0x001EC6A8, "bl #0x3ce0ac"),
    ".hook_single_channel_float_formats": (0x00212B2C, "beq #0x3ce000"),
    ".hook_float_formats": (0x00212BC8, "beq #0x3ce000"),
    ".hook_argb16": (0x00212C84, "b #0x3ce000"),
    ".hook_render_nodes": (0x001ECE3C, "bl #0x3ce02c"),
}
EXPECTED_RGBA16F_ASM = (
    "mov r0, #0",
    "movw r1, #0x140b",
    "movw r2, #0x1908",
    "str r5, [sp]",
    "stmib sp, {r0, r2}",
    "movw r2, #0x881a",
    "str r1, [sp, #0xc]",
    "mov r1, #0",
    "str r0, [sp, #0x10]",
    "movw r0, #0xde1",
    "b #0x212b60",
)
EXPECTED_RENDER_NODES_ASM = (
    "push {r4, r6, r7, lr}",
    "sub r0, pc, #8",
    "movw r1, #0xe030",
    "movt r1, #0x3c",
    "sub r0, r0, r1",
    "movw r1, #0xf2c4",
    "movt r1, #0x44",
    "ldr r0, [r0, r1]",
    "cmp r0, #0",
    "movwne r1, #0xaf60",
    "strne r0, [fp, r1]",
    "add r4, fp, sl",
    "ldr r4, [r4, #0x858]",
    "ldr r6, [r4, #0x10]",
    "ldr r7, [r4, #0x14]",
    "cmp r7, r6",
    "blo #0x3ce0a0",
    "sub r0, r7, r6",
    "cmp r0, #0x100",
    "bhi #0x3ce0a0",
    "cmp r6, r7",
    "bhs #0x3ce0a0",
    "ldr r2, [r6], #4",
    "cmp r2, #0",
    "beq #0x3ce07c",
    "mov r0, fp",
    "mov r1, r4",
    "bl #0x1eaef8",
    "b #0x3ce07c",
    "pop {r4, r6, r7, lr}",
    "ldr r6, [sp, #0x40]",
    "b #0x1ece40",
)
EXPECTED_PUBLISH_EFFECT_ASM = (
    "push {r0, r1, lr}",
    "sub r0, pc, #8",
    "movw r1, #0xe0b0",
    "movt r1, #0x3c",
    "sub r0, r0, r1",
    "movw r1, #0xf2c4",
    "movt r1, #0x44",
    "ldr r1, [r0, r1]",
    "cmp r1, #0",
    "movwne r0, #0xaf60",
    "strne r1, [fp, r0]",
    "pop {r0, r1, lr}",
    "mov r6, #0",
    "bx lr",
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--bin", required=True, type=Path)
    parser.add_argument("--hook-dir", required=True, type=Path)
    args = parser.parse_args()

    payload = args.bin.read_bytes()
    with args.elf.open("rb") as stream:
        elf = ELFFile(stream)
        section = elf.get_section_by_name(".payload")
        if section is None or int(section["sh_addr"]) != START:
            raise SystemExit("PostFX payload section address mismatch")
        if section.data() != payload:
            raise SystemExit("PostFX raw payload differs from ELF section")
        symtab = elf.get_section_by_name(".symtab")
        expected_symbols = {
            "guest_postfx_rgba16f": START,
            "guest_postfx_render_nodes": START + 0x2C,
            "guest_postfx_publish_effect": START + 0xAC,
        }
        for name, address in expected_symbols.items():
            symbols = symtab.get_symbol_by_name(name) if symtab else None
            if not symbols or int(symbols[0]["st_value"]) != address:
                raise SystemExit(f"PostFX symbol mismatch: {name}")
        for section_name, (address, expected_asm) in HOOKS.items():
            hook_section = elf.get_section_by_name(section_name)
            if hook_section is None or int(hook_section["sh_addr"]) != address:
                raise SystemExit(f"{section_name}: address mismatch")
            hook = hook_section.data()
            hook_path = args.hook_dir / f"{section_name[1:]}.bin"
            if hook_path.read_bytes() != hook:
                raise SystemExit(f"{section_name}: raw hook differs from ELF section")
            instructions = tuple(
                f"{insn.mnemonic} {insn.op_str}".strip()
                for insn in Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN).disasm(
                    hook, address
                )
            )
            if instructions != (expected_asm,):
                raise SystemExit(f"{section_name}: disassembly mismatch: {instructions!r}")

    decoder = Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN)
    actual = tuple(
        f"{insn.mnemonic} {insn.op_str}".strip()
        for insn in decoder.disasm(payload[:0x2C], START)
    )
    if actual != EXPECTED_RGBA16F_ASM:
        raise SystemExit(f"PostFX RGBA16F payload disassembly mismatch: {actual!r}")
    actual_render_nodes = tuple(
        f"{insn.mnemonic} {insn.op_str}".strip()
        for insn in decoder.disasm(payload[0x2C:0xAC], START + 0x2C)
    )
    if actual_render_nodes != EXPECTED_RENDER_NODES_ASM:
        raise SystemExit(
            "PostFX render-node payload disassembly mismatch: "
            f"{actual_render_nodes!r}"
        )
    actual_publish_effect = tuple(
        f"{insn.mnemonic} {insn.op_str}".strip()
        for insn in decoder.disasm(payload[0xAC:], START + 0xAC)
    )
    if actual_publish_effect != EXPECTED_PUBLISH_EFFECT_ASM:
        raise SystemExit(
            "PostFX early effect payload disassembly mismatch: "
            f"{actual_publish_effect!r}"
        )
    if sum(insn.size for insn in decoder.disasm(payload, START)) != len(payload):
        raise SystemExit("PostFX payload contains undecoded bytes")

    print(
        f"postfx-rt-payload: OK start={START:#x} bytes={len(payload)} "
        f"sha256={hashlib.sha256(payload).hexdigest()} hooks={len(HOOKS)}"
    )


if __name__ == "__main__":
    main()
