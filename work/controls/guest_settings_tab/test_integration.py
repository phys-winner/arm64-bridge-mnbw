#!/usr/bin/env python3
"""In-memory patcher composition proof; never writes a guest .so."""

from __future__ import annotations

import argparse
import io
import struct
from pathlib import Path

from elftools.elf.elffile import ELFFile

import verify_payload as proof


def phdr_field_offsets(data: bytes) -> tuple[int, int, int]:
    e_phoff = proof.u32(data, 0x1C)
    e_phentsize = struct.unpack_from("<H", data, 0x2A)[0]
    e_phnum = struct.unpack_from("<H", data, 0x2C)[0]
    load_indices: list[int] = []
    for index in range(e_phnum):
        offset = e_phoff + index * e_phentsize
        if proof.u32(data, offset) == 1:  # PT_LOAD
            load_indices.append(index)
    if len(load_indices) < 2 or e_phentsize != 32:
        raise AssertionError("unexpected ELF32 program-header table")
    rx = e_phoff + load_indices[0] * e_phentsize
    rw = e_phoff + load_indices[1] * e_phentsize
    return rx + 16, rx + 20, rw + 20


def ranges_cover(index: int, ranges: list[tuple[int, int]]) -> bool:
    return any(start <= index < end for start, end in ranges)


def simulate(path: Path, payload: bytes, payload_symbols: dict[str, int]) -> None:
    original = path.read_bytes()
    guest = proof.guest_proof(path)
    data = bytearray(original)
    end = proof.PAYLOAD_VA + len(payload)
    if any(data[proof.PAYLOAD_VA:end]):
        raise AssertionError(f"{path}: target payload bytes are not zero")

    rx_filesz_off, rx_memsz_off, rw_memsz_off = phdr_field_offsets(data)
    old_rx_filesz = proof.u32(data, rx_filesz_off)
    old_rx_memsz = proof.u32(data, rx_memsz_off)
    old_rw_memsz = proof.u32(data, rw_memsz_off)
    if old_rx_filesz != old_rx_memsz or old_rx_filesz > proof.PAYLOAD_VA:
        raise AssertionError(f"{path}: incompatible RX extension preimage")
    if any(data[old_rx_filesz:proof.PAYLOAD_VA]):
        raise AssertionError(f"{path}: non-zero bytes in RX extension gap")
    required_rw_memsz = proof.STATE_END_VA - proof.NEXT_LOAD_VA
    if old_rw_memsz > required_rw_memsz:
        raise AssertionError(f"{path}: RW PT_LOAD already extends beyond reserved state")

    hook_patches: list[tuple[str, int, bytes]] = []
    for name, address, preimage, symbol_name in proof.HOOK_SPECS:
        if bytes(data[address:address + len(preimage)]) != preimage:
            raise AssertionError(f"{path}: {name} hook preimage drift")
        hook_patches.append((
            name,
            address,
            proof.arm_branch(address, payload_symbols[symbol_name], link=False),
        ))
    data[proof.PAYLOAD_VA:end] = payload
    for _name, address, patch in hook_patches:
        data[address:address + len(patch)] = patch
    struct.pack_into("<I", data, rx_filesz_off, end)
    struct.pack_into("<I", data, rx_memsz_off, end)
    struct.pack_into("<I", data, rw_memsz_off, required_rw_memsz)

    allowed = [
        (proof.PAYLOAD_VA, end),
        *((address, address + len(patch))
          for _name, address, patch in hook_patches),
        (rx_filesz_off, rx_filesz_off + 4),
        (rx_memsz_off, rx_memsz_off + 4),
        (rw_memsz_off, rw_memsz_off + 4),
    ]
    unexpected = [
        index
        for index, (before, after) in enumerate(zip(original, data))
        if before != after and not ranges_cover(index, allowed)
    ]
    if unexpected:
        raise AssertionError(f"{path}: unexpected changed byte at {unexpected[0]:#x}")

    patched_elf = ELFFile(io.BytesIO(data))
    loads = [s for s in patched_elf.iter_segments() if s["p_type"] == "PT_LOAD"]
    if int(loads[0]["p_filesz"]) != end or int(loads[0]["p_memsz"]) != end:
        raise AssertionError("patched RX PT_LOAD does not end at payload end")
    if int(loads[1]["p_memsz"]) != required_rw_memsz:
        raise AssertionError("patched RW PT_LOAD does not cover state block")
    if data[proof.PAYLOAD_VA:end] != payload:
        raise AssertionError("payload copy mismatch")
    for name, address, patch in hook_patches:
        if proof.one_instruction(patch, address)[0] != "b":
            raise AssertionError(f"{name} replacement is not unconditional ARM B")
    if any(data[proof.DIRECT_LOOK_STUB_VA:proof.DIRECT_LOOK_STUB_END]):
        raise AssertionError("integration touched the runtime direct-look stub")

    print(
        f"PASS in-memory integration {guest['role']}: "
        f"RX {old_rx_filesz:#x}->{end:#x}, RW {old_rw_memsz:#x}->{required_rw_memsz:#x}, "
        f"changed-bytes confined to {len(allowed)} declared ranges"
    )


def model_checks() -> None:
    ranges = ((0.5, 1.8), (0.2, 4.0), (0.0, 0.06), (0.6, 1.4), (0.2, 1.0), (0.0, 3.0))
    for minimum, maximum in ranges:
        for normalized in (0.0, 0.125, 0.5, 0.875, 1.0):
            value = minimum + normalized * (maximum - minimum)
            round_trip = (value - minimum) / (maximum - minimum)
            if abs(round_trip - normalized) > 1.0e-6:
                raise AssertionError("normalization model is not reversible")

    options_id = 13
    custom_id = 14
    controls_id = 15
    previous_events = {102, 105}
    next_events = {103, 104}

    def controls_dispatch(
        event_code: int, tab_event: int, requested: int, has_tab: bool = True
    ) -> tuple[str, int, bool]:
        if not has_tab:
            return "epilogue", requested, False
        if requested == custom_id or event_code in next_events:
            return "event", options_id, True
        return ("event" if tab_event == 1 else "epilogue"), requested, False

    def options_custom_nav(custom_active: bool, event_code: int) -> str:
        if not custom_active:
            return "replay"
        if event_code in previous_events:
            return "controls"
        if event_code in next_events:
            return "consume"
        return "replay"

    def options_tab_dispatch(
        state_valid: bool, custom_active: bool, requested: int
    ) -> tuple[bool, int, bool]:
        if requested == custom_id:
            if state_valid:
                return True, options_id, True
            return True, options_id, False
        if not state_valid:
            return False, requested, False
        if custom_active and requested == options_id:
            return True, options_id, False
        return False, requested, False

    # Both hosts append the alias only after their pristine entries.
    if [options_id, controls_id, custom_id] != [13, 15, 14]:
        raise AssertionError("tab header order model mismatch")
    if 0x2B4 + 1 * 4 != 0x2B8:
        raise AssertionError("pending token is not modeled at reserved[1]")

    # Forward cycle: General -> Controls -> Options-hosted custom.  The
    # Controls dispatcher rewrites 14/next to 13 and carries a one-shot token.
    active_trace = [options_id]
    active_trace.append(controls_id)  # pristine Options RB path
    branch, requested, pending = controls_dispatch(103, 0, controls_id)
    if (branch, requested, pending) != ("event", options_id, True):
        raise AssertionError("Controls RB redirect model mismatch")
    active_trace.append(requested)
    custom_active = pending  # Options activate consumes pending after building.
    if not custom_active or custom_id in active_trace:
        raise AssertionError("forward cycle exposed real window 14")

    # Reverse cycle: custom LB -> Controls, then pristine Controls LB -> General.
    if options_custom_nav(custom_active, 102) != "controls":
        raise AssertionError("custom LB model mismatch")
    active_trace.extend((controls_id, options_id))
    if custom_id in active_trace:
        raise AssertionError("reverse cycle exposed real window 14")
    if options_custom_nav(True, 103) != "consume":
        raise AssertionError("custom right-edge RB was not consumed")

    # Direct custom taps are local on Options and redirected through 13 on
    # Controls.  Even invalid Options state must consume 14 and repair to 13.
    branch, requested, pending = controls_dispatch(0, 1, custom_id)
    if (branch, requested, pending) != ("event", options_id, True):
        raise AssertionError("Controls direct-custom redirect model mismatch")
    if options_tab_dispatch(True, False, custom_id) != (True, options_id, True):
        raise AssertionError("Options direct-custom local dispatch model mismatch")
    if options_tab_dispatch(False, False, custom_id) != (True, options_id, False):
        raise AssertionError("Options ID14 fail-safe model mismatch")
    if controls_dispatch(103, 0, controls_id, has_tab=False)[0] != "epilogue":
        raise AssertionError("missing Controls tab did not fail closed")

    print(
        "PASS protocol model: normalization and tab navigation; "
        "layout editing is no longer exposed in Additional settings"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--guest", type=Path, action="append", required=True)
    args = parser.parse_args()

    payload_info = proof.payload_proof(args.elf, args.bin)
    payload = args.bin.read_bytes()
    symbols = {name: int(value) for name, value in payload_info["symbols"].items()}
    model_checks()
    for guest in args.guest:
        simulate(guest, payload, symbols)


if __name__ == "__main__":
    main()
