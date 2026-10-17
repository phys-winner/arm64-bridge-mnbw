#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Exact warmed CommitChanges of libMBExpMobile.so SHA256
// ABF89B91B8E0C601A4807F5157A2C84701738B7046A3AF61C883296FF0D99A3E.
// Every upload uses its existing registered compatibility GL handler. Cold
// uniform queries and unsupported buffers resume the original guest code.
namespace warband_render {

template<class T>
inline T Read(const uint8_t* p) {
    T result;
    std::memcpy(&result, p, sizeof(result));
    return result;
}

inline bool Valid(size_t size, uint64_t address, uint64_t bytes) {
    return address != 0 && address <= size && bytes <= size - address;
}

inline uint32_t CompareZero(uint32_t value) {
    return value == 0 ? 0x60000000u : 0x20000000u | (value & 0x80000000u);
}

enum class Uniform : uint8_t { Uniform1i, Uniform1f, Uniform4fv, UniformMatrix4fv, Uniform1iv };
struct Binding {
    const char* name;
    uint32_t plt, got;
};
inline constexpr std::array<Binding, 5> kBindings{{
    {"glUniform1i", 0x51718u, 0x3D7480u},
    {"glUniform1f", 0x50FB0u, 0x3D7208u},
    {"glUniform4fv", 0x5152Cu, 0x3D73DCu},
    {"glUniformMatrix4fv", 0x51088u, 0x3D7250u},
    {"glUniform1iv", 0x511F0u, 0x3D72C8u},
}};
struct CodeGuard { uint32_t offset, bytes, hash; };
// Hashes are filled and checked by render-commit-audit.py against the pinned ELF.
inline constexpr CodeGuard kCommitGuards[] = {
    {0x1E35A8u, 6196u, 0x58B1B218u},
    {0x51718u, 12u, 0x36FE02A4u},
    {0x50FB0u, 12u, 0x41BA9346u},
    {0x5152Cu, 12u, 0x4FD4E525u},
    {0x51088u, 12u, 0x3D7AD22Bu},
    {0x511F0u, 12u, 0x66A300C0u},
};
inline constexpr uint32_t kCommitFragmentHash = 0xBA29B546u;
inline constexpr uint32_t kCommitHook = 0x1E35B0u, kCommitTail = 0x1E4C68u;
inline constexpr uint32_t kGraphicsContextGot = 0x3D7FD0u;

struct Upload {
    uint16_t location, value, count;
    Uniform kind;
    uint32_t call;
};
// Preserve the instruction order, including 0x1BC before 0x1B4 and the late
// 0x1B8 upload. A zero pointer skips an array even when its location is cold.
inline constexpr std::array<Upload, 48> kUploads{{
    {0x1C0, 0x324, 0, Uniform::Uniform1i, 0x1E36AC},
    {0x1C4, 0x340, 0, Uniform::Uniform1i, 0x1E3718},
    {0x1C8, 0x35C, 0, Uniform::Uniform1i, 0x1E3784},
    {0x1CC, 0x378, 0, Uniform::Uniform1i, 0x1E37F0},
    {0x1D0, 0x394, 0, Uniform::Uniform1i, 0x1E385C},
    {0x1D4, 0x3B0, 0, Uniform::Uniform1i, 0x1E38C8},
    {0x1D8, 0x3CC, 0, Uniform::Uniform1i, 0x1E3934},
    {0x1DC, 0x3E8, 0, Uniform::Uniform1i, 0x1E39A0},
    {0x1E0, 0x404, 0, Uniform::Uniform1i, 0x1E3A0C},
    {0x224, 0x5EC, 0x5F0, Uniform::UniformMatrix4fv, 0x1E3A94},
    {0x228, 0x608, 0x60C, Uniform::UniformMatrix4fv, 0x1E3B18},
    {0x22C, 0x624, 0x628, Uniform::UniformMatrix4fv, 0x1E3B9C},
    {0x230, 0x640, 0x644, Uniform::UniformMatrix4fv, 0x1E3C20},
    {0x234, 0x65C, 0x660, Uniform::UniformMatrix4fv, 0x1E3CA4},
    {0x238, 0x678, 0x67C, Uniform::UniformMatrix4fv, 0x1E3D28},
    {0x23C, 0x694, 0x698, Uniform::UniformMatrix4fv, 0x1E3DAC},
    {0x240, 0x6B0, 0x6B4, Uniform::UniformMatrix4fv, 0x1E3E30},
    {0x244, 0x6CC, 0x6D0, Uniform::UniformMatrix4fv, 0x1E3EB4},
    {0x198, 0x218, 0x21C, Uniform::Uniform4fv, 0x1E3F34},
    {0x19C, 0x234, 0x238, Uniform::Uniform4fv, 0x1E3FB4},
    {0x1A0, 0x24C, 0, Uniform::Uniform1f, 0x1E4018},
    {0x1A4, 0x26C, 0x270, Uniform::Uniform4fv, 0x1E4098},
    {0x1A8, 0x288, 0x28C, Uniform::Uniform4fv, 0x1E4118},
    {0x1AC, 0x2A4, 0x2A8, Uniform::Uniform4fv, 0x1E4198},
    {0x1B0, 0x2BC, 0, Uniform::Uniform1f, 0x1E41FC},
    {0x1BC, 0x310, 0, Uniform::Uniform1f, 0x1E4260},
    {0x1B4, 0x2D8, 0, Uniform::Uniform1f, 0x1E42C4},
    {0x1E4, 0x420, 0, Uniform::Uniform1i, 0x1E4328},
    {0x1E8, 0x444, 0, Uniform::Uniform1f, 0x1E438C},
    {0x1EC, 0x464, 0x468, Uniform::Uniform4fv, 0x1E440C},
    {0x1F0, 0x47C, 0, Uniform::Uniform1f, 0x1E4470},
    {0x1F4, 0x494, 0x4A0, Uniform::Uniform1iv, 0x1E44F0},
    {0x1F8, 0x4B8, 0x4BC, Uniform::Uniform4fv, 0x1E4570},
    {0x1FC, 0x4D4, 0x4D8, Uniform::Uniform4fv, 0x1E45F0},
    {0x200, 0x4F0, 0x4F4, Uniform::Uniform4fv, 0x1E4670},
    {0x204, 0x50C, 0x510, Uniform::Uniform4fv, 0x1E46F0},
    {0x208, 0x528, 0x52C, Uniform::Uniform4fv, 0x1E4770},
    {0x20C, 0x544, 0x548, Uniform::Uniform4fv, 0x1E47F0},
    {0x210, 0x560, 0x564, Uniform::Uniform4fv, 0x1E4870},
    {0x1B8, 0x2F8, 0x2FC, Uniform::Uniform4fv, 0x1E48F0},
    {0x214, 0x578, 0, Uniform::Uniform1f, 0x1E4954},
    {0x218, 0x594, 0, Uniform::Uniform1f, 0x1E49B8},
    {0x21C, 0x5B0, 0, Uniform::Uniform1f, 0x1E4A1C},
    {0x220, 0x5C4, 0, Uniform::Uniform1i, 0x1E4A80},
    {0x248, 0x6E8, 0x6EC, Uniform::Uniform4fv, 0x1E4B00},
    {0x24C, 0x704, 0x708, Uniform::Uniform4fv, 0x1E4B80},
    {0x250, 0x71C, 0, Uniform::Uniform1f, 0x1E4BE4},
    {0x254, 0x73C, 0x740, Uniform::Uniform4fv, 0x1E4C64},
}};

struct Commit {
    uint32_t effect, table, context, program, index;
};

// No GL calls, guest writes or FP operations occur during this validation.
inline bool PrepareCommit(const uint8_t* guest, size_t size, uint32_t base, uint32_t effect,
                          const std::array<uint32_t, 5>& thunks, Commit& out) {
    if (!guest || !Valid(size, effect, 0x54) || (effect & 3)) return false;
    out.effect = effect;
    out.program = Read<uint32_t>(guest + effect + 0x50);
    if (!out.program) return true;
    for (size_t i = 0; i < kBindings.size(); ++i) {
        const uint64_t slot = uint64_t(base) + kBindings[i].got;
        if (!Valid(size, slot, 4) || !Valid(size, thunks[i], 8) ||
            Read<uint32_t>(guest + slot) != thunks[i]) return false;
    }
    out.index = Read<uint32_t>(guest + effect + 0x4C);
    const uint64_t table = uint64_t(Read<uint32_t>(guest + effect + 0x48)) +
                           uint64_t(out.index) * 192;
    const uint64_t context_slot = uint64_t(base) + kGraphicsContextGot;
    if (!Valid(size, table, 0x258) || table > UINT32_MAX || (table & 3) ||
        !Valid(size, context_slot, 4)) return false;
    out.table = static_cast<uint32_t>(table);
    out.context = Read<uint32_t>(guest + context_slot);
    if (!Valid(size, out.context, 0x744) || (out.context & 3)) return false;
    for (const auto& upload : kUploads) {
        const uint32_t value = Read<uint32_t>(guest + out.context + upload.value);
        if (upload.count && !value) continue;
        const uint32_t location = Read<uint32_t>(guest + out.table + upload.location);
        if (location == 0xFFFFFFFEu) return false;
        if (location & 0x80000000u) continue;
        if (upload.count) {
            const uint32_t count = Read<uint32_t>(guest + out.context + upload.count);
            const uint32_t stride = upload.kind == Uniform::UniformMatrix4fv ? 64 :
                                    upload.kind == Uniform::Uniform4fv ? 16 : 4;
            if (count > uint32_t(INT32_MAX) || (value & 3) ||
                (count && !Valid(size, value, uint64_t(count) * stride))) return false;
        }
    }
    return true;
}

// Hook after the original PUSH/MOV at 0x1E35B0, resume at the original MOV/POP
// tail 0x1E4C68. At each callback the virtual registers, NZCV and PLT side effects
// match the original SVC seam. The GL handler leaves all guest registers intact.
// Return false from call() to suspend after that upload. An unchanged seam
// resumes at its original BL continuation; a callback PC redirect is retained.
// A changed LR resumes the original trampoline BX rather than assuming a target.
template<class Call>
inline void CommitChanges(const uint8_t* guest, uint32_t base, const Commit& data,
                          const std::array<uint32_t, 5>& thunks,
                          std::array<uint32_t, 16>& r, uint32_t& nzcv, Call&& call) {
    r[2] = data.program;
    nzcv = CompareZero(r[2]);
    if (!data.program) {
        r[15] = base + kCommitTail;
        return;
    }
    for (size_t i = 0; i < kUploads.size(); ++i) {
        const auto& upload = kUploads[i];
        if (i == 9) r[5] = 0xEA8u;
        if (upload.count) {
            r[0] = Read<uint32_t>(guest + data.context + upload.value);
            nzcv = CompareZero(r[0]);
            if (!r[0]) continue;
        }
        r[1] = data.index * 3;
        r[0] = Read<uint32_t>(guest + data.table + upload.location);
        nzcv = CompareZero(r[0]);
        if (r[0] & 0x80000000u) continue;
        if (i < 9) r[2] = 0xEA8u;
        const uint32_t value = Read<uint32_t>(guest + data.context + upload.value);
        if (!upload.count) {
            r[1] = value;
        } else if (upload.kind == Uniform::UniformMatrix4fv) {
            r[1] = Read<uint32_t>(guest + data.context + upload.count);
            r[2] = 0;
            r[3] = value;
        } else {
            r[1] = Read<uint32_t>(guest + data.context + upload.count);
            r[2] = value;
        }
        const size_t kind = static_cast<size_t>(upload.kind);
        r[12] = base + kBindings[kind].got;
        r[14] = base + upload.call + 4;
        r[15] = thunks[kind] + 4;
        const bool proceed = call(upload.kind);
        if (r[15] != thunks[kind] + 4) return;
        if (r[14] != base + upload.call + 4) return;
        r[15] = r[14];
        if (!proceed) return;
    }
    r[15] = base + kCommitTail;
}

}  // namespace warband_render
