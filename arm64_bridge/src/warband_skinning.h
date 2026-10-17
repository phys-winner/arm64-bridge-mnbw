#pragma once

#include <arm_neon.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

// libMBExpMobile.so SHA256 ABF89B91B8E0C601A4807F5157A2C84701738B7046A3AF61C883296FF0D99A3E.
// Native equivalent of [0x20A760, 0x20A9BC), after the guest prepared its skin palette.
// The original function still owns allocation, palette generation, normalization and dirty flags.
namespace warband_skinning {

constexpr uint32_t kWeightThreshold = 0x3727C5ACu;

template<class T>
inline T Read(const uint8_t* p) {
    T value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

inline void WriteFloat(uint8_t* p, float value) {
    std::memcpy(p, &value, sizeof(value));
}

// These instructions preserve the guest's operand order, individual binary32
// rounding and exception flags; the compiler cannot turn them into fused operations.
inline float Mul(float a, float b) {
    float result;
    asm volatile("fmul %s0, %s1, %s2" : "=w"(result) : "w"(a), "w"(b));
    return result;
}

inline float Add(float a, float b) {
    float result;
    asm volatile("fadd %s0, %s1, %s2" : "=w"(result) : "w"(a), "w"(b));
    return result;
}

inline float32x4_t Mul(float32x4_t a, float32x4_t b) {
    float32x4_t result;
    asm volatile("fmul %0.4s, %1.4s, %2.4s" : "=w"(result) : "w"(a), "w"(b));
    return result;
}

inline float32x4_t Add(float32x4_t a, float32x4_t b) {
    float32x4_t result;
    asm volatile("fadd %0.4s, %1.4s, %2.4s" : "=w"(result) : "w"(a), "w"(b));
    return result;
}

inline bool WeightActive(float weight, uint32_t& nzcv) {
    uint64_t flags;
    uint32_t active;
    float threshold;
    std::memcpy(&threshold, &kWeightThreshold, 4);
    // Original VCMPE followed by BLE: NaNs are skipped and also raise IOC.
    asm volatile("fcmpe %s2, %s3\n\tmrs %0, nzcv\n\tcset %w1, gt"
                 : "=&r"(flags), "=r"(active)
                 : "w"(weight), "w"(threshold) : "cc");
    nzcv = static_cast<uint32_t>(flags);
    return active != 0;
}

inline float32x4_t ReadXYZ(const uint8_t* p) {
    const float x = Read<float>(p);
    const float y = Read<float>(p + 4);
    const float z = Read<float>(p + 8);
    // Duplicate a real operation in lane 3. A zero padding lane would introduce
    // spurious IOC for Inf * 0, which is absent from the three guest components.
    return {x, y, z, z};
}

inline void WriteXYZ(uint8_t* p, float32x4_t v) {
    vst1_f32(reinterpret_cast<float*>(p), vget_low_f32(v));
    const float z = vgetq_lane_f32(v, 2);
    WriteFloat(p + 8, z);
}

inline float32x4_t MixedColumnMul(float32x4_t column, float value) {
    const uint32x4_t x_first = {0xFFFFFFFFu, 0, 0, 0};
    const auto broadcast = vdupq_n_f32(value);
    // The stock instructions multiply matrix * data for x, data * matrix for y/z.
    // Exchanging those operands would change which NaN payload survives.
    return Mul(vbslq_f32(x_first, column, broadcast),
               vbslq_f32(x_first, broadcast, column));
}

inline float32x4_t Transform(float32x4_t x, float32x4_t y, float32x4_t z,
                            float32x4_t value) {
    return Add(Add(MixedColumnMul(x, vgetq_lane_f32(value, 0)),
                   MixedColumnMul(y, vgetq_lane_f32(value, 1))),
               MixedColumnMul(z, vgetq_lane_f32(value, 2)));
}

struct Buffers {
    uint8_t* corners;
    const uint8_t* skin;
    uint8_t* vertices;
    const uint8_t* palette;
    uint32_t count;
};

inline bool Prepare(uint8_t* guest, size_t size, uint32_t mesh, uint32_t palette,
                    Buffers& out) {
    if (!guest || !mesh || uint64_t(mesh) + 0x2D8 > size) return false;
    const auto* object = guest + mesh;
    const uint32_t corner_begin = Read<uint32_t>(object + 0x154);
    const uint32_t corner_end = Read<uint32_t>(object + 0x158);
    if (corner_end < corner_begin || (corner_end - corner_begin) % 72) return false;
    out.count = (corner_end - corner_begin) / 72;
    if (!out.count) return true;

    const uint32_t skin_begin = Read<uint32_t>(object + 0x160);
    const uint32_t skin_end = Read<uint32_t>(object + 0x164);
    const uint32_t vertex_begin = Read<uint32_t>(object + 0x16C);
    const uint32_t vertex_end = Read<uint32_t>(object + 0x170);
    const uint32_t bones = Read<uint32_t>(object + 0x2D4);
    if (skin_end < skin_begin || uint64_t(skin_end - skin_begin) < uint64_t(out.count) * 20 ||
        vertex_end < vertex_begin || (vertex_end - vertex_begin) % 24 ||
        bones == 0 || bones > 28) return false;  // The guest palette allocation is 1792 bytes.

    struct Range { uint64_t begin, bytes; };
    const Range ranges[] = {{mesh, 0x2D8}, {corner_begin, uint64_t(out.count) * 72},
                            {skin_begin, uint64_t(out.count) * 20},
                            {vertex_begin, vertex_end - vertex_begin}, {palette, bones * 64}};
    for (size_t i = 0; i < 5; ++i) {
        if (!ranges[i].begin || !ranges[i].bytes || ranges[i].begin + ranges[i].bytes > size)
            return false;
        for (size_t j = 0; j < i; ++j)
            if (ranges[i].begin < ranges[j].begin + ranges[j].bytes &&
                ranges[j].begin < ranges[i].begin + ranges[i].bytes) return false;
    }
    out.corners = guest + corner_begin;
    out.skin = guest + skin_begin;
    out.vertices = guest + vertex_begin;
    out.palette = guest + palette;
    const uint32_t vertices = (vertex_end - vertex_begin) / 24;
    // Validate every potentially accessed index before writing any output. Integer
    // comparisons recognize positive finite/Inf weights > threshold without touching FPSR.
    for (uint32_t i = 0; i < out.count; ++i) {
        if (Read<uint16_t>(out.corners + i * 72 + 56) >= vertices) return false;
        const auto* skin = out.skin + i * 20;
        for (uint32_t j = 0; j < 4; ++j) {
            const uint32_t bits = Read<uint32_t>(skin + j * 4);
            const bool nan = (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu);
            if (!(bits & 0x80000000u) && !nan && bits > kWeightThreshold && skin[16 + j] >= bones)
                return false;
        }
    }
    return true;
}

inline void Scalar(const Buffers& data, uint32_t& nzcv) {
    for (uint32_t i = 0; i < data.count; ++i) {
        auto* corner = data.corners + i * 72;
        auto* vertex = data.vertices + Read<uint16_t>(corner + 56) * 24;
        const auto* skin = data.skin + i * 20;
        std::memset(vertex + 12, 0, 12);
        std::memset(corner + 36, 0, 12);
        for (uint32_t j = 0; j < 4; ++j) {
            const float weight = Read<float>(skin + j * 4);
            if (!WeightActive(weight, nzcv)) continue;
            const auto* matrix = data.palette + skin[16 + j] * 64;
            float point[3], normal[3];
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const float vx = Read<float>(vertex);
                const float vy = Read<float>(vertex + 4);
                const float vz = Read<float>(vertex + 8);
                const float mx = Read<float>(matrix + axis * 4);
                const float my = Read<float>(matrix + 16 + axis * 4);
                const float mz = Read<float>(matrix + 32 + axis * 4);
                const float x = axis == 0 ? Mul(mx, vx) : Mul(vx, mx);
                const float y = axis == 0 ? Mul(my, vy) : Mul(vy, my);
                const float z = axis == 0 ? Mul(mz, vz) : Mul(vz, mz);
                point[axis] = Add(Read<float>(matrix + 48 + axis * 4), Add(Add(x, y), z));
            }
            for (uint32_t axis = 0; axis < 3; ++axis)
                WriteFloat(vertex + 12 + axis * 4,
                           Add(Mul(weight, point[axis]), Read<float>(vertex + 12 + axis * 4)));
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const float nx = Read<float>(corner);
                const float ny = Read<float>(corner + 4);
                const float nz = Read<float>(corner + 8);
                const float mx = Read<float>(matrix + axis * 4);
                const float my = Read<float>(matrix + 16 + axis * 4);
                const float mz = Read<float>(matrix + 32 + axis * 4);
                const float x = axis == 0 ? Mul(mx, nx) : Mul(nx, mx);
                const float y = axis == 0 ? Mul(my, ny) : Mul(ny, my);
                const float z = axis == 0 ? Mul(mz, nz) : Mul(nz, mz);
                normal[axis] = Add(Add(x, y), z);
            }
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const float weighted = axis == 2 ? Mul(weight, normal[axis]) : Mul(normal[axis], weight);
                WriteFloat(corner + 36 + axis * 4,
                           Add(weighted, Read<float>(corner + 36 + axis * 4)));
            }
        }
    }
}

inline void Neon(const Buffers& data, uint32_t& nzcv) {
    const uint32x4_t normal_first = {0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0};
    for (uint32_t i = 0; i < data.count; ++i) {
        auto* corner = data.corners + i * 72;
        auto* vertex = data.vertices + Read<uint16_t>(corner + 56) * 24;
        const auto* skin = data.skin + i * 20;
        auto point_sum = vdupq_n_f32(0.0f);
        auto normal_sum = vdupq_n_f32(0.0f);
        WriteXYZ(vertex + 12, point_sum);
        WriteXYZ(corner + 36, normal_sum);
        const auto point = ReadXYZ(vertex);
        const auto normal = ReadXYZ(corner);
        for (uint32_t j = 0; j < 4; ++j) {
            const float weight = Read<float>(skin + j * 4);
            if (!WeightActive(weight, nzcv)) continue;
            const auto* matrix = data.palette + skin[16 + j] * 64;
            const auto x = ReadXYZ(matrix);
            const auto y = ReadXYZ(matrix + 16);
            const auto z = ReadXYZ(matrix + 32);
            const auto w = vdupq_n_f32(weight);
            const auto transformed_point = Add(ReadXYZ(matrix + 48), Transform(x, y, z, point));
            point_sum = Add(Mul(w, transformed_point), point_sum);
            WriteXYZ(vertex + 12, point_sum);
            const auto transformed_normal = Transform(x, y, z, normal);
            const auto weighted_normal = Mul(vbslq_f32(normal_first, transformed_normal, w),
                                             vbslq_f32(normal_first, w, transformed_normal));
            normal_sum = Add(weighted_normal, normal_sum);
            WriteXYZ(corner + 36, normal_sum);
        }
    }
}

// False leaves guest memory and the guest/host FP state unchanged; the caller must
// resume the original guest fragment through its saved-prologue stub in that case.
inline bool Run(uint8_t* guest, size_t size, uint32_t mesh, uint32_t palette,
                uint32_t fpscr, uint32_t& result_fpscr, bool neon = true) {
    if (fpscr & 0x00370000u) return false;  // Legacy VFP vector length/stride needs the guest path.
    Buffers data{};
    if (!Prepare(guest, size, mesh, palette, data)) return false;
    result_fpscr = fpscr;
    if (!data.count) return true;
    uint64_t previous_fpcr, previous_fpsr;
    const uint64_t fpcr = fpscr & 0x07C00000u;
    asm volatile("mrs %0, fpcr\n\tmrs %1, fpsr\n\tmsr fpcr, %2\n\tmsr fpsr, xzr"
                 : "=&r"(previous_fpcr), "=&r"(previous_fpsr) : "r"(fpcr) : "memory");
    uint32_t nzcv = fpscr & 0xF0000000u;
    if (neon) Neon(data, nzcv); else Scalar(data, nzcv);
    uint64_t raised;
    asm volatile("mrs %0, fpsr\n\tmsr fpcr, %1\n\tmsr fpsr, %2"
                 : "=&r"(raised) : "r"(previous_fpcr), "r"(previous_fpsr) : "memory");
    result_fpscr = (fpscr & ~0xF0000000u) | nzcv | (static_cast<uint32_t>(raised) & 0x0800009Fu);
    return true;
}

}  // namespace warband_skinning
