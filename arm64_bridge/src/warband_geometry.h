#pragma once

#include "warband_skinning.h"
#include <array>
#include <limits>

// libMBExpMobile.so SHA256 ABF89B91B8E0C601A4807F5157A2C84701738B7046A3AF61C883296FF0D99A3E.
// Capacity-fitting equivalent of render_mesh_to_mesh [0x1DF1A8,0x1DF668).
// The original prefix retains copy-on-write/allocation; the original tail sets dirty flags.
namespace warband_geometry {

using warband_skinning::Read;
using warband_skinning::ReadXYZ;
using warband_skinning::WriteXYZ;
using warband_skinning::Add;
using warband_skinning::Mul;
using warband_skinning::Transform;

template<class T>
inline void Write(uint8_t* p, T value) {
    std::memcpy(p, &value, sizeof(value));
}

struct Range {
    uint64_t begin, bytes;
};

inline bool Overlap(Range a, Range b) {
    return a.bytes && b.bytes && a.begin < b.begin + b.bytes && b.begin < a.begin + a.bytes;
}

struct Vector {
    uint32_t begin, end, capacity, count, bytes;
};

inline bool PrepareVector(const uint8_t* object, uint32_t offset, uint32_t stride,
                          size_t size, Vector& out) {
    out.begin = Read<uint32_t>(object + offset);
    out.end = Read<uint32_t>(object + offset + 4);
    out.capacity = Read<uint32_t>(object + offset + 8);
    if (out.end < out.begin || out.capacity < out.end ||
        (out.end - out.begin) % stride || (out.capacity - out.begin) % stride ||
        out.end - out.begin > uint32_t(INT32_MAX) || out.capacity > size ||
        (!out.begin && out.capacity)) return false;
    out.bytes = out.end - out.begin;
    out.count = out.bytes / stride;
    return true;
}

struct Buffers {
    uint8_t* destination;
    const uint8_t* frame;
    Vector source_vertices, source_corners, source_faces;
    Vector destination_vertices, destination_corners, destination_faces;
    uint32_t color;
};

inline bool Prepare(uint8_t* guest, size_t size, uint32_t destination,
                    uint32_t vertex_owner, uint32_t corner_owner,
                    uint32_t source, uint32_t frame, Buffers& out) {
    auto object_valid = [size](uint32_t address) {
        return address && uint64_t(address) + 0x234 <= size;
    };
    if (!guest || !object_valid(destination) || !object_valid(vertex_owner) ||
        !object_valid(corner_owner) || !object_valid(source) ||
        !frame || uint64_t(frame) + 64 > size) return false;
    if (!guest[destination + 0x190] || !guest[destination + 0x191] ||
        !guest[vertex_owner + 0x191] || !guest[corner_owner + 0x191]) return false;

    // Only the face owner remains unresolved at the hook. Bound cycles and long
    // chains without changing their guest behavior: either case takes the fallback.
    std::array<uint32_t, 64> owners{};
    uint32_t owner = source;
    size_t owner_count = 0;
    for (;;) {
        if (!object_valid(owner) || owner_count == owners.size()) return false;
        for (size_t i = 0; i < owner_count; ++i) if (owners[i] == owner) return false;
        owners[owner_count++] = owner;
        if (guest[owner + 0x190]) break;
        owner = Read<uint32_t>(guest + owner + 0xA0);
    }

    out.destination = guest + destination;
    out.frame = guest + frame;
    out.color = Read<uint32_t>(guest + source + 0x144);
    if (!PrepareVector(out.destination, 0x16C, 24, size, out.destination_vertices) ||
        !PrepareVector(out.destination, 0x154, 72, size, out.destination_corners) ||
        !PrepareVector(out.destination, 0x184, 6, size, out.destination_faces) ||
        !PrepareVector(guest + vertex_owner, 0x16C, 24, size, out.source_vertices) ||
        !PrepareVector(guest + corner_owner, 0x154, 72, size, out.source_corners) ||
        !PrepareVector(guest + owner, 0x184, 6, size, out.source_faces)) return false;

    const Vector* writes[] = {&out.destination_vertices, &out.destination_corners, &out.destination_faces};
    const Vector* reads[] = {&out.source_vertices, &out.source_corners, &out.source_faces};
    const Range objects[] = {{destination, 0x234}, {vertex_owner, 0x234},
                             {corner_owner, 0x234}, {source, 0x234}, {frame, 64}};
    for (size_t i = 0; i < 3; ++i) {
        if (uint64_t(writes[i]->end) + reads[i]->bytes > writes[i]->capacity) return false;
        const Range capacity{writes[i]->begin, writes[i]->capacity - writes[i]->begin};
        for (size_t j = 0; j < i; ++j)
            if (Overlap(capacity, {writes[j]->begin, writes[j]->capacity - writes[j]->begin})) return false;
        for (Range object : objects) if (Overlap(capacity, object)) return false;
        for (size_t j = 0; j < owner_count; ++j)
            if (Overlap(capacity, {owners[j], 0x234})) return false;
        for (const Vector* read : reads)
            if (Overlap(capacity, {read->begin, read->bytes})) return false;
    }
    // The prefix snapshots destination counts. Self-append or a read array inside
    // its object would make the original loop observe evolving metadata instead.
    const Range destination_object{destination, 0x234};
    if (destination == vertex_owner || destination == corner_owner || destination == source)
        return false;
    for (size_t j = 0; j < owner_count; ++j)
        if (destination == owners[j]) return false;
    for (const Vector* read : reads)
        if (Overlap(destination_object, {read->begin, read->bytes})) return false;
    for (Range object : objects)
        if (object.begin != destination && Overlap(destination_object, object)) return false;
    for (size_t j = 0; j < owner_count; ++j)
        if (Overlap(destination_object, {owners[j], 0x234})) return false;
    return true;
}

inline float32x4_t Div(float32x4_t a, float32x4_t b) {
    float32x4_t result;
    asm volatile("fdiv %0.4s, %1.4s, %2.4s" : "=w"(result) : "w"(a), "w"(b));
    return result;
}

inline float32x4_t ColorFloat(uint32_t color) {
    const uint32x4_t channels = {color & 255, (color >> 8) & 255, (color >> 16) & 255, color >> 24};
    return vcvtq_f32_u32(channels);  // All byte values are exactly representable.
}

inline uint32_t Color(float32x4_t factor, uint32_t color) {
    auto value = Div(ColorFloat(color), vdupq_n_f32(255.0f));
    value = Mul(Mul(factor, value), vdupq_n_f32(256.0f));
    uint32x4_t integer;
    // Stock VCVT.u32.f32 uses RTZ regardless of FPSCR.RMode.
    asm volatile("fcvtzu %0.4s, %1.4s" : "=w"(integer) : "w"(value));
    integer = vminq_u32(integer, vdupq_n_u32(255));
    return vgetq_lane_u32(integer, 0) | (vgetq_lane_u32(integer, 1) << 8) |
           (vgetq_lane_u32(integer, 2) << 16) | (vgetq_lane_u32(integer, 3) << 24);
}

inline void Append(uint8_t* guest, const Buffers& data) {
    const auto x = ReadXYZ(data.frame);
    const auto y = ReadXYZ(data.frame + 16);
    const auto z = ReadXYZ(data.frame + 32);
    const auto origin = ReadXYZ(data.frame + 48);
    for (uint32_t i = 0; i < data.source_vertices.count; ++i) {
        const auto value = ReadXYZ(guest + data.source_vertices.begin + i * 24 + 12);
        const auto transformed = Add(origin, Transform(x, y, z, value));
        auto* output = guest + data.destination_vertices.end + i * 24;
        WriteXYZ(output, transformed);
        WriteXYZ(output + 12, transformed);
    }
    Write(data.destination + 0x170, data.destination_vertices.end + data.source_vertices.bytes);

    if (data.source_corners.count) {
        const auto factor = Div(ColorFloat(data.color), vdupq_n_f32(255.0f));
        for (uint32_t i = 0; i < data.source_corners.count; ++i) {
            const auto* source = guest + data.source_corners.begin + i * 72;
            auto* output = guest + data.destination_corners.end + i * 72;
            std::memcpy(output, source, 72);
            Write(output + 56, static_cast<uint16_t>(Read<uint16_t>(source + 56) + data.destination_vertices.count));
            WriteXYZ(output + 36, Transform(x, y, z, ReadXYZ(source + 36)));
            Write(output + 60, Color(factor, Read<uint32_t>(source + 60)));
        }
    }
    Write(data.destination + 0x158, data.destination_corners.end + data.source_corners.bytes);

    for (uint32_t i = 0; i < data.source_faces.count; ++i) {
        const auto* source = guest + data.source_faces.begin + i * 6;
        auto* output = guest + data.destination_faces.end + i * 6;
        for (uint32_t index = 0; index < 3; ++index)
            Write(output + index * 2, static_cast<uint16_t>(Read<uint16_t>(source + index * 2) +
                                                          data.destination_corners.count));
    }
    Write(data.destination + 0x188, data.destination_faces.end + data.source_faces.bytes);
}

// False is safe before any writes or host FP changes. On success the hook handler
// additionally stores destination at guest [sp+0x14], then branches to 0x1DF668.
inline bool Run(uint8_t* guest, size_t size, uint32_t destination,
                uint32_t vertex_owner, uint32_t corner_owner, uint32_t source,
                uint32_t frame, uint32_t fpscr, uint32_t& result_fpscr) {
    if (fpscr & 0x00370000u) return false;
    Buffers data{};
    if (!Prepare(guest, size, destination, vertex_owner, corner_owner, source, frame, data)) return false;
    uint64_t previous_fpcr, previous_fpsr;
    const uint64_t fpcr = fpscr & 0x07C00000u;
    asm volatile("mrs %0, fpcr\n\tmrs %1, fpsr\n\tmsr fpcr, %2\n\tmsr fpsr, xzr"
                 : "=&r"(previous_fpcr), "=&r"(previous_fpsr) : "r"(fpcr) : "memory");
    Append(guest, data);
    uint64_t raised;
    asm volatile("mrs %0, fpsr\n\tmsr fpcr, %1\n\tmsr fpsr, %2"
                 : "=&r"(raised) : "r"(previous_fpcr), "r"(previous_fpsr) : "memory");
    result_fpscr = fpscr | (static_cast<uint32_t>(raised) & 0x0800009Fu);
    return true;
}

}  // namespace warband_geometry
