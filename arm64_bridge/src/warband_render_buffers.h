#pragma once

#include "warband_render_commit.h"
#include <array>

// Exact vertex-stream packing from the pinned guest's set_vb_callback.
// The original prefix resolves material/mesh ownership. The original POP
// restores saved GPRs after this allocation-free bulk operation.
namespace warband_render_buffers {

using warband_render::Read;
using warband_render::Valid;
using warband_render::CodeGuard;
inline constexpr CodeGuard kGuards[] = {{0x1F160Cu, 916u, 0xB04431FFu}};
inline constexpr uint32_t kHook = 0x1F1658u, kTail = 0x1F199Cu;
inline constexpr uint32_t kFragmentHash = 0x6D59F7DDu;

template<class T>
inline void Write(uint8_t* p, T value) { std::memcpy(p, &value, sizeof(value)); }

struct Range { uint64_t begin, bytes; };
inline bool Overlap(Range a, Range b) {
    return a.bytes && b.bytes && uint64_t(a.begin) < uint64_t(b.begin) + b.bytes &&
           uint64_t(b.begin) < uint64_t(a.begin) + a.bytes;
}

inline bool Vector(const uint8_t* guest, size_t size, uint32_t owner, uint32_t offset,
                   uint32_t stride, Range& result, uint32_t& count) {
    result.begin = Read<uint32_t>(guest + owner + offset);
    const uint32_t end = Read<uint32_t>(guest + owner + offset + 4);
    if (end < result.begin || end - result.begin > uint32_t(INT32_MAX) ||
        (end - result.begin) % stride ||
        (end != result.begin && !Valid(size, result.begin, end - result.begin))) return false;
    result.bytes = end - result.begin;
    count = result.bytes / stride;
    return true;
}

struct Buffers {
    Range output, corners, vertices, weights;
    uint32_t count, format, stride, color_offset, position_offset;
};

inline bool Prepare(uint8_t* guest, size_t size, uint32_t base, uint32_t mesh, uint32_t corner_owner,
                    uint32_t vertex_owner, uint32_t output, uint32_t format_address,
                    uint32_t shader_flags, Buffers& data) {
    if (!guest || !Valid(size, mesh, 0x30C) || !Valid(size, corner_owner, 0x30C) ||
        !Valid(size, vertex_owner, 0x30C)) return false;
    if (!guest[corner_owner + 0x191] || !guest[vertex_owner + 0x191]) return false;
    if (!Vector(guest, size, corner_owner, 0x154, 72, data.corners, data.count)) return false;
    if (!data.count) return true;
    if (!Valid(size, format_address, 4) || (format_address & 3)) return false;
    data.format = Read<uint32_t>(guest + format_address);
    const bool positions = (data.format & 0x03000000u) != 0;
    data.position_offset = data.format & 0x02000000u ? 12 : 0;
    data.color_offset = (shader_flags & 31) == 24 ? 64 : 60;
    data.stride = positions ? 12 : 0;
    if (data.format & 1) data.stride += 12;
    if (data.format & 2) data.stride += 4;
    if (data.format & 0x40) data.stride += 4;
    if (data.format & 0x10) data.stride += 8;
    if (data.format & 0x100) data.stride += 24;
    if (data.format & 0x1000) data.stride += 32;
    const uint64_t bytes = uint64_t(data.stride) * data.count;
    if (bytes > UINT32_MAX || (bytes && (!Valid(size, output, bytes) || (output & 3)))) return false;
    data.output = {output, static_cast<uint32_t>(bytes)};

    uint32_t vertex_count = 0, weight_count = 0;
    if (positions) {
        if (!Vector(guest, size, vertex_owner, 0x16C, 24, data.vertices, vertex_count) ||
            (data.vertices.begin & 3) || (data.corners.begin & 3)) return false;
        for (uint32_t i = 0; i < data.count; ++i)
            if (Read<uint16_t>(guest + data.corners.begin + i * 72 + 56) >= vertex_count) return false;
    }

    // The body repeats this lookup from the original mesh for bone data. Resolve
    // it once only after rejecting cycles, aliases and all invalid source ranges.
    std::array<uint32_t, 64> owners{};
    size_t owner_count = 0;
    if (data.format & 0x1000) {
        uint32_t owner = mesh;
        for (;;) {
            if (!Valid(size, owner, 0x30C) || owner_count == owners.size()) return false;
            for (size_t i = 0; i < owner_count; ++i) if (owners[i] == owner) return false;
            owners[owner_count++] = owner;
            if (guest[owner + 0x191]) break;
            owner = Read<uint32_t>(guest + owner + 0xA0);
        }
        if (!Vector(guest, size, owner, 0x160, 20, data.weights, weight_count) ||
            weight_count < data.count || (data.weights.begin & 3)) return false;
    }
    if (Overlap(data.output, data.corners) || Overlap(data.output, data.vertices) ||
        Overlap(data.output, data.weights) || Overlap(data.output, {mesh, 0x30C}) ||
        Overlap(data.output, {corner_owner, 0x30C}) ||
        Overlap(data.output, {vertex_owner, 0x30C}) ||
        Overlap(data.output, {format_address, 4}) ||
        // Original mapped executable and RELRO (including GOT) ranges. A
        // destination there must retain the original fault/control behavior.
        Overlap(data.output, {base, 0x3CE0E4}) ||
        Overlap(data.output, {uint64_t(base) + 0x3D0A18, 0x75E8})) return false;
    if ((data.format & (1 | 0x10 | 0x100)) && (data.corners.begin & 3)) return false;
    for (size_t i = 0; i < owner_count; ++i)
        if (Overlap(data.output, {owners[i], 0x30C})) return false;
    return true;
}

// All possible byte values have an exact IEEE binary32 representation. Derive
// the bits with integer arithmetic, preserving guest FPSCR and the host FP state.
inline uint32_t ByteFloatBits(uint8_t value) {
    if (!value) return 0;
    const uint32_t exponent = 31 - static_cast<uint32_t>(__builtin_clz(static_cast<uint32_t>(value)));
    return ((exponent + 127) << 23) | ((uint32_t(value) << (23 - exponent)) & 0x7FFFFFu);
}

inline void Pack(uint8_t* guest, const Buffers& data, uint32_t& s0) {
    if (!data.count) return;
    auto* output = guest + data.output.begin;
    for (uint32_t i = 0; i < data.count; ++i) {
        const auto* corner = guest + data.corners.begin + i * 72;
        if (data.format & 0x03000000u) {
            const uint32_t index = Read<uint16_t>(corner + 56);
            const auto* position = guest + data.vertices.begin + index * 24 + data.position_offset;
            std::memcpy(output, position, 12);
            s0 = Read<uint32_t>(position + 8);
            output += 12;
        }
        if (data.format & 1) {
            std::memcpy(output, corner + 36, 12);
            s0 = Read<uint32_t>(corner + 44);
            output += 12;
        }
        if (data.format & 2) {
            std::memcpy(output, corner + data.color_offset, 4);
            output += 4;
        }
        if (data.format & 0x40) {
            std::memcpy(output, corner + 68, 4);
            output += 4;
        }
        if (data.format & 0x10) {
            std::memcpy(output, corner + 48, 8);
            s0 = Read<uint32_t>(corner + 52);
            output += 8;
        }
        if (data.format & 0x100) {
            std::memcpy(output, corner + 12, 24);
            s0 = Read<uint32_t>(corner + 32);
            output += 24;
        }
        if (data.format & 0x1000) {
            const auto* weights = guest + data.weights.begin + i * 20;
            std::memcpy(output, weights, 16);
            for (uint32_t j = 0; j < 4; ++j) {
                s0 = ByteFloatBits(weights[16 + j]);
                Write(output + 16 + j * 4, s0);
            }
            output += 32;
        }
    }
}

// Hook at 0x1F1658 after the unchanged guest prefix. Success preserves r0/r1/r2,
// all FP status and all VFP registers except the stock clobber of s0. The original
// tail restores r4-r11/PC. Unsupported inputs make no writes and resume the LDR.
inline bool Run(uint8_t* guest, size_t size, uint32_t base, std::array<uint32_t, 16>& r,
                std::array<uint32_t, 64>& s, uint32_t fpscr, uint32_t& nzcv) {
    if (fpscr & 0x00370000u) return false;
    Buffers data{};
    if (!Prepare(guest, size, base, r[0], r[11], r[12], r[1], r[2], r[6], data)) return false;
    Pack(guest, data, s[0]);
    if (data.count) {
        r[3] = static_cast<uint32_t>(data.corners.bytes >> 3);
        r[14] = r[6] & 31;
    }
    nzcv = 0x60000000u;
    return true;
}

}  // namespace warband_render_buffers
