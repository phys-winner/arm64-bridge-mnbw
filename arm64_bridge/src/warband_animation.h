#pragma once

#include "warband_skinning.h"
#include <array>
#include <cmath>

// Static native translation of the pinned guest's allocation-free skeleton bone
// loop and its helpers. Instruction comments retain the exact ELF source address.
// Keep original prefix/channel preparation and final flags/prologue restoration.
// libMBExpMobile.so SHA256 ABF89B91B8E0C601A4807F5157A2C84701738B7046A3AF61C883296FF0D99A3E.
namespace warband_animation {
struct CodeGuard { uint32_t offset, bytes, hash; };
constexpr CodeGuard kGuards[] = {
    {0x2086E0u, 3792u, 0xA2EC0A6u},
    {0x20AC20u, 688u, 0x4070E628u},
    {0x20AAD0u, 336u, 0x939D716u},
    {0x20AED0u, 248u, 0x41227B2u},
    {0x20AFC8u, 336u, 0x939D716u},
    {0x2095B0u, 284u, 0x3904E463u},
    {0x6E824u, 404u, 0x8A378A24u},
    {0x516B8u, 12u, 0x3B0FCB64u},
};
// Guard [2087C0,209574); install one SVC at the one-time prefix branch2087C0.
// Leave repeated entries2087C4/2087C8 unpatched, so rejected skeletons enter SVC once.
constexpr uint32_t kLoopHash = 0x23B79698u;
constexpr uint32_t kSqrtGotOffset = 0x3D7460u;


using warband_skinning::Read;
using warband_skinning::Mul;
using warband_skinning::Add;

inline uint32_t Bits(float value) { uint32_t bits; std::memcpy(&bits, &value, 4); return bits; }
inline float Float(uint32_t bits) { float value; std::memcpy(&value, &bits, 4); return value; }
inline float Sub(float a, float b) {
    float value; asm volatile("fsub %s0, %s1, %s2" : "=w"(value) : "w"(a), "w"(b)); return value;
}
inline float Div(float a, float b) {
    float value; asm volatile("fdiv %s0, %s1, %s2" : "=w"(value) : "w"(a), "w"(b)); return value;
}
inline float Neg(float a) {
    float value; asm volatile("fneg %s0, %s1" : "=w"(value) : "w"(a)); return value;
}
inline float SignedFloat(uint32_t bits) {
    float value; asm volatile("scvtf %s0, %w1" : "=w"(value) : "r"(bits)); return value;
}
inline uint32_t Compare(float a, float b) {
    uint64_t value; asm volatile("fcmpe %s1, %s2\n\tmrs %0, nzcv" : "=r"(value) : "w"(a), "w"(b) : "cc");
    return static_cast<uint32_t>(value);
}
inline uint32_t Cmp(uint32_t a, uint32_t b) {
    uint64_t value; asm volatile("cmp %w1, %w2\n\tmrs %0, nzcv" : "=&r"(value) : "r"(a), "r"(b) : "cc");
    return static_cast<uint32_t>(value);
}
inline uint32_t Cmn(uint32_t a, uint32_t b) {
    uint64_t value; asm volatile("cmn %w1, %w2\n\tmrs %0, nzcv" : "=&r"(value) : "r"(a), "r"(b) : "cc");
    return static_cast<uint32_t>(value);
}
inline uint32_t Logic(uint32_t flags, uint32_t value) {
    return (flags & 0x30000000u) | (value & 0x80000000u) | (!value ? 0x40000000u : 0u);
}
inline bool Pred(uint32_t flags, uint32_t condition) {
    const bool n = flags & 0x80000000u, z = flags & 0x40000000u;
    const bool c = flags & 0x20000000u, v = flags & 0x10000000u;
    switch (condition) {
    case 0: return z; case 1: return !z; case 2: return c; case 3: return !c;
    case 4: return n; case 5: return !n; case 6: return v; case 7: return !v;
    case 8: return c && !z; case 9: return !c || z; case 10: return n == v;
    case 11: return n != v; case 12: return !z && n == v; case 13: return z || n != v;
    default: return true;
    }
}
inline uint32_t Lsl(uint32_t value, uint32_t shift) { return shift >= 32 ? 0 : value << shift; }
inline uint32_t Lsr(uint32_t value, uint32_t shift) { return shift >= 32 ? 0 : value >> shift; }
inline uint32_t Asr(uint32_t value, uint32_t shift) {
    return static_cast<uint32_t>(static_cast<int32_t>(value) >> (shift >= 32 ? 31 : shift));
}

struct Context {
    uint8_t* guest;
    uint32_t base, flags, nzcv;
    std::array<uint32_t, 16> r;
    std::array<float, 64> s;
    uint32_t Load32(uint32_t address) const { return Read<uint32_t>(guest + address); }
    uint32_t Load8(uint32_t address) const { return guest[address]; }
    void Store32(uint32_t address, uint32_t value) { std::memcpy(guest + address, &value, 4); }
    void LoadFP(uint32_t address, uint32_t first, uint32_t count) {
        std::memcpy(s.data() + first, guest + address, count * 4);
    }
    void StoreFP(uint32_t address, uint32_t first, uint32_t count) {
        std::memcpy(guest + address, s.data() + first, count * 4);
    }
};

// The original host sqrtf callback can set errno, but its FPSR exception flags
// are discarded before subsequent guest FP instructions. Preserve both effects.
// A volatile function pointer prevents compiler builtin substitution.
inline void Sqrt(Context& c) {
    // Stock PLT ADD r12,pc,#300000; ADD r12,r12,#85000;
    // LDR pc,[r12,#DA0]! leaves IP at its GOT slot through the sqrtf thunk.
    c.r[12] = c.base + kSqrtGotOffset;
    uint64_t saved;
    asm volatile("mrs %0, fpsr" : "=r"(saved) : : "memory");
    float (*volatile function)(float) = ::sqrtf;
    c.r[0] = Bits(function(Float(c.r[0])));
    asm volatile("msr fpsr, %0" : : "r"(saved) : "memory");
}

inline void Loop(Context& c);
inline void Evaluate(Context& c);
inline void KeyQuat(Context& c);
inline void Root(Context& c);
inline void KeyVec(Context& c);
inline void Global(Context& c);
inline void Matrix(Context& c);


// Register storage is private to each native activation. Masks are conservative
// unions of the pinned helper's register references/definitions, including its
// nested calls. Synchronization preserves the original guest BL/stack protocol.
template<uint32_t R, uint64_t S>
__attribute__((always_inline)) inline void Publish(Context& c, const std::array<uint32_t, 16>& r,
                    const std::array<float, 64>& s) {
    #pragma clang loop unroll(full)
    for (uint32_t i = 0; i < 16; ++i) if (R & (1u << i)) c.r[i] = r[i];
    #pragma clang loop unroll(full)
    for (uint32_t i = 0; i < 64; ++i) if (S & (uint64_t{1} << i)) c.s[i] = s[i];
}
template<uint32_t R, uint64_t S>
__attribute__((always_inline)) inline void Reload(const Context& c, std::array<uint32_t, 16>& r,
                   std::array<float, 64>& s) {
    #pragma clang loop unroll(full)
    for (uint32_t i = 0; i < 16; ++i) if (R & (1u << i)) r[i] = c.r[i];
    #pragma clang loop unroll(full)
    for (uint32_t i = 0; i < 64; ++i) if (S & (uint64_t{1} << i)) s[i] = c.s[i];
}

inline void Loop(Context& c) {
    auto r = c.r;
    auto s = c.s;
    auto& flags = c.flags;
    goto L2087C8;
L2087C4:
    r[1] = c.Load32(r[10] + 0x4Cu);
L2087C8:
    r[0] = c.Load32(r[10] + 0x4u);  // 2087C8 ldr r0, [r10, #0x4]
    r[0] = r[11] * r[9] + r[0];  // 2087CC mla r0, r11, r9, r0
    s[0] = Float(c.Load32(r[0] + 0xC8u));  // 2087D0 vldr s0, [r0, #200]
    s[2] = Float(c.Load32(r[0] + 0xD0u));  // 2087D4 vldr s2, [r0, #208]
    c.Store32(r[13] + 0x34u, Bits(s[0]));  // 2087D8 vstr s0, [sp, #52]
    s[0] = Float(c.Load32(r[0] + 0xCCu));  // 2087DC vldr s0, [r0, #204]
    s[26] = Float(c.Load32(r[0] + 0xC4u));  // 2087E0 vldr s26, [r0, #196]
    { r[0] = r[1] - 0x1u; flags = Cmp(r[1], 0x1u); }  // 2087E4 subs r0, r1, #1
    c.Store32(r[13] + 0x38u, Bits(s[0]));  // 2087E8 vstr s0, [sp, #56]
    c.Store32(r[13] + 0x3Cu, Bits(s[2]));  // 2087EC vstr s2, [sp, #60]
    c.Store32(r[13] + 0xD4u, r[0]);  // 2087F0 str r0, [sp, #0xd4]
    if (Pred(flags, 11u)) goto L208C4C;  // 2087F4 blt 0x208c4c <_ZN11rglSkeleton20setup_frames_dynamicEv+0x56c>
    r[2] = r[1] * r[8];  // 2087F8 mul r2, r1, r8
    r[0] = ~(0x1Fu);  // 2087FC mvn r0, #31
    r[6] = r[11] & 255u;  // 208800 uxtb r6, r11
    r[3] = 0x20u - r[6];  // 208804 rsb r3, r6, #32
    r[7] = r[0] + (r[11] & 255u);  // 208808 uxtab r7, r0, r11
    r[5] = 0x1u;  // 20880C mov r5, #1
    r[0] = c.Load32(r[10] + 0x48u);  // 208810 ldr r0, [r10, #0x48]
    r[3] = Lsr(r[5], r[3] & 255u);  // 208814 lsr r3, r5, r3
    flags = Cmp(r[7], 0x0u);  // 208818 cmp r7, #0
    if (Pred(flags, 10u)) r[3] = Lsl(r[5], r[7] & 255u);  // 20881C lslge r3, r5, r7
    r[7] = r[0] - 0xC0u;  // 208820 sub r7, r0, #192
    r[12] = Lsl(r[5], r[6] & 255u);  // 208824 lsl r12, r5, r6
L208828:
    r[4] = r[0] + r[2];  // 208828 add r4, r0, r2
    s[0] = Float(c.Load32(r[4] + 0xFFFFFF38u));  // 20882C vldr s0, [r4, #-200]
    c.nzcv = Compare(s[0], Float(0x0u));  // 208830 vcmpe.f32 s0, #0
    flags = c.nzcv;  // 208834 vmrs APSR_nzcv, fpscr
    if (Pred(flags, 9u)) goto L208858;  // 208838 bls 0x208858 <_ZN11rglSkeleton20setup_frames_dynamicEv+0x178>
    r[6] = r[7];  // 20883C mov r6, r7
    r[5] = c.Load32((r[6] = r[6] + r[2]));  // 208840 ldr r5, [r6, r2]!
    r[6] = c.Load32(r[6] + 0x4u);  // 208844 ldr r6, [r6, #0x4]
    r[5] = r[12] & r[5];  // 208848 and r5, r12, r5
    r[6] = r[3] & r[6];  // 20884C and r6, r3, r6
    { r[6] = r[5] | r[6]; flags = Logic(flags, r[6]); }  // 208850 orrs r6, r5, r6
    if (Pred(flags, 1u)) goto L20887C;  // 208854 bne 0x20887c <_ZN11rglSkeleton20setup_frames_dynamicEv+0x19c>
L208858:
    r[6] = r[1] - 0x2u;  // 208858 sub r6, r1, #2
    r[1] = r[1] - 0x1u;  // 20885C sub r1, r1, #1
    r[2] = r[2] - 0xC8u;  // 208860 sub r2, r2, #200
    c.Store32(r[13] + 0xD4u, r[6]);  // 208864 str r6, [sp, #0xd4]
    flags = Cmp(r[1], 0x0u);  // 208868 cmp r1, #0
    if (Pred(flags, 12u)) goto L208828;  // 20886C bgt 0x208828 <_ZN11rglSkeleton20setup_frames_dynamicEv+0x148>
    r[9] = 0x214u;  // 208870 mov r9, #532
    r[11] = c.Load32(r[13] + 0xD0u);  // 208874 ldr r11, [sp, #0xd0]
    goto L208C50;  // 208878 b 0x208c50 <_ZN11rglSkeleton20setup_frames_dynamicEv+0x570>
L20887C:
    r[7] = r[1] - 0x1u;  // 20887C sub r7, r1, #1
    r[9] = 0x214u;  // 208880 mov r9, #532
    flags = Cmp(r[11], 0x0u);  // 208884 cmp r11, #0
    if (Pred(flags, 1u)) goto L2088E4;  // 208888 bne 0x2088e4 <_ZN11rglSkeleton20setup_frames_dynamicEv+0x204>
    r[1] = r[4] - 0x38u;  // 20888C sub r1, r4, #56
    s[23] = Float(c.Load32(r[13] + 0x30u));  // 208890 vldr s23, [sp, #48]
    s[25] = Float(c.Load32(r[13] + 0x2Cu));  // 208894 vldr s25, [sp, #44]
    r[1] = c.Load32(r[1]);  // 208898 ldr r1, [r1]
    s[27] = Float(c.Load32(r[13] + 0x28u));  // 20889C vldr s27, [sp, #40]
    flags = Cmp(r[1], 0xDu);  // 2088A0 cmp r1, #13
    if (Pred(flags, 1u)) goto L2088E4;  // 2088A4 bne 0x2088e4 <_ZN11rglSkeleton20setup_frames_dynamicEv+0x204>
    r[0] = r[13] + 0xC0u;  // 2088A8 add r0, sp, #192
    r[2] = r[13] + 0xD4u;  // 2088AC add r2, sp, #212
    r[1] = r[10];  // 2088B0 mov r1, r10
    { r[14] = c.base + 0x2088B8u; Publish<0xF8FFu, 0x0000000300005555ull>(c, r, s); Root(c); Reload<0xF8FFu, 0x0000000300005555ull>(c, r, s); }  // 2088B4 bl 0x20aed0 <_ZN11rglSkeleton26evaluate_root_pos_intervalERKi>
    s[4] = Float(c.Load32(r[13] + 0xC0u));  // 2088B8 vldr s4, [sp, #192]
    s[2] = Float(c.Load32(r[13] + 0xC4u));  // 2088BC vldr s2, [sp, #196]
    s[0] = Float(c.Load32(r[13] + 0xC8u));  // 2088C0 vldr s0, [sp, #200]
    s[27] = s[4];  // 2088C4 vmov.f32 s27, s4
    s[25] = s[2];  // 2088C8 vmov.f32 s25, s2
    r[0] = c.Load32(r[10] + 0x48u);  // 2088CC ldr r0, [r10, #0x48]
    s[23] = s[0];  // 2088D0 vmov.f32 s23, s0
    r[7] = c.Load32(r[13] + 0xD4u);  // 2088D4 ldr r7, [sp, #0xd4]
    c.Store32(r[13] + 0x28u, Bits(s[4]));  // 2088D8 vstr s4, [sp, #40]
    c.Store32(r[13] + 0x2Cu, Bits(s[2]));  // 2088DC vstr s2, [sp, #44]
    c.Store32(r[13] + 0x30u, Bits(s[0]));  // 2088E0 vstr s0, [sp, #48]
L2088E4:
    r[1] = r[7] * r[8] + r[0];  // 2088E4 mla r1, r7, r8, r0
    r[1] = c.Load32(r[1] + 0x34u);  // 2088E8 ldr r1, [r1, #0x34]
    flags = Cmp(r[1], r[11]);  // 2088EC cmp r1, r11
    if (Pred(flags, 1u)) goto L208B8C;  // 2088F0 bne 0x208b8c <_ZN11rglSkeleton20setup_frames_dynamicEv+0x4ac>
    s[28] = s[17];  // 2088F4 vmov.f32 s28, s17
    r[4] = r[13] + 0xD0u;  // 2088F8 add r4, sp, #208
    s[16] = s[17];  // 2088FC vmov.f32 s16, s17
    r[5] = r[13] + 0xA8u;  // 208900 add r5, sp, #168
    s[30] = s[17];  // 208904 vmov.f32 s30, s17
    r[11] = r[4];  // 208908 mov r11, r4
    s[22] = s[17];  // 20890C vmov.f32 s22, s17
    r[6] = r[13] + 0xD4u;  // 208910 add r6, sp, #212
    r[4] = r[13] + 0xBCu;  // 208914 add r4, sp, #188
    r[1] = 0x0u;  // 208918 mov r1, #0
    c.Store32(r[13] + 0xBCu, r[1]);  // 20891C str r1, [sp, #0xbc]
L208920:
    r[2] = r[7] * r[8] + r[0];  // 208920 mla r2, r7, r8, r0
    r[2] = r[2] + Lsl(r[1], 0x2u);  // 208924 add r2, r2, r1, lsl #2
    s[0] = Float(c.Load32(r[2] + 0xB4u));  // 208928 vldr s0, [r2, #180]
    c.nzcv = Compare(s[0], s[18]);  // 20892C vcmpe.f32 s0, s18
    flags = c.nzcv;  // 208930 vmrs APSR_nzcv, fpscr
    if (Pred(flags, 13u)) goto L208998;  // 208934 ble 0x208998 <_ZN11rglSkeleton20setup_frames_dynamicEv+0x2b8>
    r[0] = r[5];  // 208938 mov r0, r5
    r[1] = r[10];  // 20893C mov r1, r10
    r[2] = r[6];  // 208940 mov r2, r6
    r[3] = r[4];  // 208944 mov r3, r4
    c.Store32(r[13], r[11]);  // 208948 str r11, [sp]
    { r[14] = c.base + 0x208950u; Publish<0xFFFFu, 0x0000000F0000557Full>(c, r, s); Evaluate(c); Reload<0xFFFFu, 0x0000000F0000557Full>(c, r, s); }  // 20894C bl 0x20ac20 <_ZN11rglSkeleton17evaluate_intervalERKiS1_S1_>
    r[7] = c.Load32(r[13] + 0xD4u);  // 208950 ldr r7, [sp, #0xd4]
    r[0] = c.Load32(r[10] + 0x48u);  // 208954 ldr r0, [r10, #0x48]
    r[1] = c.Load32(r[13] + 0xBCu);  // 208958 ldr r1, [sp, #0xbc]
    r[2] = r[7] * r[8] + r[0];  // 20895C mla r2, r7, r8, r0
    s[2] = Float(c.Load32(r[13] + 0xA8u));  // 208960 vldr s2, [sp, #168]
    s[0] = Float(c.Load32(r[13] + 0xACu));  // 208964 vldr s0, [sp, #172]
    s[4] = Float(c.Load32(r[13] + 0xB0u));  // 208968 vldr s4, [sp, #176]
    s[6] = Float(c.Load32(r[13] + 0xB4u));  // 20896C vldr s6, [sp, #180]
    r[2] = r[2] + Lsl(r[1], 0x2u);  // 208970 add r2, r2, r1, lsl #2
    s[8] = Float(c.Load32(r[2] + 0xB4u));  // 208974 vldr s8, [r2, #180]
    s[2] = Mul(s[8], s[2]);  // 208978 vmul.f32 s2, s8, s2
    s[6] = Mul(s[8], s[6]);  // 20897C vmul.f32 s6, s8, s6
    s[4] = Mul(s[8], s[4]);  // 208980 vmul.f32 s4, s8, s4
    s[0] = Mul(s[0], s[8]);  // 208984 vmul.f32 s0, s0, s8
    s[28] = Add(s[28], s[2]);  // 208988 vadd.f32 s28, s28, s2
    s[22] = Add(s[22], s[6]);  // 20898C vadd.f32 s22, s22, s6
    s[30] = Add(s[30], s[4]);  // 208990 vadd.f32 s30, s30, s4
    s[16] = Add(s[16], s[0]);  // 208994 vadd.f32 s16, s16, s0
L208998:
    r[1] = r[1] + 0x1u;  // 208998 add r1, r1, #1
    c.Store32(r[13] + 0xBCu, r[1]);  // 20899C str r1, [sp, #0xbc]
    flags = Cmp(r[1], 0x2u);  // 2089A0 cmp r1, #2
    if (Pred(flags, 11u)) goto L208920;  // 2089A4 blt 0x208920 <_ZN11rglSkeleton20setup_frames_dynamicEv+0x240>
    r[5] = c.Load32(r[13] + 0xD0u);  // 2089A8 ldr r5, [sp, #0xd0]
    r[0] = c.Load32(r[10] + 0x4u);  // 2089AC ldr r0, [r10, #0x4]
    r[3] = r[5] * r[9] + r[0];  // 2089B0 mla r3, r5, r9, r0
    r[0] = c.Load32((r[3] = r[3] + 0x1F8u));  // 2089B4 ldr r0, [r3, #0x1f8]!
    flags = Cmn(r[0], 0x1u);  // 2089B8 cmn r0, #1
    if (Pred(flags, 0u)) goto L208C3C;  // 2089BC beq 0x208c3c <_ZN11rglSkeleton20setup_frames_dynamicEv+0x55c>
    r[0] = r[13] + 0x98u;  // 2089C0 add r0, sp, #152
    r[1] = r[10];  // 2089C4 mov r1, r10
    r[2] = r[6];  // 2089C8 mov r2, r6
    { r[14] = c.base + 0x2089D0u; Publish<0xFFFFu, 0x0000000F0003FFFFull>(c, r, s); Global(c); Reload<0xFFFFu, 0x0000000F0003FFFFull>(c, r, s); }  // 2089CC bl 0x2095b0 <_ZN11rglSkeleton15get_global_quatERKiS1_>
    s[0] = Mul(s[16], s[16]);  // 2089D0 vmul.f32 s0, s16, s16
    s[4] = Mul(s[30], s[30]);  // 2089D4 vmul.f32 s4, s30, s30
    s[2] = Mul(s[22], s[22]);  // 2089D8 vmul.f32 s2, s22, s22
    s[0] = Add(s[4], s[0]);  // 2089DC vadd.f32 s0, s4, s0
    s[4] = Mul(s[28], s[28]);  // 2089E0 vmul.f32 s4, s28, s28
    s[0] = Add(s[2], s[0]);  // 2089E4 vadd.f32 s0, s2, s0
    s[0] = Add(s[4], s[0]);  // 2089E8 vadd.f32 s0, s4, s0
    r[0] = Bits(s[0]);  // 2089EC vmov r0, s0
    { r[14] = c.base + 0x2089F4u; Publish<0x0001u, 0x0000000000000000ull>(c, r, s); Sqrt(c); Reload<0x1001u, 0x0000000000000000ull>(c, r, s); }  // 2089F0 bl 0x516b8 <.plt+0x998>
    s[8] = Float(r[0]);  // 2089F4 vmov s8, r0
    s[0] = Float(c.Load32(c.base + 0x208A00u + 0x320u));  // 2089F8 vldr s0, [pc, #800]
    s[2] = s[17];  // 2089FC vmov.f32 s2, s17
    c.nzcv = Compare(s[8], s[0]);  // 208A00 vcmpe.f32 s8, s0
    s[0] = s[17];  // 208A04 vmov.f32 s0, s17
    flags = c.nzcv;  // 208A08 vmrs APSR_nzcv, fpscr
    s[4] = s[19];  // 208A0C vmov.f32 s4, s19
    s[6] = s[17];  // 208A10 vmov.f32 s6, s17
    if (Pred(flags, 9u)) goto L208A2C;  // 208A14 bls 0x208a2c <_ZN11rglSkeleton20setup_frames_dynamicEv+0x34c>
    s[4] = Div(s[19], s[8]);  // 208A18 vdiv.f32 s4, s19, s8
    s[6] = Mul(s[28], s[4]);  // 208A1C vmul.f32 s6, s28, s4
    s[0] = Mul(s[22], s[4]);  // 208A20 vmul.f32 s0, s22, s4
    s[2] = Mul(s[30], s[4]);  // 208A24 vmul.f32 s2, s30, s4
    s[4] = Mul(s[16], s[4]);  // 208A28 vmul.f32 s4, s16, s4
L208A2C:
    s[14] = Float(c.Load32(r[13] + 0x9Cu));  // 208A2C vldr s14, [sp, #156]
    s[12] = Float(c.Load32(r[13] + 0xA0u));  // 208A30 vldr s12, [sp, #160]
    s[10] = Float(c.Load32(r[13] + 0xA4u));  // 208A34 vldr s10, [sp, #164]
    s[7] = Mul(s[0], s[14]);  // 208A38 vmul.f32 s7, s0, s14
    s[5] = Mul(s[0], s[12]);  // 208A3C vmul.f32 s5, s0, s12
    s[8] = Float(c.Load32(r[13] + 0x98u));  // 208A40 vldr s8, [sp, #152]
    s[13] = Mul(s[4], s[10]);  // 208A44 vmul.f32 s13, s4, s10
    r[1] = c.Load32(r[10] + 0x4u);  // 208A48 ldr r1, [r10, #0x4]
    s[1] = Mul(s[2], s[10]);  // 208A4C vmul.f32 s1, s2, s10
    r[2] = r[5] * r[9] + r[1];  // 208A50 mla r2, r5, r9, r1
    s[11] = Mul(s[4], s[12]);  // 208A54 vmul.f32 s11, s4, s12
    s[15] = Mul(s[2], s[14]);  // 208A58 vmul.f32 s15, s2, s14
    r[7] = c.Load32(r[13] + 0xD4u);  // 208A5C ldr r7, [sp, #0xd4]
    s[3] = Mul(s[4], s[8]);  // 208A60 vmul.f32 s3, s4, s8
    r[0] = c.Load32(r[10] + 0x48u);  // 208A64 ldr r0, [r10, #0x48]
    r[0] = r[7] * r[8] + r[0];  // 208A68 mla r0, r7, r8, r0
    s[13] = Sub(s[13], s[7]);  // 208A6C vsub.f32 s13, s13, s7
    s[9] = Sub(s[5], s[1]);  // 208A70 vsub.f32 s9, s5, s1
    r[2] = c.Load32(r[2] + 0x1F8u);  // 208A74 ldr r2, [r2, #0x1f8]
    s[5] = Mul(s[2], s[8]);  // 208A78 vmul.f32 s5, s2, s8
    s[7] = Mul(s[0], s[8]);  // 208A7C vmul.f32 s7, s0, s8
    s[11] = Sub(s[15], s[11]);  // 208A80 vsub.f32 s11, s15, s11
    r[1] = r[2] * r[9] + r[1];  // 208A84 mla r1, r2, r9, r1
    s[1] = Mul(s[4], s[14]);  // 208A88 vmul.f32 s1, s4, s14
    s[15] = Mul(s[2], s[12]);  // 208A8C vmul.f32 s15, s2, s12
    s[14] = Mul(s[6], s[14]);  // 208A90 vmul.f32 s14, s6, s14
    s[3] = Add(s[9], s[3]);  // 208A94 vadd.f32 s3, s9, s3
    s[5] = Add(s[5], s[13]);  // 208A98 vadd.f32 s5, s5, s13
    s[12] = Mul(s[6], s[12]);  // 208A9C vmul.f32 s12, s6, s12
    s[7] = Add(s[7], s[11]);  // 208AA0 vadd.f32 s7, s7, s11
    s[13] = Mul(s[6], s[10]);  // 208AA4 vmul.f32 s13, s6, s10
    s[9] = Mul(s[0], s[10]);  // 208AA8 vmul.f32 s9, s0, s10
    s[10] = Float(c.Load32(r[1] + 0xE8u));  // 208AAC vldr s10, [r1, #232]
    s[11] = Add(s[15], s[1]);  // 208AB0 vadd.f32 s11, s15, s1
    s[1] = Float(c.Load32(r[1] + 0xF0u));  // 208AB4 vldr s1, [r1, #240]
    s[3] = Add(s[3], s[14]);  // 208AB8 vadd.f32 s3, s3, s14
    s[14] = Float(c.Load32(r[1] + 0xE4u));  // 208ABC vldr s14, [r1, #228]
    s[8] = Mul(s[6], s[8]);  // 208AC0 vmul.f32 s8, s6, s8
    s[5] = Add(s[12], s[5]);  // 208AC4 vadd.f32 s5, s12, s5
    s[12] = Float(c.Load32(r[1] + 0xECu));  // 208AC8 vldr s12, [r1, #236]
    s[7] = Add(s[13], s[7]);  // 208ACC vadd.f32 s7, s13, s7
    s[20] = Add(s[9], s[11]);  // 208AD0 vadd.f32 s20, s9, s11
    s[22] = Mul(s[3], s[10]);  // 208AD4 vmul.f32 s22, s3, s10
    s[13] = Mul(s[3], s[12]);  // 208AD8 vmul.f32 s13, s3, s12
    s[30] = Mul(s[5], s[12]);  // 208ADC vmul.f32 s30, s5, s12
    s[9] = Mul(s[5], s[10]);  // 208AE0 vmul.f32 s9, s5, s10
    s[11] = Mul(s[3], s[1]);  // 208AE4 vmul.f32 s11, s3, s1
    s[15] = Mul(s[7], s[10]);  // 208AE8 vmul.f32 s15, s7, s10
    s[28] = Mul(s[1], s[5]);  // 208AEC vmul.f32 s28, s1, s5
    s[16] = Mul(s[7], s[12]);  // 208AF0 vmul.f32 s16, s7, s12
    s[8] = Sub(s[8], s[20]);  // 208AF4 vsub.f32 s8, s8, s20
    s[20] = Mul(s[1], s[7]);  // 208AF8 vmul.f32 s20, s1, s7
    s[13] = Sub(s[13], s[9]);  // 208AFC vsub.f32 s13, s13, s9
    s[7] = Mul(s[7], s[14]);  // 208B00 vmul.f32 s7, s7, s14
    s[22] = Add(s[30], s[22]);  // 208B04 vadd.f32 s22, s30, s22
    s[15] = Sub(s[15], s[11]);  // 208B08 vsub.f32 s15, s15, s11
    s[5] = Mul(s[5], s[14]);  // 208B0C vmul.f32 s5, s5, s14
    s[3] = Mul(s[3], s[14]);  // 208B10 vmul.f32 s3, s3, s14
    s[9] = Sub(s[28], s[16]);  // 208B14 vsub.f32 s9, s28, s16
    s[14] = Mul(s[8], s[14]);  // 208B18 vmul.f32 s14, s8, s14
    s[7] = Add(s[7], s[13]);  // 208B1C vadd.f32 s7, s7, s13
    s[11] = Add(s[20], s[22]);  // 208B20 vadd.f32 s11, s20, s22
    s[13] = Mul(s[8], s[12]);  // 208B24 vmul.f32 s13, s8, s12
    s[12] = Float(c.Load32(r[0] + 0x38u));  // 208B28 vldr s12, [r0, #56]
    s[1] = Mul(s[1], s[8]);  // 208B2C vmul.f32 s1, s1, s8
    s[10] = Mul(s[8], s[10]);  // 208B30 vmul.f32 s10, s8, s10
    s[9] = Add(s[9], s[3]);  // 208B34 vadd.f32 s9, s9, s3
    s[5] = Add(s[5], s[15]);  // 208B38 vadd.f32 s5, s5, s15
    s[8] = Sub(s[19], s[12]);  // 208B3C vsub.f32 s8, s19, s12
    s[11] = Add(s[14], s[11]);  // 208B40 vadd.f32 s11, s14, s11
    s[3] = Sub(s[7], s[1]);  // 208B44 vsub.f32 s3, s7, s1
    s[10] = Sub(s[9], s[10]);  // 208B48 vsub.f32 s10, s9, s10
    s[14] = Sub(s[5], s[13]);  // 208B4C vsub.f32 s14, s5, s13
    s[6] = Mul(s[6], s[8]);  // 208B50 vmul.f32 s6, s6, s8
    s[1] = Mul(s[12], s[11]);  // 208B54 vmul.f32 s1, s12, s11
    s[0] = Mul(s[0], s[8]);  // 208B58 vmul.f32 s0, s0, s8
    s[3] = Mul(s[12], s[3]);  // 208B5C vmul.f32 s3, s12, s3
    s[2] = Mul(s[2], s[8]);  // 208B60 vmul.f32 s2, s2, s8
    s[4] = Mul(s[4], s[8]);  // 208B64 vmul.f32 s4, s4, s8
    s[14] = Mul(s[12], s[14]);  // 208B68 vmul.f32 s14, s12, s14
    s[8] = Mul(s[10], s[12]);  // 208B6C vmul.f32 s8, s10, s12
    s[28] = Add(s[1], s[6]);  // 208B70 vadd.f32 s28, s1, s6
    s[22] = Add(s[3], s[0]);  // 208B74 vadd.f32 s22, s3, s0
    s[30] = Add(s[14], s[2]);  // 208B78 vadd.f32 s30, s14, s2
    s[16] = Add(s[8], s[4]);  // 208B7C vadd.f32 s16, s8, s4
    goto L208C3C;  // 208B80 b 0x208c3c <_ZN11rglSkeleton20setup_frames_dynamicEv+0x55c>
L208B8C:
    s[28] = s[17];  // 208B8C vmov.f32 s28, s17
    r[4] = r[13] + 0xD0u;  // 208B90 add r4, sp, #208
    s[16] = s[17];  // 208B94 vmov.f32 s16, s17
    r[5] = r[13] + 0xD4u;  // 208B98 add r5, sp, #212
    s[30] = s[17];  // 208B9C vmov.f32 s30, s17
    r[6] = r[13] + 0x80u;  // 208BA0 add r6, sp, #128
    s[22] = s[17];  // 208BA4 vmov.f32 s22, s17
    r[11] = r[13] + 0x94u;  // 208BA8 add r11, sp, #148
    r[1] = 0x0u;  // 208BAC mov r1, #0
    c.Store32(r[13] + 0x94u, r[1]);  // 208BB0 str r1, [sp, #0x94]
L208BB4:
    r[2] = r[7] * r[8] + r[0];  // 208BB4 mla r2, r7, r8, r0
    r[2] = r[2] + Lsl(r[1], 0x2u);  // 208BB8 add r2, r2, r1, lsl #2
    s[0] = Float(c.Load32(r[2] + 0xB4u));  // 208BBC vldr s0, [r2, #180]
    c.nzcv = Compare(s[0], s[18]);  // 208BC0 vcmpe.f32 s0, s18
    flags = c.nzcv;  // 208BC4 vmrs APSR_nzcv, fpscr
    if (Pred(flags, 13u)) goto L208C2C;  // 208BC8 ble 0x208c2c <_ZN11rglSkeleton20setup_frames_dynamicEv+0x54c>
    r[0] = r[6];  // 208BCC mov r0, r6
    r[1] = r[10];  // 208BD0 mov r1, r10
    r[2] = r[5];  // 208BD4 mov r2, r5
    r[3] = r[11];  // 208BD8 mov r3, r11
    c.Store32(r[13], r[4]);  // 208BDC str r4, [sp]
    { r[14] = c.base + 0x208BE4u; Publish<0xFFFFu, 0x0000000F0000557Full>(c, r, s); Evaluate(c); Reload<0xFFFFu, 0x0000000F0000557Full>(c, r, s); }  // 208BE0 bl 0x20ac20 <_ZN11rglSkeleton17evaluate_intervalERKiS1_S1_>
    r[7] = c.Load32(r[13] + 0xD4u);  // 208BE4 ldr r7, [sp, #0xd4]
    r[0] = c.Load32(r[10] + 0x48u);  // 208BE8 ldr r0, [r10, #0x48]
    r[1] = c.Load32(r[13] + 0x94u);  // 208BEC ldr r1, [sp, #0x94]
    r[2] = r[7] * r[8] + r[0];  // 208BF0 mla r2, r7, r8, r0
    s[2] = Float(c.Load32(r[13] + 0x80u));  // 208BF4 vldr s2, [sp, #128]
    s[0] = Float(c.Load32(r[13] + 0x84u));  // 208BF8 vldr s0, [sp, #132]
    s[4] = Float(c.Load32(r[13] + 0x88u));  // 208BFC vldr s4, [sp, #136]
    s[6] = Float(c.Load32(r[13] + 0x8Cu));  // 208C00 vldr s6, [sp, #140]
    r[2] = r[2] + Lsl(r[1], 0x2u);  // 208C04 add r2, r2, r1, lsl #2
    s[8] = Float(c.Load32(r[2] + 0xB4u));  // 208C08 vldr s8, [r2, #180]
    s[2] = Mul(s[8], s[2]);  // 208C0C vmul.f32 s2, s8, s2
    s[6] = Mul(s[8], s[6]);  // 208C10 vmul.f32 s6, s8, s6
    s[4] = Mul(s[8], s[4]);  // 208C14 vmul.f32 s4, s8, s4
    s[0] = Mul(s[0], s[8]);  // 208C18 vmul.f32 s0, s0, s8
    s[28] = Add(s[28], s[2]);  // 208C1C vadd.f32 s28, s28, s2
    s[22] = Add(s[22], s[6]);  // 208C20 vadd.f32 s22, s22, s6
    s[30] = Add(s[30], s[4]);  // 208C24 vadd.f32 s30, s30, s4
    s[16] = Add(s[16], s[0]);  // 208C28 vadd.f32 s16, s16, s0
L208C2C:
    r[1] = r[1] + 0x1u;  // 208C2C add r1, r1, #1
    c.Store32(r[13] + 0x94u, r[1]);  // 208C30 str r1, [sp, #0x94]
    flags = Cmp(r[1], 0x2u);  // 208C34 cmp r1, #2
    if (Pred(flags, 11u)) goto L208BB4;  // 208C38 blt 0x208bb4 <_ZN11rglSkeleton20setup_frames_dynamicEv+0x4d4>
L208C3C:
    r[0] = r[7] - 0x1u;  // 208C3C sub r0, r7, #1
    c.Store32(r[13] + 0xD4u, r[0]);  // 208C40 str r0, [sp, #0xd4]
    flags = Cmn(r[7], 0x1u);  // 208C44 cmn r7, #1
    if (Pred(flags, 1u)) goto L2090AC;  // 208C48 bne 0x2090ac <_ZN11rglSkeleton20setup_frames_dynamicEv+0x9cc>
L208C4C:
    r[11] = c.Load32(r[13] + 0xD0u);  // 208C4C ldr r11, [sp, #0xd0]
L208C50:
    r[0] = c.Load32(r[10] + 0x4u);  // 208C50 ldr r0, [r10, #0x4]
    r[0] = r[11] * r[9] + r[0];  // 208C54 mla r0, r11, r9, r0
    s[15] = Float(c.Load32(r[13] + 0x34u));  // 208C58 vldr s15, [sp, #52]
    s[13] = Float(c.Load32(r[13] + 0x38u));  // 208C5C vldr s13, [sp, #56]
    s[11] = Float(c.Load32(r[13] + 0x3Cu));  // 208C60 vldr s11, [sp, #60]
    s[6] = Float(c.Load32(r[0] + 0xC8u));  // 208C64 vldr s6, [r0, #200]
    s[4] = Float(c.Load32(r[0] + 0xCCu));  // 208C68 vldr s4, [r0, #204]
    s[2] = Float(c.Load32(r[0] + 0xD0u));  // 208C6C vldr s2, [r0, #208]
    s[8] = Mul(s[15], s[6]);  // 208C70 vmul.f32 s8, s15, s6
    s[3] = Mul(s[13], s[4]);  // 208C74 vmul.f32 s3, s13, s4
    s[0] = Float(c.Load32(r[0] + 0xC4u));  // 208C78 vldr s0, [r0, #196]
    s[10] = Mul(s[13], s[2]);  // 208C7C vmul.f32 s10, s13, s2
    s[5] = Mul(s[11], s[4]);  // 208C80 vmul.f32 s5, s11, s4
    s[14] = Mul(s[15], s[4]);  // 208C84 vmul.f32 s14, s15, s4
    s[9] = Mul(s[13], s[6]);  // 208C88 vmul.f32 s9, s13, s6
    s[12] = Mul(s[11], s[6]);  // 208C8C vmul.f32 s12, s11, s6
    s[7] = Mul(s[15], s[2]);  // 208C90 vmul.f32 s7, s15, s2
    s[1] = Mul(s[11], s[2]);  // 208C94 vmul.f32 s1, s11, s2
    s[8] = Add(s[3], s[8]);  // 208C98 vadd.f32 s8, s3, s8
    s[6] = Mul(s[26], s[6]);  // 208C9C vmul.f32 s6, s26, s6
    s[10] = Sub(s[5], s[10]);  // 208CA0 vsub.f32 s10, s5, s10
    s[4] = Mul(s[26], s[4]);  // 208CA4 vmul.f32 s4, s26, s4
    s[2] = Mul(s[26], s[2]);  // 208CA8 vmul.f32 s2, s26, s2
    s[12] = Sub(s[7], s[12]);  // 208CAC vsub.f32 s12, s7, s12
    s[14] = Sub(s[9], s[14]);  // 208CB0 vsub.f32 s14, s9, s14
    s[8] = Add(s[1], s[8]);  // 208CB4 vadd.f32 s8, s1, s8
    s[3] = Mul(s[26], s[0]);  // 208CB8 vmul.f32 s3, s26, s0
    s[6] = Add(s[10], s[6]);  // 208CBC vadd.f32 s6, s10, s6
    s[1] = Mul(s[15], s[0]);  // 208CC0 vmul.f32 s1, s15, s0
    s[10] = Mul(s[13], s[0]);  // 208CC4 vmul.f32 s10, s13, s0
    s[4] = Add(s[4], s[12]);  // 208CC8 vadd.f32 s4, s4, s12
    s[0] = Mul(s[11], s[0]);  // 208CCC vmul.f32 s0, s11, s0
    s[2] = Add(s[2], s[14]);  // 208CD0 vadd.f32 s2, s2, s14
    s[8] = Add(s[3], s[8]);  // 208CD4 vadd.f32 s8, s3, s8
    s[6] = Sub(s[6], s[1]);  // 208CD8 vsub.f32 s6, s6, s1
    s[4] = Sub(s[4], s[10]);  // 208CDC vsub.f32 s4, s4, s10
    s[0] = Sub(s[2], s[0]);  // 208CE0 vsub.f32 s0, s2, s0
    c.Store32(r[0] + 0xF4u, Bits(s[8]));  // 208CE4 vstr s8, [r0, #244]
    c.Store32(r[0] + 0xF8u, Bits(s[6]));  // 208CE8 vstr s6, [r0, #248]
    c.Store32(r[0] + 0xFCu, Bits(s[4]));  // 208CEC vstr s4, [r0, #252]
    c.Store32(r[0] + 0x100u, Bits(s[0]));  // 208CF0 vstr s0, [r0, #256]
    r[2] = c.Load32(r[13] + 0xD0u);  // 208CF4 ldr r2, [sp, #0xd0]
    r[3] = c.Load32(r[10] + 0x4u);  // 208CF8 ldr r3, [r10, #0x4]
    r[0] = r[2] * r[9] + r[3];  // 208CFC mla r0, r2, r9, r3
    r[1] = r[0] + 0xE4u;  // 208D00 add r1, r0, #228
    r[7] = c.Load32(r[0] + 0x1F8u);  // 208D04 ldr r7, [r0, #0x1f8]
    flags = Cmn(r[7], 0x1u);  // 208D08 cmn r7, #1
    if (Pred(flags, 1u)) goto L208D24;  // 208D0C bne 0x208d24 <_ZN11rglSkeleton20setup_frames_dynamicEv+0x644>
    r[0] = r[0] + 0xC4u;  // 208D10 add r0, r0, #196
    std::memcpy(s.data() + 32, c.guest + r[0], 16);  // 208D14 vld1.32 {d16, d17}, [r0]
    std::memcpy(c.guest + r[1], s.data() + 32, 16);  // 208D18 vst1.32 {d16, d17}, [r1]
    goto L208DCC;  // 208D1C b 0x208dcc <_ZN11rglSkeleton20setup_frames_dynamicEv+0x6ec>
L208D24:
    r[2] = r[7] * r[9] + r[3];  // 208D24 mla r2, r7, r9, r3
    s[14] = Float(c.Load32(r[0] + 0xC8u));  // 208D28 vldr s14, [r0, #200]
    s[12] = Float(c.Load32(r[0] + 0xCCu));  // 208D2C vldr s12, [r0, #204]
    s[10] = Float(c.Load32(r[0] + 0xD0u));  // 208D30 vldr s10, [r0, #208]
    s[4] = Float(c.Load32(r[0] + 0xC4u));  // 208D34 vldr s4, [r0, #196]
    s[6] = Float(c.Load32(r[2] + 0xE8u));  // 208D38 vldr s6, [r2, #232]
    s[2] = Float(c.Load32(r[2] + 0xECu));  // 208D3C vldr s2, [r2, #236]
    s[0] = Float(c.Load32(r[2] + 0xF0u));  // 208D40 vldr s0, [r2, #240]
    s[1] = Mul(s[14], s[6]);  // 208D44 vmul.f32 s1, s14, s6
    s[11] = Mul(s[2], s[12]);  // 208D48 vmul.f32 s11, s2, s12
    s[8] = Float(c.Load32(r[2] + 0xE4u));  // 208D4C vldr s8, [r2, #228]
    s[3] = Mul(s[0], s[12]);  // 208D50 vmul.f32 s3, s0, s12
    s[13] = Mul(s[2], s[10]);  // 208D54 vmul.f32 s13, s2, s10
    s[7] = Mul(s[2], s[14]);  // 208D58 vmul.f32 s7, s2, s14
    s[16] = Mul(s[12], s[6]);  // 208D5C vmul.f32 s16, s12, s6
    s[5] = Mul(s[10], s[6]);  // 208D60 vmul.f32 s5, s10, s6
    s[15] = Mul(s[0], s[14]);  // 208D64 vmul.f32 s15, s0, s14
    s[9] = Mul(s[10], s[0]);  // 208D68 vmul.f32 s9, s10, s0
    s[1] = Add(s[11], s[1]);  // 208D6C vadd.f32 s1, s11, s1
    s[14] = Mul(s[8], s[14]);  // 208D70 vmul.f32 s14, s8, s14
    s[3] = Sub(s[13], s[3]);  // 208D74 vsub.f32 s3, s13, s3
    s[12] = Mul(s[12], s[8]);  // 208D78 vmul.f32 s12, s12, s8
    s[10] = Mul(s[10], s[8]);  // 208D7C vmul.f32 s10, s10, s8
    s[5] = Sub(s[15], s[5]);  // 208D80 vsub.f32 s5, s15, s5
    s[7] = Sub(s[16], s[7]);  // 208D84 vsub.f32 s7, s16, s7
    s[8] = Mul(s[8], s[4]);  // 208D88 vmul.f32 s8, s8, s4
    s[1] = Add(s[9], s[1]);  // 208D8C vadd.f32 s1, s9, s1
    s[6] = Mul(s[6], s[4]);  // 208D90 vmul.f32 s6, s6, s4
    s[14] = Add(s[3], s[14]);  // 208D94 vadd.f32 s14, s3, s14
    s[2] = Mul(s[2], s[4]);  // 208D98 vmul.f32 s2, s2, s4
    s[12] = Add(s[12], s[5]);  // 208D9C vadd.f32 s12, s12, s5
    s[0] = Mul(s[0], s[4]);  // 208DA0 vmul.f32 s0, s0, s4
    s[4] = Add(s[10], s[7]);  // 208DA4 vadd.f32 s4, s10, s7
    s[8] = Sub(s[8], s[1]);  // 208DA8 vsub.f32 s8, s8, s1
    s[6] = Add(s[14], s[6]);  // 208DAC vadd.f32 s6, s14, s6
    s[2] = Add(s[2], s[12]);  // 208DB0 vadd.f32 s2, s2, s12
    s[0] = Add(s[0], s[4]);  // 208DB4 vadd.f32 s0, s0, s4
    c.Store32(r[1], Bits(s[8]));  // 208DB8 vstr s8, [r1]
    c.Store32(r[0] + 0xE8u, Bits(s[6]));  // 208DBC vstr s6, [r0, #232]
    c.Store32(r[0] + 0xECu, Bits(s[2]));  // 208DC0 vstr s2, [r0, #236]
    c.Store32(r[0] + 0xF0u, Bits(s[0]));  // 208DC4 vstr s0, [r0, #240]
    r[2] = c.Load32(r[13] + 0xD0u);  // 208DC8 ldr r2, [sp, #0xd0]
L208DCC:
    r[0] = c.Load32(r[10] + 0x4u);  // 208DCC ldr r0, [r10, #0x4]
    r[0] = r[2] * r[9] + r[0];  // 208DD0 mla r0, r2, r9, r0
    s[4] = Float(c.Load32(r[0] + 0xC8u));  // 208DD4 vldr s4, [r0, #200]
    s[2] = Float(c.Load32(r[0] + 0xCCu));  // 208DD8 vldr s2, [r0, #204]
    s[10] = Float(c.Load32(r[0] + 0xD0u));  // 208DDC vldr s10, [r0, #208]
    s[12] = Add(s[4], s[4]);  // 208DE0 vadd.f32 s12, s4, s4
    s[8] = Add(s[2], s[2]);  // 208DE4 vadd.f32 s8, s2, s2
    s[0] = Float(c.Load32(r[0] + 0xC4u));  // 208DE8 vldr s0, [r0, #196]
    s[6] = Add(s[10], s[10]);  // 208DEC vadd.f32 s6, s10, s10
    s[3] = Mul(s[4], s[12]);  // 208DF0 vmul.f32 s3, s4, s12
    s[14] = Mul(s[2], s[8]);  // 208DF4 vmul.f32 s14, s2, s8
    s[7] = Mul(s[10], s[6]);  // 208DF8 vmul.f32 s7, s10, s6
    s[1] = Mul(s[4], s[8]);  // 208DFC vmul.f32 s1, s4, s8
    s[9] = Mul(s[2], s[6]);  // 208E00 vmul.f32 s9, s2, s6
    s[10] = Mul(s[0], s[6]);  // 208E04 vmul.f32 s10, s0, s6
    s[8] = Mul(s[8], s[0]);  // 208E08 vmul.f32 s8, s8, s0
    s[0] = Mul(s[12], s[0]);  // 208E0C vmul.f32 s0, s12, s0
    s[2] = Add(s[3], s[14]);  // 208E10 vadd.f32 s2, s3, s14
    s[5] = Add(s[14], s[7]);  // 208E14 vadd.f32 s5, s14, s7
    s[4] = Mul(s[4], s[6]);  // 208E18 vmul.f32 s4, s4, s6
    s[7] = Add(s[3], s[7]);  // 208E1C vadd.f32 s7, s3, s7
    s[12] = Add(s[1], s[10]);  // 208E20 vadd.f32 s12, s1, s10
    s[10] = Sub(s[1], s[10]);  // 208E24 vsub.f32 s10, s1, s10
    s[3] = Add(s[9], s[0]);  // 208E28 vadd.f32 s3, s9, s0
    s[0] = Sub(s[9], s[0]);  // 208E2C vsub.f32 s0, s9, s0
    s[6] = Sub(s[19], s[5]);  // 208E30 vsub.f32 s6, s19, s5
    s[14] = Sub(s[4], s[8]);  // 208E34 vsub.f32 s14, s4, s8
    s[1] = Sub(s[19], s[7]);  // 208E38 vsub.f32 s1, s19, s7
    s[4] = Add(s[4], s[8]);  // 208E3C vadd.f32 s4, s4, s8
    s[2] = Sub(s[19], s[2]);  // 208E40 vsub.f32 s2, s19, s2
    c.Store32(r[0] + 0x44u, Bits(s[6]));  // 208E44 vstr s6, [r0, #68]
    c.Store32(r[0] + 0x48u, Bits(s[12]));  // 208E48 vstr s12, [r0, #72]
    c.Store32(r[0] + 0x4Cu, Bits(s[14]));  // 208E4C vstr s14, [r0, #76]
    c.Store32(r[0] + 0x54u, Bits(s[10]));  // 208E50 vstr s10, [r0, #84]
    c.Store32(r[0] + 0x58u, Bits(s[1]));  // 208E54 vstr s1, [r0, #88]
    c.Store32(r[0] + 0x5Cu, Bits(s[3]));  // 208E58 vstr s3, [r0, #92]
    c.Store32(r[0] + 0x64u, Bits(s[4]));  // 208E5C vstr s4, [r0, #100]
    c.Store32(r[0] + 0x68u, Bits(s[0]));  // 208E60 vstr s0, [r0, #104]
    c.Store32(r[0] + 0x6Cu, Bits(s[2]));  // 208E64 vstr s2, [r0, #108]
    r[1] = c.Load32(r[13] + 0xD0u);  // 208E68 ldr r1, [sp, #0xd0]
    flags = Cmp(r[1], 0x0u);  // 208E6C cmp r1, #0
    if (Pred(flags, 1u)) goto L208EB0;  // 208E70 bne 0x208eb0 <_ZN11rglSkeleton20setup_frames_dynamicEv+0x7d0>
    r[0] = c.Load32(r[10] + 0x8u);  // 208E74 ldr r0, [r10, #0x8]
    s[0] = Float(c.Load32(r[10] + 0xCu));  // 208E78 vldr s0, [r10, #12]
    s[2] = Float(c.Load32(r[10] + 0x10u));  // 208E7C vldr s2, [r10, #16]
    r[0] = c.Load32(r[0] + 0xA8u);  // 208E80 ldr r0, [r0, #0xa8]
    s[4] = Float(c.Load32(r[10] + 0x14u));  // 208E84 vldr s4, [r10, #20]
    s[6] = Float(c.Load32(r[0] + 0x3Cu));  // 208E88 vldr s6, [r0, #60]
    s[8] = Float(c.Load32(r[0] + 0x40u));  // 208E8C vldr s8, [r0, #64]
    s[10] = Float(c.Load32(r[0] + 0x44u));  // 208E90 vldr s10, [r0, #68]
    s[0] = Add(s[6], s[0]);  // 208E94 vadd.f32 s0, s6, s0
    s[2] = Add(s[8], s[2]);  // 208E98 vadd.f32 s2, s8, s2
    r[0] = c.Load32(r[10] + 0x4u);  // 208E9C ldr r0, [r10, #0x4]
    s[4] = Add(s[10], s[4]);  // 208EA0 vadd.f32 s4, s10, s4
    c.Store32(r[0] + 0x74u, Bits(s[0]));  // 208EA4 vstr s0, [r0, #116]
    c.Store32(r[0] + 0x78u, Bits(s[2]));  // 208EA8 vstr s2, [r0, #120]
    c.Store32(r[0] + 0x7Cu, Bits(s[4]));  // 208EAC vstr s4, [r0, #124]
L208EB0:
    r[2] = c.Load32(r[10] + 0x4u);  // 208EB0 ldr r2, [r10, #0x4]
    r[0] = r[1] * r[9] + r[2];  // 208EB4 mla r0, r1, r9, r2
    r[5] = r[0] + 0x84u;  // 208EB8 add r5, r0, #132
    r[3] = c.Load32(r[0] + 0x1F8u);  // 208EBC ldr r3, [r0, #0x1f8]
    flags = Cmn(r[3], 0x1u);  // 208EC0 cmn r3, #1
    if (Pred(flags, 1u)) goto L208F08;  // 208EC4 bne 0x208f08 <_ZN11rglSkeleton20setup_frames_dynamicEv+0x828>
    r[2] = r[0] + 0x64u;  // 208EC8 add r2, r0, #100
    r[3] = r[0] + 0x44u;  // 208ECC add r3, r0, #68
    std::memcpy(s.data() + 32, c.guest + r[2], 16);  // 208ED0 vld1.32 {d16, d17}, [r2]
    r[2] = r[0] + 0x54u;  // 208ED4 add r2, r0, #84
    r[0] = r[0] + 0x74u;  // 208ED8 add r0, r0, #116
    std::memcpy(s.data() + 44, c.guest + r[0], 16);  // 208EDC vld1.32 {d22, d23}, [r0]
    r[0] = r[5] + 0x30u;  // 208EE0 add r0, r5, #48
    std::memcpy(s.data() + 36, c.guest + r[2], 16);  // 208EE4 vld1.32 {d18, d19}, [r2]
    std::memcpy(s.data() + 40, c.guest + r[3], 16);  // 208EE8 vld1.32 {d20, d21}, [r3]
    std::memcpy(c.guest + r[0], s.data() + 44, 16);  // 208EEC vst1.32 {d22, d23}, [r0]
    r[0] = r[5] + 0x20u;  // 208EF0 add r0, r5, #32
    std::memcpy(c.guest + r[0], s.data() + 32, 16);  // 208EF4 vst1.32 {d16, d17}, [r0]
    r[0] = r[5] + 0x10u;  // 208EF8 add r0, r5, #16
    std::memcpy(c.guest + r[0], s.data() + 36, 16);  // 208EFC vst1.32 {d18, d19}, [r0]
    std::memcpy(c.guest + r[5], s.data() + 40, 16);  // 208F00 vst1.32 {d20, d21}, [r5]
    goto L208F5C;  // 208F04 b 0x208f5c <_ZN11rglSkeleton20setup_frames_dynamicEv+0x87c>
L208F08:
    r[1] = r[3] * r[9] + r[2];  // 208F08 mla r1, r3, r9, r2
    r[4] = r[13] + 0x40u;  // 208F0C add r4, sp, #64
    r[2] = r[0] + 0x44u;  // 208F10 add r2, r0, #68
    r[0] = r[4];  // 208F14 mov r0, r4
    r[1] = r[1] + 0x84u;  // 208F18 add r1, r1, #132
    { r[14] = c.base + 0x208F20u; Publish<0x2007u, 0x00000000FFFFFFFFull>(c, r, s); Matrix(c); Reload<0x2000u, 0x00000000FFFFFFFFull>(c, r, s); }  // 208F1C bl 0x6e824 <_ZNK15rglMatrix_frame19transform_to_parentERKS_>
    r[0] = r[4] + 0x20u;  // 208F20 add r0, r4, #32
    std::memcpy(s.data() + 40, c.guest + r[4], 16);  // 208F24 vld1.64 {d20, d21}, [r4]
    std::memcpy(s.data() + 32, c.guest + r[0], 16);  // 208F28 vld1.64 {d16, d17}, [r0]
    r[0] = r[4] + 0x10u;  // 208F2C add r0, r4, #16
    std::memcpy(s.data() + 36, c.guest + r[0], 16);  // 208F30 vld1.64 {d18, d19}, [r0]
    r[0] = r[4] + 0x30u;  // 208F34 add r0, r4, #48
    std::memcpy(s.data() + 44, c.guest + r[0], 16);  // 208F38 vld1.64 {d22, d23}, [r0]
    r[0] = r[5] + 0x30u;  // 208F3C add r0, r5, #48
    std::memcpy(c.guest + r[0], s.data() + 44, 16);  // 208F40 vst1.32 {d22, d23}, [r0]
    r[0] = r[5] + 0x20u;  // 208F44 add r0, r5, #32
    std::memcpy(c.guest + r[0], s.data() + 32, 16);  // 208F48 vst1.32 {d16, d17}, [r0]
    r[0] = r[5] + 0x10u;  // 208F4C add r0, r5, #16
    std::memcpy(c.guest + r[0], s.data() + 36, 16);  // 208F50 vst1.32 {d18, d19}, [r0]
    std::memcpy(c.guest + r[5], s.data() + 40, 16);  // 208F54 vst1.32 {d20, d21}, [r5]
    r[1] = c.Load32(r[13] + 0xD0u);  // 208F58 ldr r1, [sp, #0xd0]
L208F5C:
    r[0] = c.Load32(r[10] + 0x4u);  // 208F5C ldr r0, [r10, #0x4]
    r[0] = r[1] * r[9] + r[0];  // 208F60 mla r0, r1, r9, r0
    r[2] = c.Load32(r[0] + 0x144u);  // 208F64 ldr r2, [r0, #0x144]
    flags = Cmp(r[2], 0x0u);  // 208F68 cmp r2, #0
    if (Pred(flags, 0u)) goto L209094;  // 208F6C beq 0x209094 <_ZN11rglSkeleton20setup_frames_dynamicEv+0x9b4>
    s[7] = Float(c.Load32(r[0] + 0x12Cu));  // 208F70 vldr s7, [r0, #300]
    s[11] = Float(c.Load32(r[0] + 0x98u));  // 208F74 vldr s11, [r0, #152]
    s[1] = Float(c.Load32(r[0] + 0x94u));  // 208F78 vldr s1, [r0, #148]
    s[3] = Float(c.Load32(r[0] + 0x11Cu));  // 208F7C vldr s3, [r0, #284]
    s[24] = Mul(s[7], s[11]);  // 208F80 vmul.f32 s24, s7, s11
    s[0] = Float(c.Load32(r[0] + 0xA8u));  // 208F84 vldr s0, [r0, #168]
    s[26] = Mul(s[3], s[1]);  // 208F88 vmul.f32 s26, s3, s1
    s[10] = Float(c.Load32(r[0] + 0x88u));  // 208F8C vldr s10, [r0, #136]
    s[8] = Float(c.Load32(r[0] + 0x124u));  // 208F90 vldr s8, [r0, #292]
    s[5] = Mul(s[7], s[0]);  // 208F94 vmul.f32 s5, s7, s0
    s[14] = Float(c.Load32(r[0] + 0x84u));  // 208F98 vldr s14, [r0, #132]
    s[13] = Mul(s[10], s[7]);  // 208F9C vmul.f32 s13, s10, s7
    s[4] = Float(c.Load32(r[0] + 0x114u));  // 208FA0 vldr s4, [r0, #276]
    s[9] = Mul(s[8], s[11]);  // 208FA4 vmul.f32 s9, s8, s11
    s[6] = Float(c.Load32(r[0] + 0x118u));  // 208FA8 vldr s6, [r0, #280]
    s[22] = Mul(s[14], s[3]);  // 208FAC vmul.f32 s22, s14, s3
    s[12] = Float(c.Load32(r[0] + 0x128u));  // 208FB0 vldr s12, [r0, #296]
    s[20] = Mul(s[8], s[10]);  // 208FB4 vmul.f32 s20, s8, s10
    s[16] = Mul(s[14], s[6]);  // 208FB8 vmul.f32 s16, s14, s6
    s[2] = Float(c.Load32(r[0] + 0x8Cu));  // 208FBC vldr s2, [r0, #140]
    s[7] = Mul(s[12], s[11]);  // 208FC0 vmul.f32 s7, s12, s11
    s[28] = Float(c.Load32(r[0] + 0x9Cu));  // 208FC4 vldr s28, [r0, #156]
    s[11] = Mul(s[10], s[12]);  // 208FC8 vmul.f32 s11, s10, s12
    s[21] = Float(c.Load32(r[0] + 0x13Cu));  // 208FCC vldr s21, [r0, #316]
    s[10] = Add(s[26], s[24]);  // 208FD0 vadd.f32 s10, s26, s24
    s[26] = Float(c.Load32(r[0] + 0x134u));  // 208FD4 vldr s26, [r0, #308]
    s[24] = Mul(s[4], s[14]);  // 208FD8 vmul.f32 s24, s4, s14
    s[30] = Float(c.Load32(r[0] + 0xACu));  // 208FDC vldr s30, [r0, #172]
    s[14] = Mul(s[6], s[1]);  // 208FE0 vmul.f32 s14, s6, s1
    s[15] = Mul(s[4], s[1]);  // 208FE4 vmul.f32 s15, s4, s1
    s[12] = Mul(s[12], s[0]);  // 208FE8 vmul.f32 s12, s12, s0
    s[0] = Mul(s[8], s[0]);  // 208FEC vmul.f32 s0, s8, s0
    s[11] = Add(s[16], s[11]);  // 208FF0 vadd.f32 s11, s16, s11
    s[16] = Mul(s[21], s[30]);  // 208FF4 vmul.f32 s16, s21, s30
    s[20] = Add(s[24], s[20]);  // 208FF8 vadd.f32 s20, s24, s20
    s[14] = Add(s[14], s[7]);  // 208FFC vadd.f32 s14, s14, s7
    s[7] = Float(c.Load32(r[0] + 0xA4u));  // 209000 vldr s7, [r0, #164]
    s[24] = Mul(s[26], s[30]);  // 209004 vmul.f32 s24, s26, s30
    s[1] = Mul(s[3], s[7]);  // 209008 vmul.f32 s1, s3, s7
    s[3] = Add(s[15], s[9]);  // 20900C vadd.f32 s3, s15, s9
    s[15] = Float(c.Load32(r[0] + 0x138u));  // 209010 vldr s15, [r0, #312]
    s[6] = Mul(s[6], s[7]);  // 209014 vmul.f32 s6, s6, s7
    s[9] = Mul(s[21], s[28]);  // 209018 vmul.f32 s9, s21, s28
    s[1] = Add(s[1], s[5]);  // 20901C vadd.f32 s1, s1, s5
    s[5] = Add(s[22], s[13]);  // 209020 vadd.f32 s5, s22, s13
    s[22] = Mul(s[26], s[28]);  // 209024 vmul.f32 s22, s26, s28
    s[13] = Mul(s[15], s[28]);  // 209028 vmul.f32 s13, s15, s28
    s[28] = Mul(s[2], s[21]);  // 20902C vmul.f32 s28, s2, s21
    s[26] = Mul(s[26], s[2]);  // 209030 vmul.f32 s26, s26, s2
    s[21] = Mul(s[2], s[15]);  // 209034 vmul.f32 s21, s2, s15
    s[2] = Mul(s[4], s[7]);  // 209038 vmul.f32 s2, s4, s7
    s[6] = Add(s[6], s[12]);  // 20903C vadd.f32 s6, s6, s12
    s[4] = Mul(s[15], s[30]);  // 209040 vmul.f32 s4, s15, s30
    s[3] = Add(s[3], s[22]);  // 209044 vadd.f32 s3, s3, s22
    s[5] = Add(s[5], s[28]);  // 209048 vadd.f32 s5, s5, s28
    s[8] = Add(s[20], s[26]);  // 20904C vadd.f32 s8, s20, s26
    s[12] = Add(s[11], s[21]);  // 209050 vadd.f32 s12, s11, s21
    s[2] = Add(s[2], s[0]);  // 209054 vadd.f32 s2, s2, s0
    s[14] = Add(s[14], s[13]);  // 209058 vadd.f32 s14, s14, s13
    s[10] = Add(s[10], s[9]);  // 20905C vadd.f32 s10, s10, s9
    s[0] = Add(s[1], s[16]);  // 209060 vadd.f32 s0, s1, s16
    s[4] = Add(s[6], s[4]);  // 209064 vadd.f32 s4, s6, s4
    c.Store32(r[0] + 0x84u, Bits(s[8]));  // 209068 vstr s8, [r0, #132]
    c.Store32(r[0] + 0x88u, Bits(s[12]));  // 20906C vstr s12, [r0, #136]
    s[2] = Add(s[2], s[24]);  // 209070 vadd.f32 s2, s2, s24
    c.Store32(r[0] + 0x8Cu, Bits(s[5]));  // 209074 vstr s5, [r0, #140]
    c.Store32(r[0] + 0x94u, Bits(s[3]));  // 209078 vstr s3, [r0, #148]
    c.Store32(r[0] + 0x98u, Bits(s[14]));  // 20907C vstr s14, [r0, #152]
    c.Store32(r[0] + 0x9Cu, Bits(s[10]));  // 209080 vstr s10, [r0, #156]
    c.Store32(r[0] + 0xA4u, Bits(s[2]));  // 209084 vstr s2, [r0, #164]
    c.Store32(r[0] + 0xA8u, Bits(s[4]));  // 209088 vstr s4, [r0, #168]
    c.Store32(r[0] + 0xACu, Bits(s[0]));  // 20908C vstr s0, [r0, #172]
    r[1] = c.Load32(r[13] + 0xD0u);  // 209090 ldr r1, [sp, #0xd0]
L209094:
    r[11] = r[1] + 0x1u;  // 209094 add r11, r1, #1
    c.Store32(r[13] + 0xD0u, r[11]);  // 209098 str r11, [sp, #0xd0]
    r[0] = c.Load32(r[10]);  // 20909C ldr r0, [r10]
    flags = Cmp(r[11], r[0]);  // 2090A0 cmp r11, r0
    if (Pred(flags, 10u)) { Publish<0xFFFFu, 0x0000FFFFFFFFFFFFull>(c, r, s); return; }  // 2090A4 bge 0x209574 <_ZN11rglSkeleton20setup_frames_dynamicEv+0xe94>
    goto L2087C4;  // 2090A8 b 0x2087c4 <_ZN11rglSkeleton20setup_frames_dynamicEv+0xe4>
L2090AC:
    s[0] = Mul(s[16], s[16]);  // 2090AC vmul.f32 s0, s16, s16
    s[4] = Mul(s[30], s[30]);  // 2090B0 vmul.f32 s4, s30, s30
    s[2] = Mul(s[22], s[22]);  // 2090B4 vmul.f32 s2, s22, s22
    s[0] = Add(s[4], s[0]);  // 2090B8 vadd.f32 s0, s4, s0
    s[4] = Mul(s[28], s[28]);  // 2090BC vmul.f32 s4, s28, s28
    s[0] = Add(s[2], s[0]);  // 2090C0 vadd.f32 s0, s2, s0
    s[0] = Add(s[4], s[0]);  // 2090C4 vadd.f32 s0, s4, s0
    r[0] = Bits(s[0]);  // 2090C8 vmov r0, s0
    { r[14] = c.base + 0x2090D0u; Publish<0x0001u, 0x0000000000000000ull>(c, r, s); Sqrt(c); Reload<0x1001u, 0x0000000000000000ull>(c, r, s); }  // 2090CC bl 0x516b8 <.plt+0x998>
    s[8] = Float(r[0]);  // 2090D0 vmov s8, r0
    s[0] = Float(c.Load32(c.base + 0x2090DCu + 0xFFFFFC44u));  // 2090D4 vldr s0, [pc, #-956]
    s[2] = s[17];  // 2090D8 vmov.f32 s2, s17
    c.nzcv = Compare(s[8], s[0]);  // 2090DC vcmpe.f32 s8, s0
    s[4] = s[17];  // 2090E0 vmov.f32 s4, s17
    flags = c.nzcv;  // 2090E4 vmrs APSR_nzcv, fpscr
    s[6] = s[19];  // 2090E8 vmov.f32 s6, s19
    s[0] = s[17];  // 2090EC vmov.f32 s0, s17
    if (Pred(flags, 9u)) goto L209108;  // 2090F0 bls 0x209108 <_ZN11rglSkeleton20setup_frames_dynamicEv+0xa28>
    s[6] = Div(s[19], s[8]);  // 2090F4 vdiv.f32 s6, s19, s8
    s[0] = Mul(s[28], s[6]);  // 2090F8 vmul.f32 s0, s28, s6
    s[2] = Mul(s[22], s[6]);  // 2090FC vmul.f32 s2, s22, s6
    s[4] = Mul(s[30], s[6]);  // 209100 vmul.f32 s4, s30, s6
    s[6] = Mul(s[16], s[6]);  // 209104 vmul.f32 s6, s16, s6
L209108:
    r[0] = c.Load32(r[10] + 0x48u);  // 209108 ldr r0, [r10, #0x48]
    r[0] = r[7] * r[8] + r[0];  // 20910C mla r0, r7, r8, r0
    r[11] = c.Load32(r[13] + 0xD0u);  // 209110 ldr r11, [sp, #0xd0]
    r[2] = c.Load32(r[13] + 0x24u);  // 209114 ldr r2, [sp, #0x24]
    r[1] = c.Load32((r[2] = r[2] + Lsl(r[11], 0x3u)));  // 209118 ldr r1, [r2, r11, lsl #3]!
    r[4] = c.Load32(r[0] + 0x10u);  // 20911C ldr r4, [r0, #0x10]
    r[3] = c.Load32(r[0] + 0x14u);  // 209120 ldr r3, [r0, #0x14]
    r[2] = c.Load32(r[2] + 0x4u);  // 209124 ldr r2, [r2, #0x4]
    r[1] = r[1] & r[4];  // 209128 and r1, r1, r4
    r[2] = r[2] & r[3];  // 20912C and r2, r2, r3
    { r[1] = r[1] | r[2]; flags = Logic(flags, r[1]); }  // 209130 orrs r1, r1, r2
    if (Pred(flags, 1u)) goto L209150;  // 209134 bne 0x209150 <_ZN11rglSkeleton20setup_frames_dynamicEv+0xa70>
    r[7] = r[11];  // 209138 mov r7, r11
    s[16] = s[2];  // 20913C vmov.f32 s16, s2
    s[22] = s[4];  // 209140 vmov.f32 s22, s4
    s[28] = s[6];  // 209144 vmov.f32 s28, s6
    s[30] = s[0];  // 209148 vmov.f32 s30, s0
    goto L209238;  // 20914C b 0x209238 <_ZN11rglSkeleton20setup_frames_dynamicEv+0xb58>
L209150:
    r[1] = c.Load32(r[0] + 0x58u);  // 209150 ldr r1, [r0, #0x58]
    r[1] = r[1] + Lsl(r[11], 0x4u);  // 209154 add r1, r1, r11, lsl #4
    s[14] = Float(c.Load32(r[1] + 0x8u));  // 209158 vldr s14, [r1, #8]
    s[12] = Float(c.Load32(r[1] + 0x4u));  // 20915C vldr s12, [r1, #4]
    s[10] = Float(c.Load32(r[1] + 0xCu));  // 209160 vldr s10, [r1, #12]
    s[15] = Mul(s[2], s[14]);  // 209164 vmul.f32 s15, s2, s14
    s[5] = Mul(s[2], s[12]);  // 209168 vmul.f32 s5, s2, s12
    s[8] = Float(c.Load32(r[1]));  // 20916C vldr s8, [r1]
    s[9] = Mul(s[6], s[10]);  // 209170 vmul.f32 s9, s6, s10
    s[7] = Mul(s[4], s[10]);  // 209174 vmul.f32 s7, s4, s10
    s[1] = Mul(s[6], s[14]);  // 209178 vmul.f32 s1, s6, s14
    s[11] = Mul(s[4], s[12]);  // 20917C vmul.f32 s11, s4, s12
    s[3] = Mul(s[4], s[8]);  // 209180 vmul.f32 s3, s4, s8
    s[16] = Mul(s[4], s[14]);  // 209184 vmul.f32 s16, s4, s14
    s[14] = Mul(s[0], s[14]);  // 209188 vmul.f32 s14, s0, s14
    s[13] = Sub(s[9], s[5]);  // 20918C vsub.f32 s13, s9, s5
    s[15] = Sub(s[15], s[7]);  // 209190 vsub.f32 s15, s15, s7
    s[5] = Mul(s[6], s[8]);  // 209194 vmul.f32 s5, s6, s8
    s[7] = Mul(s[6], s[12]);  // 209198 vmul.f32 s7, s6, s12
    s[9] = Mul(s[2], s[8]);  // 20919C vmul.f32 s9, s2, s8
    s[1] = Sub(s[11], s[1]);  // 2091A0 vsub.f32 s1, s11, s1
    s[12] = Mul(s[0], s[12]);  // 2091A4 vmul.f32 s12, s0, s12
    s[3] = Add(s[3], s[13]);  // 2091A8 vadd.f32 s3, s3, s13
    s[8] = Mul(s[0], s[8]);  // 2091AC vmul.f32 s8, s0, s8
    s[11] = Add(s[15], s[5]);  // 2091B0 vadd.f32 s11, s15, s5
    s[5] = Mul(s[2], s[10]);  // 2091B4 vmul.f32 s5, s2, s10
    s[7] = Add(s[16], s[7]);  // 2091B8 vadd.f32 s7, s16, s7
    s[10] = Mul(s[0], s[10]);  // 2091BC vmul.f32 s10, s0, s10
    s[1] = Add(s[9], s[1]);  // 2091C0 vadd.f32 s1, s9, s1
    s[22] = Add(s[14], s[3]);  // 2091C4 vadd.f32 s22, s14, s3
    s[28] = Add(s[11], s[12]);  // 2091C8 vadd.f32 s28, s11, s12
    s[12] = Add(s[5], s[7]);  // 2091CC vadd.f32 s12, s5, s7
    s[16] = Add(s[10], s[1]);  // 2091D0 vadd.f32 s16, s10, s1
    s[4] = Mul(s[4], s[22]);  // 2091D4 vmul.f32 s4, s4, s22
    s[6] = Mul(s[6], s[28]);  // 2091D8 vmul.f32 s6, s6, s28
    s[30] = Sub(s[8], s[12]);  // 2091DC vsub.f32 s30, s8, s12
    s[2] = Mul(s[2], s[16]);  // 2091E0 vmul.f32 s2, s2, s16
    s[4] = Add(s[6], s[4]);  // 2091E4 vadd.f32 s4, s6, s4
    s[0] = Mul(s[0], s[30]);  // 2091E8 vmul.f32 s0, s0, s30
    s[2] = Add(s[2], s[4]);  // 2091EC vadd.f32 s2, s2, s4
    s[0] = Add(s[0], s[2]);  // 2091F0 vadd.f32 s0, s0, s2
    c.nzcv = Compare(s[0], Float(0x0u));  // 2091F4 vcmpe.f32 s0, #0
    flags = c.nzcv;  // 2091F8 vmrs APSR_nzcv, fpscr
    if (Pred(flags, 4u)) s[30] = Neg(s[30]);  // 2091FC vnegmi.f32 s30, s30
    if (Pred(flags, 4u)) s[16] = Neg(s[16]);  // 209200 vnegmi.f32 s16, s16
    if (Pred(flags, 4u)) s[22] = Neg(s[22]);  // 209204 vnegmi.f32 s22, s22
    if (Pred(flags, 4u)) s[28] = Neg(s[28]);  // 209208 vnegmi.f32 s28, s28
    flags = Cmp(r[11], 0x0u);  // 20920C cmp r11, #0
    if (Pred(flags, 1u)) goto L209234;  // 209210 bne 0x209234 <_ZN11rglSkeleton20setup_frames_dynamicEv+0xb54>
    s[0] = Float(c.Load32(r[0] + 0x5Cu));  // 209214 vldr s0, [r0, #92]
    r[7] = 0x0u;  // 209218 mov r7, #0
    s[2] = Float(c.Load32(r[0] + 0x60u));  // 20921C vldr s2, [r0, #96]
    s[4] = Float(c.Load32(r[0] + 0x64u));  // 209220 vldr s4, [r0, #100]
    s[27] = Add(s[27], s[0]);  // 209224 vadd.f32 s27, s27, s0
    s[25] = Add(s[25], s[2]);  // 209228 vadd.f32 s25, s25, s2
    s[23] = Add(s[23], s[4]);  // 20922C vadd.f32 s23, s23, s4
    goto L209238;  // 209230 b 0x209238 <_ZN11rglSkeleton20setup_frames_dynamicEv+0xb58>
L209234:
    r[7] = r[11];  // 209234 mov r7, r11
L209238:
    r[1] = c.Load32(r[10] + 0x4u);  // 209238 ldr r1, [r10, #0x4]
    s[4] = Float(0x41F00000u);  // 20923C vmov.f32 s4, #3.000000e+01
    r[1] = r[7] * r[9] + r[1];  // 209240 mla r1, r7, r9, r1
    s[0] = Float(c.Load32(r[10] + 0x5Cu));  // 209244 vldr s0, [r10, #92]
    s[6] = Float(c.Load32(r[0]));  // 209248 vldr s6, [r0]
    r[2] = c.Load8(r[0] + 0x1Cu);  // 20924C ldrb r2, [r0, #0x1c]
    s[20] = s[19];  // 209250 vmov.f32 s20, s19
    r[0] = c.Load8(r[0] + 0x1Du);  // 209254 ldrb r0, [r0, #0x1d]
    flags = Cmp(r[2], 0x0u);  // 209258 cmp r2, #0
    s[2] = Float(c.Load32(r[1] + 0x208u));  // 20925C vldr s2, [r1, #520]
    s[4] = Mul(s[0], s[4]);  // 209260 vmul.f32 s4, s0, s4
    s[2] = Mul(s[6], s[2]);  // 209264 vmul.f32 s2, s6, s2
    if (Pred(flags, 1u)) s[6] = Add(s[6], s[17]);  // 209268 vaddne.f32 s6, s6, s17
    s[4] = Mul(s[2], s[4]);  // 20926C vmul.f32 s4, s2, s4
    if (Pred(flags, 1u)) s[4] = Mul(s[6], s[4]);  // 209270 vmulne.f32 s4, s6, s4
    flags = Cmp(r[0], 0x0u);  // 209274 cmp r0, #0
    if (Pred(flags, 1u)) goto L20929C;  // 209278 bne 0x20929c <_ZN11rglSkeleton20setup_frames_dynamicEv+0xbbc>
    s[20] = s[17];  // 20927C vmov.f32 s20, s17
    c.nzcv = Compare(s[4], Float(0x0u));  // 209280 vcmpe.f32 s4, #0
    flags = c.nzcv;  // 209284 vmrs APSR_nzcv, fpscr
    if (Pred(flags, 4u)) goto L20929C;  // 209288 bmi 0x20929c <_ZN11rglSkeleton20setup_frames_dynamicEv+0xbbc>
    c.nzcv = Compare(s[4], s[19]);  // 20928C vcmpe.f32 s4, s19
    flags = c.nzcv;  // 209290 vmrs APSR_nzcv, fpscr
    s[20] = s[4];  // 209294 vmov.f32 s20, s4
    if (Pred(flags, 12u)) s[20] = s[19];  // 209298 vmovgt.f32 s20, s19
L20929C:
    s[4] = Float(c.Load32(c.base + 0x2092A4u + 0x300u));  // 20929C vldr s4, [pc, #768]
    c.nzcv = Compare(s[2], s[4]);  // 2092A0 vcmpe.f32 s2, s4
    flags = c.nzcv;  // 2092A4 vmrs APSR_nzcv, fpscr
    if (Pred(flags, 9u)) goto L2092C0;  // 2092A8 bls 0x2092c0 <_ZN11rglSkeleton20setup_frames_dynamicEv+0xbe0>
    c.Store32(r[1] + 0xC4u, Bits(s[30]));  // 2092AC vstr s30, [r1, #196]
    c.Store32(r[1] + 0xC8u, Bits(s[28]));  // 2092B0 vstr s28, [r1, #200]
    c.Store32(r[1] + 0xCCu, Bits(s[22]));  // 2092B4 vstr s22, [r1, #204]
    c.Store32(r[1] + 0xD0u, Bits(s[16]));  // 2092B8 vstr s16, [r1, #208]
    goto L209528;  // 2092BC b 0x209528 <_ZN11rglSkeleton20setup_frames_dynamicEv+0xe48>
L2092C0:
    s[2] = Float(c.Load32(c.base + 0x2092C8u + 0x2E0u));  // 2092C0 vldr s2, [pc, #736]
    s[29] = s[18];  // 2092C4 vmov.f32 s29, s18
    s[18] = s[23];  // 2092C8 vmov.f32 s18, s23
    s[21] = Float(c.Load32(r[1] + 0xF8u));  // 2092CC vldr s21, [r1, #248]
    s[0] = Mul(s[0], s[2]);  // 2092D0 vmul.f32 s0, s0, s2
    s[2] = Float(c.Load32(c.base + 0x2092DCu + 0x2D0u));  // 2092D4 vldr s2, [pc, #720]
    s[23] = Float(c.Load32(r[1] + 0xFCu));  // 2092D8 vldr s23, [r1, #252]
    s[24] = s[27];  // 2092DC vmov.f32 s24, s27
    s[4] = Mul(s[21], s[21]);  // 2092E0 vmul.f32 s4, s21, s21
    s[31] = s[25];  // 2092E4 vmov.f32 s31, s25
    s[25] = Float(c.Load32(r[1] + 0x100u));  // 2092E8 vldr s25, [r1, #256]
    s[0] = Add(s[0], s[2]);  // 2092EC vadd.f32 s0, s0, s2
    s[2] = Float(c.Load32(r[1] + 0xF4u));  // 2092F0 vldr s2, [r1, #244]
    c.nzcv = Compare(s[2], Float(0x0u));  // 2092F4 vcmpe.f32 s2, #0
    flags = c.nzcv;  // 2092F8 vmrs APSR_nzcv, fpscr
    if (Pred(flags, 13u)) s[27] = Sub(s[2], s[0]);  // 2092FC vsuble.f32 s27, s2, s0
    if (Pred(flags, 12u)) s[27] = Add(s[2], s[0]);  // 209300 vaddgt.f32 s27, s2, s0
    s[0] = Mul(s[23], s[23]);  // 209304 vmul.f32 s0, s23, s23
    s[2] = Mul(s[25], s[25]);  // 209308 vmul.f32 s2, s25, s25
    s[0] = Add(s[4], s[0]);  // 20930C vadd.f32 s0, s4, s0
    s[4] = Mul(s[27], s[27]);  // 209310 vmul.f32 s4, s27, s27
    s[0] = Add(s[0], s[2]);  // 209314 vadd.f32 s0, s0, s2
    s[0] = Add(s[0], s[4]);  // 209318 vadd.f32 s0, s0, s4
    r[0] = Bits(s[0]);  // 20931C vmov r0, s0
    { r[14] = c.base + 0x209324u; Publish<0x0001u, 0x0000000000000000ull>(c, r, s); Sqrt(c); Reload<0x1001u, 0x0000000000000000ull>(c, r, s); }  // 209320 bl 0x516b8 <.plt+0x998>
    s[8] = Float(r[0]);  // 209324 vmov s8, r0
    s[0] = Float(c.Load32(c.base + 0x209330u + 0x268u));  // 209328 vldr s0, [pc, #616]
    s[4] = s[17];  // 20932C vmov.f32 s4, s17
    c.nzcv = Compare(s[8], s[0]);  // 209330 vcmpe.f32 s8, s0
    s[0] = s[17];  // 209334 vmov.f32 s0, s17
    flags = c.nzcv;  // 209338 vmrs APSR_nzcv, fpscr
    s[6] = s[19];  // 20933C vmov.f32 s6, s19
    s[2] = s[17];  // 209340 vmov.f32 s2, s17
    s[9] = s[17];  // 209344 vmov.f32 s9, s17
    if (Pred(flags, 9u)) goto L209360;  // 209348 bls 0x209360 <_ZN11rglSkeleton20setup_frames_dynamicEv+0xc80>
    s[6] = Div(s[19], s[8]);  // 20934C vdiv.f32 s6, s19, s8
    s[2] = Mul(s[27], s[6]);  // 209350 vmul.f32 s2, s27, s6
    s[0] = Mul(s[25], s[6]);  // 209354 vmul.f32 s0, s25, s6
    s[4] = Mul(s[23], s[6]);  // 209358 vmul.f32 s4, s23, s6
    s[6] = Mul(s[21], s[6]);  // 20935C vmul.f32 s6, s21, s6
L209360:
    s[15] = Float(c.Load32(r[13] + 0x34u));  // 209360 vldr s15, [sp, #52]
    s[10] = Mul(s[26], s[4]);  // 209364 vmul.f32 s10, s26, s4
    s[11] = Float(c.Load32(r[13] + 0x3Cu));  // 209368 vldr s11, [sp, #60]
    s[7] = s[19];  // 20936C vmov.f32 s7, s19
    s[13] = Float(c.Load32(r[13] + 0x38u));  // 209370 vldr s13, [sp, #56]
    s[12] = Mul(s[15], s[0]);  // 209374 vmul.f32 s12, s15, s0
    s[1] = Mul(s[11], s[6]);  // 209378 vmul.f32 s1, s11, s6
    r[0] = c.Load32(r[10] + 0x4u);  // 20937C ldr r0, [r10, #0x4]
    s[3] = Mul(s[11], s[4]);  // 209380 vmul.f32 s3, s11, s4
    r[0] = r[7] * r[9] + r[0];  // 209384 mla r0, r7, r9, r0
    s[5] = Mul(s[13], s[0]);  // 209388 vmul.f32 s5, s13, s0
    s[8] = Mul(s[13], s[6]);  // 20938C vmul.f32 s8, s13, s6
    s[14] = Mul(s[15], s[4]);  // 209390 vmul.f32 s14, s15, s4
    s[19] = s[7];  // 209394 vmov.f32 s19, s7
    s[23] = s[18];  // 209398 vmov.f32 s23, s18
    s[1] = Sub(s[1], s[12]);  // 20939C vsub.f32 s1, s1, s12
    s[12] = Mul(s[26], s[6]);  // 2093A0 vmul.f32 s12, s26, s6
    s[3] = Sub(s[5], s[3]);  // 2093A4 vsub.f32 s3, s5, s3
    s[5] = Mul(s[13], s[4]);  // 2093A8 vmul.f32 s5, s13, s4
    s[6] = Mul(s[15], s[6]);  // 2093AC vmul.f32 s6, s15, s6
    s[8] = Sub(s[14], s[8]);  // 2093B0 vsub.f32 s8, s14, s8
    s[4] = Mul(s[26], s[0]);  // 2093B4 vmul.f32 s4, s26, s0
    s[10] = Add(s[10], s[1]);  // 2093B8 vadd.f32 s10, s10, s1
    s[14] = Mul(s[13], s[2]);  // 2093BC vmul.f32 s14, s13, s2
    s[3] = Add(s[3], s[12]);  // 2093C0 vadd.f32 s3, s3, s12
    s[1] = Mul(s[15], s[2]);  // 2093C4 vmul.f32 s1, s15, s2
    s[12] = Mul(s[11], s[0]);  // 2093C8 vmul.f32 s12, s11, s0
    s[6] = Add(s[5], s[6]);  // 2093CC vadd.f32 s6, s5, s6
    s[8] = Add(s[4], s[8]);  // 2093D0 vadd.f32 s8, s4, s8
    s[5] = Mul(s[11], s[2]);  // 2093D4 vmul.f32 s5, s11, s2
    s[0] = Add(s[14], s[10]);  // 2093D8 vadd.f32 s0, s14, s10
    s[10] = Mul(s[26], s[2]);  // 2093DC vmul.f32 s10, s26, s2
    s[4] = Add(s[3], s[1]);  // 2093E0 vadd.f32 s4, s3, s1
    s[14] = Mul(s[22], s[20]);  // 2093E4 vmul.f32 s14, s22, s20
    s[6] = Add(s[12], s[6]);  // 2093E8 vadd.f32 s6, s12, s6
    s[1] = Mul(s[16], s[20]);  // 2093EC vmul.f32 s1, s16, s20
    s[2] = Add(s[5], s[8]);  // 2093F0 vadd.f32 s2, s5, s8
    s[8] = Mul(s[22], s[0]);  // 2093F4 vmul.f32 s8, s22, s0
    s[18] = s[29];  // 2093F8 vmov.f32 s18, s29
    s[12] = Mul(s[28], s[4]);  // 2093FC vmul.f32 s12, s28, s4
    s[25] = s[31];  // 209400 vmov.f32 s25, s31
    s[6] = Sub(s[10], s[6]);  // 209404 vsub.f32 s6, s10, s6
    s[27] = s[24];  // 209408 vmov.f32 s27, s24
    s[10] = Mul(s[16], s[2]);  // 20940C vmul.f32 s10, s16, s2
    s[17] = s[9];  // 209410 vmov.f32 s17, s9
    s[8] = Add(s[12], s[8]);  // 209414 vadd.f32 s8, s12, s8
    s[12] = Mul(s[30], s[6]);  // 209418 vmul.f32 s12, s30, s6
    s[8] = Add(s[10], s[8]);  // 20941C vadd.f32 s8, s10, s8
    s[10] = Sub(s[19], s[20]);  // 209420 vsub.f32 s10, s19, s20
    s[8] = Add(s[12], s[8]);  // 209424 vadd.f32 s8, s12, s8
    c.nzcv = Compare(s[8], Float(0x0u));  // 209428 vcmpe.f32 s8, #0
    flags = c.nzcv;  // 20942C vmrs APSR_nzcv, fpscr
    s[8] = Mul(s[30], s[20]);  // 209430 vmul.f32 s8, s30, s20
    if (Pred(flags, 4u)) s[2] = Neg(s[2]);  // 209434 vnegmi.f32 s2, s2
    if (Pred(flags, 4u)) s[0] = Neg(s[0]);  // 209438 vnegmi.f32 s0, s0
    if (Pred(flags, 4u)) s[4] = Neg(s[4]);  // 20943C vnegmi.f32 s4, s4
    if (Pred(flags, 4u)) s[6] = Neg(s[6]);  // 209440 vnegmi.f32 s6, s6
    s[2] = Mul(s[10], s[2]);  // 209444 vmul.f32 s2, s10, s2
    s[0] = Mul(s[10], s[0]);  // 209448 vmul.f32 s0, s10, s0
    s[4] = Mul(s[10], s[4]);  // 20944C vmul.f32 s4, s10, s4
    s[12] = Mul(s[10], s[6]);  // 209450 vmul.f32 s12, s10, s6
    s[6] = Mul(s[28], s[20]);  // 209454 vmul.f32 s6, s28, s20
    s[2] = Add(s[1], s[2]);  // 209458 vadd.f32 s2, s1, s2
    s[0] = Add(s[14], s[0]);  // 20945C vadd.f32 s0, s14, s0
    s[8] = Add(s[8], s[12]);  // 209460 vadd.f32 s8, s8, s12
    s[4] = Add(s[6], s[4]);  // 209464 vadd.f32 s4, s6, s4
    c.Store32(r[0] + 0xC4u, Bits(s[8]));  // 209468 vstr s8, [r0, #196]
    c.Store32(r[0] + 0xC8u, Bits(s[4]));  // 20946C vstr s4, [r0, #200]
    c.Store32(r[0] + 0xCCu, Bits(s[0]));  // 209470 vstr s0, [r0, #204]
    c.Store32(r[0] + 0xD0u, Bits(s[2]));  // 209474 vstr s2, [r0, #208]
    r[0] = c.Load32(r[10] + 0x4u);  // 209478 ldr r0, [r10, #0x4]
    r[5] = r[7] * r[9] + r[0];  // 20947C mla r5, r7, r9, r0
    s[6] = Float(c.Load32(r[5] + 0xCCu));  // 209480 vldr s6, [r5, #204]
    s[2] = Float(c.Load32(r[5] + 0xC8u));  // 209484 vldr s2, [r5, #200]
    s[6] = Mul(s[6], s[6]);  // 209488 vmul.f32 s6, s6, s6
    s[4] = Float(c.Load32(r[5] + 0xD0u));  // 20948C vldr s4, [r5, #208]
    s[8] = Mul(s[2], s[2]);  // 209490 vmul.f32 s8, s2, s2
    s[0] = Float(c.Load32(r[5] + 0xC4u));  // 209494 vldr s0, [r5, #196]
    s[2] = Mul(s[4], s[4]);  // 209498 vmul.f32 s2, s4, s4
    s[0] = Mul(s[0], s[0]);  // 20949C vmul.f32 s0, s0, s0
    s[4] = Add(s[8], s[6]);  // 2094A0 vadd.f32 s4, s8, s6
    s[2] = Add(s[4], s[2]);  // 2094A4 vadd.f32 s2, s4, s2
    s[0] = Add(s[2], s[0]);  // 2094A8 vadd.f32 s0, s2, s0
    r[0] = Bits(s[0]);  // 2094AC vmov r0, s0
    { r[14] = c.base + 0x2094B4u; Publish<0x0001u, 0x0000000000000000ull>(c, r, s); Sqrt(c); Reload<0x1001u, 0x0000000000000000ull>(c, r, s); }  // 2094B0 bl 0x516b8 <.plt+0x998>
    s[0] = Float(r[0]);  // 2094B4 vmov s0, r0
    s[2] = Float(c.Load32(c.base + 0x2094C0u + 0xD8u));  // 2094B8 vldr s2, [pc, #216]
    r[1] = r[5] + 0xC4u;  // 2094BC add r1, r5, #196
    r[2] = r[5] + 0xD0u;  // 2094C0 add r2, r5, #208
    r[3] = r[5] + 0xCCu;  // 2094C4 add r3, r5, #204
    r[0] = r[5] + 0xC8u;  // 2094C8 add r0, r5, #200
    c.nzcv = Compare(s[0], s[2]);  // 2094CC vcmpe.f32 s0, s2
    flags = c.nzcv;  // 2094D0 vmrs APSR_nzcv, fpscr
    if (Pred(flags, 9u)) goto L209510;  // 2094D4 bls 0x209510 <_ZN11rglSkeleton20setup_frames_dynamicEv+0xe30>
    s[0] = Div(s[19], s[0]);  // 2094D8 vdiv.f32 s0, s19, s0
    s[2] = Float(c.Load32(r[0]));  // 2094DC vldr s2, [r0]
    s[2] = Mul(s[0], s[2]);  // 2094E0 vmul.f32 s2, s0, s2
    c.Store32(r[0], Bits(s[2]));  // 2094E4 vstr s2, [r0]
    s[2] = Float(c.Load32(r[3]));  // 2094E8 vldr s2, [r3]
    s[2] = Mul(s[0], s[2]);  // 2094EC vmul.f32 s2, s0, s2
    c.Store32(r[3], Bits(s[2]));  // 2094F0 vstr s2, [r3]
    s[2] = Float(c.Load32(r[2]));  // 2094F4 vldr s2, [r2]
    s[2] = Mul(s[0], s[2]);  // 2094F8 vmul.f32 s2, s0, s2
    c.Store32(r[2], Bits(s[2]));  // 2094FC vstr s2, [r2]
    s[2] = Float(c.Load32(r[1]));  // 209500 vldr s2, [r1]
    s[0] = Mul(s[0], s[2]);  // 209504 vmul.f32 s0, s0, s2
    c.Store32(r[1], Bits(s[0]));  // 209508 vstr s0, [r1]
    goto L209528;  // 20950C b 0x209528 <_ZN11rglSkeleton20setup_frames_dynamicEv+0xe48>
L209510:
    r[6] = 0x3F800000u;  // 209510 mov r6, #1065353216
    r[12] = 0x0u;  // 209514 mov r12, #0
    c.Store32(r[0], r[6]);  // 209518 str r6, [r0]
    c.Store32(r[3], r[12]);  // 20951C str r12, [r3]
    c.Store32(r[2], r[12]);  // 209520 str r12, [r2]
    c.Store32(r[1], Bits(s[17]));  // 209524 vstr s17, [r1]
L209528:
    flags = Cmp(r[7], 0x0u);  // 209528 cmp r7, #0
    if (Pred(flags, 1u)) goto L208C50;  // 20952C bne 0x208c50 <_ZN11rglSkeleton20setup_frames_dynamicEv+0x570>
    s[0] = Sub(s[19], s[20]);  // 209530 vsub.f32 s0, s19, s20
    s[8] = Float(c.Load32(r[10] + 0x1Cu));  // 209534 vldr s8, [r10, #28]
    s[6] = Float(c.Load32(r[10] + 0x20u));  // 209538 vldr s6, [r10, #32]
    s[4] = Mul(s[27], s[20]);  // 20953C vmul.f32 s4, s27, s20
    s[2] = Float(c.Load32(r[10] + 0x24u));  // 209540 vldr s2, [r10, #36]
    s[10] = Mul(s[25], s[20]);  // 209544 vmul.f32 s10, s25, s20
    s[12] = Mul(s[23], s[20]);  // 209548 vmul.f32 s12, s23, s20
    s[8] = Mul(s[0], s[8]);  // 20954C vmul.f32 s8, s0, s8
    s[6] = Mul(s[0], s[6]);  // 209550 vmul.f32 s6, s0, s6
    s[0] = Mul(s[0], s[2]);  // 209554 vmul.f32 s0, s0, s2
    s[2] = Add(s[4], s[8]);  // 209558 vadd.f32 s2, s4, s8
    s[4] = Add(s[10], s[6]);  // 20955C vadd.f32 s4, s10, s6
    s[0] = Add(s[12], s[0]);  // 209560 vadd.f32 s0, s12, s0
    c.Store32(r[10] + 0xCu, Bits(s[2]));  // 209564 vstr s2, [r10, #12]
    c.Store32(r[10] + 0x10u, Bits(s[4]));  // 209568 vstr s4, [r10, #16]
    c.Store32(r[10] + 0x14u, Bits(s[0]));  // 20956C vstr s0, [r10, #20]
    goto L208C50;  // 209570 b 0x208c50 <_ZN11rglSkeleton20setup_frames_dynamicEv+0x570>
}

inline void Evaluate(Context& c) {
    auto r = c.r;
    auto s = c.s;
    auto& flags = c.flags;
    { r[13] -= 36u; c.Store32(r[13] + 0u, r[4]); c.Store32(r[13] + 4u, r[5]); c.Store32(r[13] + 8u, r[6]); c.Store32(r[13] + 12u, r[7]); c.Store32(r[13] + 16u, r[8]); c.Store32(r[13] + 20u, r[9]); c.Store32(r[13] + 24u, r[10]); c.Store32(r[13] + 28u, r[11]); c.Store32(r[13] + 32u, r[14]); }  // 20AC20 push {r4, r5, r6, r7, r8, r9, r10, r11, lr}
    r[13] = r[13] - 0xCu;  // 20AC24 sub sp, sp, #12
    r[8] = r[2];  // 20AC28 mov r8, r2
    r[11] = r[0];  // 20AC2C mov r11, r0
    r[0] = c.Load32(r[8]);  // 20AC30 ldr r0, [r8]
    r[10] = r[1];  // 20AC34 mov r10, r1
    r[1] = 0xC8u;  // 20AC38 mov r1, #200
    r[6] = c.Load32(r[10] + 0x48u);  // 20AC3C ldr r6, [r10, #0x48]
    r[9] = r[0] * r[1] + r[6];  // 20AC40 mla r9, r0, r1, r6
    r[1] = c.Load32((r[9] = r[9] + 0x4Cu));  // 20AC44 ldr r1, [r9, #0x4c]!
    flags = Cmp(r[1], 0x0u);  // 20AC48 cmp r1, #0
    if (Pred(flags, 0u)) goto L20AC7C;  // 20AC4C beq 0x20ac7c <_ZN11rglSkeleton17evaluate_intervalERKiS1_S1_+0x5c>
    r[2] = c.Load32(r[3]);  // 20AC50 ldr r2, [r3]
    r[4] = r[3];  // 20AC54 mov r4, r3
    r[5] = c.Load32(r[13] + 0x30u);  // 20AC58 ldr r5, [sp, #0x30]
    flags = Cmp(r[2], 0x0u);  // 20AC5C cmp r2, #0
    if (Pred(flags, 0u)) goto L20AC98;  // 20AC60 beq 0x20ac98 <_ZN11rglSkeleton17evaluate_intervalERKiS1_S1_+0x78>
    r[7] = 0xC8u;  // 20AC64 mov r7, #200
    r[7] = r[0] * r[7] + r[6];  // 20AC68 mla r7, r0, r7, r6
    r[7] = r[7] + Lsl(r[2], 0x2u);  // 20AC6C add r7, r7, r2, lsl #2
    s[0] = Float(c.Load32(r[7] + 0xBCu));  // 20AC70 vldr s0, [r7, #188]
    s[0] = SignedFloat(Bits(s[0]));  // 20AC74 vcvt.f32.s32 s0, s0
    goto L20AC9C;  // 20AC78 b 0x20ac9c <_ZN11rglSkeleton17evaluate_intervalERKiS1_S1_+0x7c>
L20AC7C:
    r[0] = 0x0u;  // 20AC7C mov r0, #0
    r[1] = 0x3F800000u;  // 20AC80 mov r1, #1065353216
    c.Store32(r[11], r[0]);  // 20AC84 str r0, [r11]
    c.Store32(r[11] + 4u, r[0]); c.Store32(r[11] + 8u, r[1]);  // 20AC88 stmib r11, {r0, r1}
    c.Store32(r[11] + 0xCu, r[0]);  // 20AC8C str r0, [r11, #0xc]
    r[13] = r[13] + 0xCu;  // 20AC90 add sp, sp, #12
    { r[4] = c.Load32(r[13] + 0u); r[5] = c.Load32(r[13] + 4u); r[6] = c.Load32(r[13] + 8u); r[7] = c.Load32(r[13] + 12u); r[8] = c.Load32(r[13] + 16u); r[9] = c.Load32(r[13] + 20u); r[10] = c.Load32(r[13] + 24u); r[11] = c.Load32(r[13] + 28u); r[15] = c.Load32(r[13] + 32u); r[13] += 36u; Publish<0xFFFFu, 0x0000000F0000557Full>(c, r, s); return; }  // 20AC94 pop {r4, r5, r6, r7, r8, r9, r10, r11, pc}
L20AC98:
    s[0] = Float(c.Load32(c.base + 0x20ACA0u + 0x224u));  // 20AC98 vldr s0, [pc, #548]
L20AC9C:
    r[7] = 0xC8u;  // 20AC9C mov r7, #200
    r[3] = r[0] * r[7] + r[6];  // 20ACA0 mla r3, r0, r7, r6
    r[6] = c.Load32(r[3] + 0x8Cu);  // 20ACA4 ldr r6, [r3, #0x8c]
    s[6] = Float(c.Load32(r[3] + 0x18u));  // 20ACA8 vldr s6, [r3, #24]
    r[7] = c.Load32(r[6] + 0x8u);  // 20ACAC ldr r7, [r6, #0x8]
    r[6] = c.Load32(r[6] + 0xCu);  // 20ACB0 ldr r6, [r6, #0xc]
    s[2] = Float(r[7]);  // 20ACB4 vmov s2, r7
    r[7] = r[6] - r[7];  // 20ACB8 sub r7, r6, r7
    r[6] = 0x214u;  // 20ACBC mov r6, #532
    s[4] = Float(r[7]);  // 20ACC0 vmov s4, r7
    s[2] = SignedFloat(Bits(s[2]));  // 20ACC4 vcvt.f32.s32 s2, s2
    s[4] = SignedFloat(Bits(s[4]));  // 20ACC8 vcvt.f32.s32 s4, s4
    s[0] = Add(s[0], s[2]);  // 20ACCC vadd.f32 s0, s0, s2
    s[2] = Mul(s[6], s[4]);  // 20ACD0 vmul.f32 s2, s6, s4
    s[0] = Add(s[0], s[2]);  // 20ACD4 vadd.f32 s0, s0, s2
    c.Store32(r[13] + 0x8u, Bits(s[0]));  // 20ACD8 vstr s0, [sp, #8]
    r[3] = c.Load32(r[5]);  // 20ACDC ldr r3, [r5]
    r[7] = c.Load32(r[10] + 0x4u);  // 20ACE0 ldr r7, [r10, #0x4]
    r[7] = r[3] * r[6] + r[7];  // 20ACE4 mla r7, r3, r6, r7
    r[0] = r[7] + Lsl(r[0], 0x3u);  // 20ACE8 add r0, r7, r0, lsl #3
    r[0] = r[0] + Lsl(r[2], 0x2u);  // 20ACEC add r0, r0, r2, lsl #2
    r[2] = r[13] + 0x4u;  // 20ACF0 add r2, sp, #4
    r[0] = c.Load32(r[0] + 0x148u);  // 20ACF4 ldr r0, [r0, #0x148]
    c.Store32(r[13], r[0]);  // 20ACF8 str r0, [sp]
    r[0] = c.Load32(r[1] + 0xA0u);  // 20ACFC ldr r0, [r1, #0xa0]
    r[1] = Lsl(r[3], 0x3u) - r[3];  // 20AD00 rsb r1, r3, r3, lsl #3
    r[3] = r[13] + 0x8u;  // 20AD04 add r3, sp, #8
    r[0] = r[0] + Lsl(r[1], 0x2u);  // 20AD08 add r0, r0, r1, lsl #2
    r[1] = r[13];  // 20AD0C mov r1, sp
    { r[14] = c.base + 0x20AD14u; Publish<0xF8FFu, 0x0000000000000015ull>(c, r, s); KeyQuat(c); Reload<0xF8FBu, 0x0000000000000015ull>(c, r, s); }  // 20AD10 bl 0x20aad0 <_ZNK6rglIpoI13rglQuaternionE16get_interval_keyERiRfRKf>
    r[14] = r[5];  // 20AD14 mov lr, r5
    r[1] = c.Load32(r[10] + 0x4u);  // 20AD18 ldr r1, [r10, #0x4]
    r[2] = c.Load32(r[14]);  // 20AD1C ldr r2, [lr]
    r[12] = r[4];  // 20AD20 mov r12, r4
    r[3] = c.Load32(r[8]);  // 20AD24 ldr r3, [r8]
    r[4] = c.Load32(r[12]);  // 20AD28 ldr r4, [r12]
    r[7] = r[2] * r[6] + r[1];  // 20AD2C mla r7, r2, r6, r1
    r[0] = c.Load32(r[13]);  // 20AD30 ldr r0, [sp]
    r[6] = r[7] + Lsl(r[3], 0x3u);  // 20AD34 add r6, r7, r3, lsl #3
    r[6] = r[6] + Lsl(r[4], 0x2u);  // 20AD38 add r6, r6, r4, lsl #2
    r[5] = c.Load32((r[6] = r[6] + 0x148u));  // 20AD3C ldr r5, [r6, #0x148]!
    flags = Cmp(r[0], r[5]);  // 20AD40 cmp r0, r5
    if (Pred(flags, 1u)) goto L20AD5C;  // 20AD44 bne 0x20ad5c <_ZN11rglSkeleton17evaluate_intervalERKiS1_S1_+0x13c>
    r[7] = r[7] + Lsl(r[3], 0x2u);  // 20AD48 add r7, r7, r3, lsl #2
    r[5] = c.Load32(r[9]);  // 20AD4C ldr r5, [r9]
    r[7] = c.Load32(r[7] + 0x158u);  // 20AD50 ldr r7, [r7, #0x158]
    flags = Cmp(r[5], r[7]);  // 20AD54 cmp r5, r7
    if (Pred(flags, 0u)) goto L20AE0C;  // 20AD58 beq 0x20ae0c <_ZN11rglSkeleton17evaluate_intervalERKiS1_S1_+0x1ec>
L20AD5C:
    c.Store32(r[6], r[0]);  // 20AD5C str r0, [r6]
    r[4] = 0x214u;  // 20AD60 mov r4, #532
    r[1] = c.Load32(r[14]);  // 20AD64 ldr r1, [lr]
    r[2] = c.Load32(r[10] + 0x4u);  // 20AD68 ldr r2, [r10, #0x4]
    r[3] = c.Load32(r[9]);  // 20AD6C ldr r3, [r9]
    r[1] = r[1] * r[4] + r[2];  // 20AD70 mla r1, r1, r4, r2
    r[2] = c.Load32(r[8]);  // 20AD74 ldr r2, [r8]
    r[1] = r[1] + Lsl(r[2], 0x2u);  // 20AD78 add r1, r1, r2, lsl #2
    c.Store32(r[1] + 0x158u, r[3]);  // 20AD7C str r3, [r1, #0x158]
    r[3] = c.Load32(r[9]);  // 20AD80 ldr r3, [r9]
    r[2] = c.Load32(r[14]);  // 20AD84 ldr r2, [lr]
    r[1] = c.Load32(r[10] + 0x4u);  // 20AD88 ldr r1, [r10, #0x4]
    r[3] = c.Load32(r[3] + 0xA0u);  // 20AD8C ldr r3, [r3, #0xa0]
    r[1] = r[2] * r[4] + r[1];  // 20AD90 mla r1, r2, r4, r1
    r[2] = Lsl(r[2], 0x3u) - r[2];  // 20AD94 rsb r2, r2, r2, lsl #3
    r[2] = c.Load32(r[3] + Lsl(r[2], 0x2u));  // 20AD98 ldr r2, [r3, r2, lsl #2]
    r[3] = c.Load32(r[8]);  // 20AD9C ldr r3, [r8]
    r[2] = r[2] + Lsl(r[0], 0x4u);  // 20ADA0 add r2, r2, r0, lsl #4
    std::memcpy(s.data() + 32, c.guest + r[2], 16);  // 20ADA4 vld1.32 {d16, d17}, [r2]
    r[1] = r[1] + Lsl(r[3], 0x5u);  // 20ADA8 add r1, r1, r3, lsl #5
    r[3] = c.Load32(r[12]);  // 20ADAC ldr r3, [r12]
    r[1] = r[1] + Lsl(r[3], 0x4u);  // 20ADB0 add r1, r1, r3, lsl #4
    r[1] = r[1] + 0x160u;  // 20ADB4 add r1, r1, #352
    std::memcpy(c.guest + r[1], s.data() + 32, 16);  // 20ADB8 vst1.32 {d16, d17}, [r1]
    r[1] = c.Load32(r[9]);  // 20ADBC ldr r1, [r9]
    r[3] = c.Load32(r[14]);  // 20ADC0 ldr r3, [lr]
    r[2] = c.Load32(r[10] + 0x4u);  // 20ADC4 ldr r2, [r10, #0x4]
    r[1] = c.Load32(r[1] + 0xA0u);  // 20ADC8 ldr r1, [r1, #0xa0]
    r[2] = r[3] * r[4] + r[2];  // 20ADCC mla r2, r3, r4, r2
    r[7] = Lsl(r[3], 0x3u) - r[3];  // 20ADD0 rsb r7, r3, r3, lsl #3
    r[3] = c.Load32(r[8]);  // 20ADD4 ldr r3, [r8]
    r[1] = c.Load32(r[1] + Lsl(r[7], 0x2u));  // 20ADD8 ldr r1, [r1, r7, lsl #2]
    r[7] = c.Load32(r[12]);  // 20ADDC ldr r7, [r12]
    r[0] = r[1] + Lsl(r[0], 0x4u);  // 20ADE0 add r0, r1, r0, lsl #4
    r[2] = r[2] + Lsl(r[3], 0x5u);  // 20ADE4 add r2, r2, r3, lsl #5
    r[0] = r[0] + 0x10u;  // 20ADE8 add r0, r0, #16
    r[2] = r[2] + Lsl(r[7], 0x4u);  // 20ADEC add r2, r2, r7, lsl #4
    std::memcpy(s.data() + 32, c.guest + r[0], 16);  // 20ADF0 vld1.32 {d16, d17}, [r0]
    r[0] = r[2] + 0x1A0u;  // 20ADF4 add r0, r2, #416
    std::memcpy(c.guest + r[0], s.data() + 32, 16);  // 20ADF8 vst1.32 {d16, d17}, [r0]
    r[1] = c.Load32(r[10] + 0x4u);  // 20ADFC ldr r1, [r10, #0x4]
    r[2] = c.Load32(r[14]);  // 20AE00 ldr r2, [lr]
    r[3] = c.Load32(r[8]);  // 20AE04 ldr r3, [r8]
    r[4] = c.Load32(r[12]);  // 20AE08 ldr r4, [r12]
L20AE0C:
    r[0] = 0x214u;  // 20AE0C mov r0, #532
    r[0] = r[2] * r[0] + r[1];  // 20AE10 mla r0, r2, r0, r1
    r[0] = r[0] + Lsl(r[3], 0x5u);  // 20AE14 add r0, r0, r3, lsl #5
    r[0] = r[0] + Lsl(r[4], 0x4u);  // 20AE18 add r0, r0, r4, lsl #4
    s[4] = Float(c.Load32(r[0] + 0x164u));  // 20AE1C vldr s4, [r0, #356]
    s[10] = Float(c.Load32(r[0] + 0x1A4u));  // 20AE20 vldr s10, [r0, #420]
    s[2] = Float(c.Load32(r[0] + 0x168u));  // 20AE24 vldr s2, [r0, #360]
    s[8] = Float(c.Load32(r[0] + 0x1A8u));  // 20AE28 vldr s8, [r0, #424]
    s[5] = Mul(s[4], s[10]);  // 20AE2C vmul.f32 s5, s4, s10
    s[0] = Float(c.Load32(r[0] + 0x16Cu));  // 20AE30 vldr s0, [r0, #364]
    s[1] = Mul(s[2], s[8]);  // 20AE34 vmul.f32 s1, s2, s8
    s[6] = Float(c.Load32(r[0] + 0x1ACu));  // 20AE38 vldr s6, [r0, #428]
    s[12] = Float(c.Load32(r[0] + 0x160u));  // 20AE3C vldr s12, [r0, #352]
    s[3] = Mul(s[0], s[6]);  // 20AE40 vmul.f32 s3, s0, s6
    s[14] = Float(c.Load32(r[0] + 0x1A0u));  // 20AE44 vldr s14, [r0, #416]
    s[1] = Add(s[5], s[1]);  // 20AE48 vadd.f32 s1, s5, s1
    s[5] = Mul(s[12], s[14]);  // 20AE4C vmul.f32 s5, s12, s14
    s[1] = Add(s[1], s[3]);  // 20AE50 vadd.f32 s1, s1, s3
    s[3] = Float(c.Load32(r[13] + 0x4u));  // 20AE54 vldr s3, [sp, #4]
    r[0] = c.base + 0x20AE60u + 0x68u;  // 20AE58 add r0, pc, #104
    s[5] = Add(s[1], s[5]);  // 20AE5C vadd.f32 s5, s1, s5
    s[1] = Float(0x3F800000u);  // 20AE60 vmov.f32 s1, #1.000000e+00
    c.nzcv = Compare(s[5], Float(0x0u));  // 20AE64 vcmpe.f32 s5, #0
    flags = c.nzcv;  // 20AE68 vmrs APSR_nzcv, fpscr
    s[1] = Sub(s[1], s[3]);  // 20AE6C vsub.f32 s1, s1, s3
    s[12] = Mul(s[12], s[1]);  // 20AE70 vmul.f32 s12, s12, s1
    s[4] = Mul(s[4], s[1]);  // 20AE74 vmul.f32 s4, s4, s1
    s[2] = Mul(s[2], s[1]);  // 20AE78 vmul.f32 s2, s2, s1
    s[0] = Mul(s[0], s[1]);  // 20AE7C vmul.f32 s0, s0, s1
    if (Pred(flags, 4u)) r[0] = r[0] + 0x4u;  // 20AE80 addmi r0, r0, #4
    s[5] = Float(c.Load32(r[0]));  // 20AE84 vldr s5, [r0]
    s[3] = Mul(s[3], s[5]);  // 20AE88 vmul.f32 s3, s3, s5
    s[14] = Mul(s[14], s[3]);  // 20AE8C vmul.f32 s14, s14, s3
    s[10] = Mul(s[10], s[3]);  // 20AE90 vmul.f32 s10, s10, s3
    s[8] = Mul(s[8], s[3]);  // 20AE94 vmul.f32 s8, s8, s3
    s[6] = Mul(s[6], s[3]);  // 20AE98 vmul.f32 s6, s6, s3
    s[12] = Add(s[12], s[14]);  // 20AE9C vadd.f32 s12, s12, s14
    s[4] = Add(s[4], s[10]);  // 20AEA0 vadd.f32 s4, s4, s10
    s[2] = Add(s[2], s[8]);  // 20AEA4 vadd.f32 s2, s2, s8
    s[0] = Add(s[0], s[6]);  // 20AEA8 vadd.f32 s0, s0, s6
    c.Store32(r[11], Bits(s[12]));  // 20AEAC vstr s12, [r11]
    c.Store32(r[11] + 0x4u, Bits(s[4]));  // 20AEB0 vstr s4, [r11, #4]
    c.Store32(r[11] + 0x8u, Bits(s[2]));  // 20AEB4 vstr s2, [r11, #8]
    c.Store32(r[11] + 0xCu, Bits(s[0]));  // 20AEB8 vstr s0, [r11, #12]
    r[13] = r[13] + 0xCu;  // 20AEBC add sp, sp, #12
    { r[4] = c.Load32(r[13] + 0u); r[5] = c.Load32(r[13] + 4u); r[6] = c.Load32(r[13] + 8u); r[7] = c.Load32(r[13] + 12u); r[8] = c.Load32(r[13] + 16u); r[9] = c.Load32(r[13] + 20u); r[10] = c.Load32(r[13] + 24u); r[11] = c.Load32(r[13] + 28u); r[15] = c.Load32(r[13] + 32u); r[13] += 36u; Publish<0xFFFFu, 0x0000000F0000557Full>(c, r, s); return; }  // 20AEC0 pop {r4, r5, r6, r7, r8, r9, r10, r11, pc}
}

inline void KeyQuat(Context& c) {
    auto r = c.r;
    auto s = c.s;
    auto& flags = c.flags;
    { r[13] -= 24u; c.Store32(r[13] + 0u, r[4]); c.Store32(r[13] + 4u, r[5]); c.Store32(r[13] + 8u, r[6]); c.Store32(r[13] + 12u, r[7]); c.Store32(r[13] + 16u, r[11]); c.Store32(r[13] + 20u, r[14]); }  // 20AAD0 push {r4, r5, r6, r7, r11, lr}
    r[12] = c.Load32(r[0] + 0x4u);  // 20AAD4 ldr r12, [r0, #0x4]
    r[4] = c.Load32(r[0] + 0x8u);  // 20AAD8 ldr r4, [r0, #0x8]
    s[2] = Float(c.Load32(r[3]));  // 20AADC vldr s2, [r3]
    s[0] = Float(c.Load32(r[12]));  // 20AAE0 vldr s0, [r12]
    r[14] = r[4] - 0x1u;  // 20AAE4 sub lr, r4, #1
    s[0] = SignedFloat(Bits(s[0]));  // 20AAE8 vcvt.f32.s32 s0, s0
    c.nzcv = Compare(s[2], s[0]);  // 20AAEC vcmpe.f32 s2, s0
    flags = c.nzcv;  // 20AAF0 vmrs APSR_nzcv, fpscr
    if (Pred(flags, 4u)) goto L20AB14;  // 20AAF4 bmi 0x20ab14 <_ZNK6rglIpoI13rglQuaternionE16get_interval_keyERiRfRKf+0x44>
    r[3] = r[12] + Lsl(r[14], 0x2u);  // 20AAF8 add r3, r12, lr, lsl #2
    s[0] = Float(c.Load32(r[3]));  // 20AAFC vldr s0, [r3]
    s[0] = SignedFloat(Bits(s[0]));  // 20AB00 vcvt.f32.s32 s0, s0
    c.nzcv = Compare(s[2], s[0]);  // 20AB04 vcmpe.f32 s2, s0
    flags = c.nzcv;  // 20AB08 vmrs APSR_nzcv, fpscr
    if (Pred(flags, 10u)) s[2] = s[0];  // 20AB0C vmovge.f32 s2, s0
    s[0] = s[2];  // 20AB10 vmov.f32 s0, s2
L20AB14:
    r[5] = c.Load32(r[1]);  // 20AB14 ldr r5, [r1]
    // Integer CPSR flags in this helper are dead at its callers: Evaluate next
    // sets them at20AD40. Each paired predicate is an exact signed comparison;
    // retain all FP compares and FPSCR.NZCV/sticky effects unchanged.
    // 20AB18 cmp r5, #0
    if (static_cast<int32_t>(r[5]) < 0) goto L20ABAC;  // 20AB1C blt 0x20abac <_ZNK6rglIpoI13rglQuaternionE16get_interval_keyERiRfRKf+0xdc>
    // 20AB20 cmp r5, lr
    if (static_cast<int32_t>(r[5]) >= static_cast<int32_t>(r[14])) goto L20ABAC;  // 20AB24 bge 0x20abac <_ZNK6rglIpoI13rglQuaternionE16get_interval_keyERiRfRKf+0xdc>
    r[6] = r[12] + Lsl(r[5], 0x2u);  // 20AB28 add r6, r12, r5, lsl #2
    s[2] = Float(c.Load32(r[6]));  // 20AB2C vldr s2, [r6]
    s[2] = SignedFloat(Bits(s[2]));  // 20AB30 vcvt.f32.s32 s2, s2
    c.nzcv = Compare(s[2], s[0]);  // 20AB34 vcmpe.f32 s2, s0
    flags = c.nzcv;  // 20AB38 vmrs APSR_nzcv, fpscr
    if (Pred(flags, 9u)) goto L20AB68;  // 20AB3C bls 0x20ab68 <_ZNK6rglIpoI13rglQuaternionE16get_interval_keyERiRfRKf+0x98>
    // 20AB40 cmp r5, #1
    if (static_cast<int32_t>(r[5]) < 1) goto L20ABAC;  // 20AB44 blt 0x20abac <_ZNK6rglIpoI13rglQuaternionE16get_interval_keyERiRfRKf+0xdc>
    r[3] = r[5] - 0x1u;  // 20AB48 sub r3, r5, #1
    r[5] = r[12] + Lsl(r[3], 0x2u);  // 20AB4C add r5, r12, r3, lsl #2
    s[2] = Float(c.Load32(r[5]));  // 20AB50 vldr s2, [r5]
    s[2] = SignedFloat(Bits(s[2]));  // 20AB54 vcvt.f32.s32 s2, s2
    c.nzcv = Compare(s[2], s[0]);  // 20AB58 vcmpe.f32 s2, s0
    flags = c.nzcv;  // 20AB5C vmrs APSR_nzcv, fpscr
    if (Pred(flags, 9u)) goto L20ABEC;  // 20AB60 bls 0x20abec <_ZNK6rglIpoI13rglQuaternionE16get_interval_keyERiRfRKf+0x11c>
    goto L20ABAC;  // 20AB64 b 0x20abac <_ZNK6rglIpoI13rglQuaternionE16get_interval_keyERiRfRKf+0xdc>
L20AB68:
    r[3] = r[5] + 0x1u;  // 20AB68 add r3, r5, #1
    r[7] = r[12] + Lsl(r[3], 0x2u);  // 20AB6C add r7, r12, r3, lsl #2
    s[2] = Float(c.Load32(r[7]));  // 20AB70 vldr s2, [r7]
    s[2] = SignedFloat(Bits(s[2]));  // 20AB74 vcvt.f32.s32 s2, s2
    c.nzcv = Compare(s[0], s[2]);  // 20AB78 vcmpe.f32 s0, s2
    flags = c.nzcv;  // 20AB7C vmrs APSR_nzcv, fpscr
    if (Pred(flags, 5u)) goto L20AB8C;  // 20AB80 bpl 0x20ab8c <_ZNK6rglIpoI13rglQuaternionE16get_interval_keyERiRfRKf+0xbc>
    r[3] = r[5];  // 20AB84 mov r3, r5
    goto L20ABF0;  // 20AB88 b 0x20abf0 <_ZNK6rglIpoI13rglQuaternionE16get_interval_keyERiRfRKf+0x120>
L20AB8C:
    r[7] = r[4] - 0x2u;  // 20AB8C sub r7, r4, #2
    // 20AB90 cmp r5, r7
    if (static_cast<int32_t>(r[5]) >= static_cast<int32_t>(r[7])) goto L20ABAC;  // 20AB94 bge 0x20abac <_ZNK6rglIpoI13rglQuaternionE16get_interval_keyERiRfRKf+0xdc>
    s[2] = Float(c.Load32(r[6] + 0x8u));  // 20AB98 vldr s2, [r6, #8]
    s[2] = SignedFloat(Bits(s[2]));  // 20AB9C vcvt.f32.s32 s2, s2
    c.nzcv = Compare(s[0], s[2]);  // 20ABA0 vcmpe.f32 s0, s2
    flags = c.nzcv;  // 20ABA4 vmrs APSR_nzcv, fpscr
    if (Pred(flags, 4u)) goto L20ABEC;  // 20ABA8 bmi 0x20abec <_ZNK6rglIpoI13rglQuaternionE16get_interval_keyERiRfRKf+0x11c>
L20ABAC:
    r[4] = r[4] - 0x2u;  // 20ABAC sub r4, r4, #2
    r[3] = 0x0u;  // 20ABB0 mov r3, #0
    // 20ABB4 cmp r4, #1
    if (static_cast<int32_t>(r[4]) < 1) goto L20ABEC;  // 20ABB8 blt 0x20abec <_ZNK6rglIpoI13rglQuaternionE16get_interval_keyERiRfRKf+0x11c>
L20ABBC:
    r[4] = r[14] + r[3];  // 20ABBC add r4, lr, r3
    r[5] = Asr(r[4], 0x1u);  // 20ABC0 asr r5, r4, #1
    r[5] = r[12] + Lsl(r[5], 0x2u);  // 20ABC4 add r5, r12, r5, lsl #2
    s[2] = Float(c.Load32(r[5]));  // 20ABC8 vldr s2, [r5]
    s[2] = SignedFloat(Bits(s[2]));  // 20ABCC vcvt.f32.s32 s2, s2
    c.nzcv = Compare(s[0], s[2]);  // 20ABD0 vcmpe.f32 s0, s2
    flags = c.nzcv;  // 20ABD4 vmrs APSR_nzcv, fpscr
    if (Pred(flags, 4u)) r[14] = Asr(r[4], 0x1u);  // 20ABD8 asrmi lr, r4, #1
    if (Pred(flags, 5u)) r[3] = Asr(r[4], 0x1u);  // 20ABDC asrpl r3, r4, #1
    r[5] = r[14] - 0x1u;  // 20ABE0 sub r5, lr, #1
    // 20ABE4 cmp r3, r5
    if (static_cast<int32_t>(r[3]) < static_cast<int32_t>(r[5])) goto L20ABBC;  // 20ABE8 blt 0x20abbc <_ZNK6rglIpoI13rglQuaternionE16get_interval_keyERiRfRKf+0xec>
L20ABEC:
    c.Store32(r[1], r[3]);  // 20ABEC str r3, [r1]
L20ABF0:
    r[1] = c.Load32(r[0] + 0x4u);  // 20ABF0 ldr r1, [r0, #0x4]
    r[0] = c.Load32((r[1] = r[1] + Lsl(r[3], 0x2u)));  // 20ABF4 ldr r0, [r1, r3, lsl #2]!
    r[1] = c.Load32(r[1] + 0x4u);  // 20ABF8 ldr r1, [r1, #0x4]
    s[2] = Float(r[0]);  // 20ABFC vmov s2, r0
    r[0] = r[1] - r[0];  // 20AC00 sub r0, r1, r0
    s[2] = SignedFloat(Bits(s[2]));  // 20AC04 vcvt.f32.s32 s2, s2
    s[4] = Float(r[0]);  // 20AC08 vmov s4, r0
    s[4] = SignedFloat(Bits(s[4]));  // 20AC0C vcvt.f32.s32 s4, s4
    s[0] = Sub(s[0], s[2]);  // 20AC10 vsub.f32 s0, s0, s2
    s[0] = Div(s[0], s[4]);  // 20AC14 vdiv.f32 s0, s0, s4
    c.Store32(r[2], Bits(s[0]));  // 20AC18 vstr s0, [r2]
    { r[4] = c.Load32(r[13] + 0u); r[5] = c.Load32(r[13] + 4u); r[6] = c.Load32(r[13] + 8u); r[7] = c.Load32(r[13] + 12u); r[11] = c.Load32(r[13] + 16u); r[15] = c.Load32(r[13] + 20u); r[13] += 24u; Publish<0xF8FBu, 0x0000000000000015ull>(c, r, s); return; }  // 20AC1C pop {r4, r5, r6, r7, r11, pc}
}

inline void Root(Context& c) {
    auto r = c.r;
    auto s = c.s;
    auto& flags = c.flags;
    { r[13] -= 16u; c.Store32(r[13] + 0u, r[4]); c.Store32(r[13] + 4u, r[5]); c.Store32(r[13] + 8u, r[6]); c.Store32(r[13] + 12u, r[14]); }  // 20AED0 push {r4, r5, r6, lr}
    r[13] = r[13] - 0x8u;  // 20AED4 sub sp, sp, #8
    r[4] = r[0];  // 20AED8 mov r4, r0
    r[0] = c.Load32(r[2]);  // 20AEDC ldr r0, [r2]
    r[5] = r[1];  // 20AEE0 mov r5, r1
    r[1] = 0xC8u;  // 20AEE4 mov r1, #200
    r[2] = c.Load32(r[5] + 0x48u);  // 20AEE8 ldr r2, [r5, #0x48]
    r[1] = r[0] * r[1] + r[2];  // 20AEEC mla r1, r0, r1, r2
    r[6] = r[1];  // 20AEF0 mov r6, r1
    r[0] = c.Load32((r[6] = r[6] + 0x4Cu));  // 20AEF4 ldr r0, [r6, #0x4c]!
    flags = Cmp(r[0], 0x0u);  // 20AEF8 cmp r0, #0
    if (Pred(flags, 0u)) goto L20AFB0;  // 20AEFC beq 0x20afb0 <_ZN11rglSkeleton26evaluate_root_pos_intervalERKi+0xe0>
    r[3] = c.Load32(r[1] + 0x8Cu);  // 20AF00 ldr r3, [r1, #0x8c]
    r[0] = r[0] + 0xB0u;  // 20AF04 add r0, r0, #176
    s[4] = Float(c.Load32(r[1] + 0x18u));  // 20AF08 vldr s4, [r1, #24]
    r[1] = r[5] + 0x2Cu;  // 20AF0C add r1, r5, #44
    r[2] = c.Load32(r[3] + 0x8u);  // 20AF10 ldr r2, [r3, #0x8]
    r[3] = c.Load32(r[3] + 0xCu);  // 20AF14 ldr r3, [r3, #0xc]
    r[3] = r[3] - r[2];  // 20AF18 sub r3, r3, r2
    s[2] = Float(r[2]);  // 20AF1C vmov s2, r2
    r[2] = r[13];  // 20AF20 mov r2, sp
    s[0] = Float(r[3]);  // 20AF24 vmov s0, r3
    r[3] = r[13] + 0x4u;  // 20AF28 add r3, sp, #4
    s[2] = SignedFloat(Bits(s[2]));  // 20AF2C vcvt.f32.s32 s2, s2
    s[0] = SignedFloat(Bits(s[0]));  // 20AF30 vcvt.f32.s32 s0, s0
    s[0] = Mul(s[4], s[0]);  // 20AF34 vmul.f32 s0, s4, s0
    s[0] = Add(s[2], s[0]);  // 20AF38 vadd.f32 s0, s2, s0
    c.Store32(r[13] + 0x4u, Bits(s[0]));  // 20AF3C vstr s0, [sp, #4]
    { r[14] = c.base + 0x20AF44u; Publish<0xF8FFu, 0x0000000000000015ull>(c, r, s); KeyVec(c); Reload<0xF8FBu, 0x0000000000000015ull>(c, r, s); }  // 20AF40 bl 0x20afc8 <_ZNK6rglIpoI7rglVec3E16get_interval_keyERiRfRKf>
    s[2] = Float(0x3F800000u);  // 20AF44 vmov.f32 s2, #1.000000e+00
    r[0] = c.Load32(r[6]);  // 20AF48 ldr r0, [r6]
    s[0] = Float(c.Load32(r[13]));  // 20AF4C vldr s0, [sp]
    r[1] = c.Load32(r[5] + 0x2Cu);  // 20AF50 ldr r1, [r5, #0x2c]
    r[0] = c.Load32(r[0] + 0xB0u);  // 20AF54 ldr r0, [r0, #0xb0]
    r[0] = r[0] + Lsl(r[1], 0x4u);  // 20AF58 add r0, r0, r1, lsl #4
    s[2] = Sub(s[2], s[0]);  // 20AF5C vsub.f32 s2, s2, s0
    s[6] = Float(c.Load32(r[0] + 0x4u));  // 20AF60 vldr s6, [r0, #4]
    s[8] = Float(c.Load32(r[0]));  // 20AF64 vldr s8, [r0]
    s[12] = Float(c.Load32(r[0] + 0x10u));  // 20AF68 vldr s12, [r0, #16]
    s[14] = Float(c.Load32(r[0] + 0x14u));  // 20AF6C vldr s14, [r0, #20]
    s[4] = Float(c.Load32(r[0] + 0x8u));  // 20AF70 vldr s4, [r0, #8]
    s[12] = Mul(s[12], s[0]);  // 20AF74 vmul.f32 s12, s12, s0
    s[10] = Float(c.Load32(r[0] + 0x18u));  // 20AF78 vldr s10, [r0, #24]
    s[14] = Mul(s[0], s[14]);  // 20AF7C vmul.f32 s14, s0, s14
    s[6] = Mul(s[2], s[6]);  // 20AF80 vmul.f32 s6, s2, s6
    s[8] = Mul(s[2], s[8]);  // 20AF84 vmul.f32 s8, s2, s8
    s[2] = Mul(s[2], s[4]);  // 20AF88 vmul.f32 s2, s2, s4
    s[0] = Mul(s[0], s[10]);  // 20AF8C vmul.f32 s0, s0, s10
    s[6] = Add(s[14], s[6]);  // 20AF90 vadd.f32 s6, s14, s6
    s[4] = Add(s[12], s[8]);  // 20AF94 vadd.f32 s4, s12, s8
    s[0] = Add(s[0], s[2]);  // 20AF98 vadd.f32 s0, s0, s2
    c.Store32(r[4], Bits(s[4]));  // 20AF9C vstr s4, [r4]
    c.Store32(r[4] + 0x4u, Bits(s[6]));  // 20AFA0 vstr s6, [r4, #4]
    c.Store32(r[4] + 0x8u, Bits(s[0]));  // 20AFA4 vstr s0, [r4, #8]
    r[13] = r[13] + 0x8u;  // 20AFA8 add sp, sp, #8
    { r[4] = c.Load32(r[13] + 0u); r[5] = c.Load32(r[13] + 4u); r[6] = c.Load32(r[13] + 8u); r[15] = c.Load32(r[13] + 12u); r[13] += 16u; Publish<0xF8FFu, 0x0000000300005555ull>(c, r, s); return; }  // 20AFAC pop {r4, r5, r6, pc}
L20AFB0:
    s[32] = s[33] = Float(0x0u);  // 20AFB0 vmov.i32 d16, #0x0
    r[0] = 0x0u;  // 20AFB4 mov r0, #0
    std::memcpy(c.guest + r[4], s.data() + 32, 8);  // 20AFB8 vstr d16, [r4]
    c.Store32(r[4] + 0x8u, r[0]);  // 20AFBC str r0, [r4, #0x8]
    r[13] = r[13] + 0x8u;  // 20AFC0 add sp, sp, #8
    { r[4] = c.Load32(r[13] + 0u); r[5] = c.Load32(r[13] + 4u); r[6] = c.Load32(r[13] + 8u); r[15] = c.Load32(r[13] + 12u); r[13] += 16u; Publish<0xF8FFu, 0x0000000300005555ull>(c, r, s); return; }  // 20AFC4 pop {r4, r5, r6, pc}
}

inline void KeyVec(Context& c) {
    auto r = c.r;
    auto s = c.s;
    auto& flags = c.flags;
    { r[13] -= 24u; c.Store32(r[13] + 0u, r[4]); c.Store32(r[13] + 4u, r[5]); c.Store32(r[13] + 8u, r[6]); c.Store32(r[13] + 12u, r[7]); c.Store32(r[13] + 16u, r[11]); c.Store32(r[13] + 20u, r[14]); }  // 20AFC8 push {r4, r5, r6, r7, r11, lr}
    r[12] = c.Load32(r[0] + 0x4u);  // 20AFCC ldr r12, [r0, #0x4]
    r[4] = c.Load32(r[0] + 0x8u);  // 20AFD0 ldr r4, [r0, #0x8]
    s[2] = Float(c.Load32(r[3]));  // 20AFD4 vldr s2, [r3]
    s[0] = Float(c.Load32(r[12]));  // 20AFD8 vldr s0, [r12]
    r[14] = r[4] - 0x1u;  // 20AFDC sub lr, r4, #1
    s[0] = SignedFloat(Bits(s[0]));  // 20AFE0 vcvt.f32.s32 s0, s0
    c.nzcv = Compare(s[2], s[0]);  // 20AFE4 vcmpe.f32 s2, s0
    flags = c.nzcv;  // 20AFE8 vmrs APSR_nzcv, fpscr
    if (Pred(flags, 4u)) goto L20B00C;  // 20AFEC bmi 0x20b00c <_ZNK6rglIpoI7rglVec3E16get_interval_keyERiRfRKf+0x44>
    r[3] = r[12] + Lsl(r[14], 0x2u);  // 20AFF0 add r3, r12, lr, lsl #2
    s[0] = Float(c.Load32(r[3]));  // 20AFF4 vldr s0, [r3]
    s[0] = SignedFloat(Bits(s[0]));  // 20AFF8 vcvt.f32.s32 s0, s0
    c.nzcv = Compare(s[2], s[0]);  // 20AFFC vcmpe.f32 s2, s0
    flags = c.nzcv;  // 20B000 vmrs APSR_nzcv, fpscr
    if (Pred(flags, 10u)) s[2] = s[0];  // 20B004 vmovge.f32 s2, s0
    s[0] = s[2];  // 20B008 vmov.f32 s0, s2
L20B00C:
    r[5] = c.Load32(r[1]);  // 20B00C ldr r5, [r1]
    // Root never consumes this helper's integer flags; Loop overwrites them
    // at2088EC before its next predicate. FP flags remain fully observable.
    // 20B010 cmp r5, #0
    if (static_cast<int32_t>(r[5]) < 0) goto L20B0A4;  // 20B014 blt 0x20b0a4 <_ZNK6rglIpoI7rglVec3E16get_interval_keyERiRfRKf+0xdc>
    // 20B018 cmp r5, lr
    if (static_cast<int32_t>(r[5]) >= static_cast<int32_t>(r[14])) goto L20B0A4;  // 20B01C bge 0x20b0a4 <_ZNK6rglIpoI7rglVec3E16get_interval_keyERiRfRKf+0xdc>
    r[6] = r[12] + Lsl(r[5], 0x2u);  // 20B020 add r6, r12, r5, lsl #2
    s[2] = Float(c.Load32(r[6]));  // 20B024 vldr s2, [r6]
    s[2] = SignedFloat(Bits(s[2]));  // 20B028 vcvt.f32.s32 s2, s2
    c.nzcv = Compare(s[2], s[0]);  // 20B02C vcmpe.f32 s2, s0
    flags = c.nzcv;  // 20B030 vmrs APSR_nzcv, fpscr
    if (Pred(flags, 9u)) goto L20B060;  // 20B034 bls 0x20b060 <_ZNK6rglIpoI7rglVec3E16get_interval_keyERiRfRKf+0x98>
    // 20B038 cmp r5, #1
    if (static_cast<int32_t>(r[5]) < 1) goto L20B0A4;  // 20B03C blt 0x20b0a4 <_ZNK6rglIpoI7rglVec3E16get_interval_keyERiRfRKf+0xdc>
    r[3] = r[5] - 0x1u;  // 20B040 sub r3, r5, #1
    r[5] = r[12] + Lsl(r[3], 0x2u);  // 20B044 add r5, r12, r3, lsl #2
    s[2] = Float(c.Load32(r[5]));  // 20B048 vldr s2, [r5]
    s[2] = SignedFloat(Bits(s[2]));  // 20B04C vcvt.f32.s32 s2, s2
    c.nzcv = Compare(s[2], s[0]);  // 20B050 vcmpe.f32 s2, s0
    flags = c.nzcv;  // 20B054 vmrs APSR_nzcv, fpscr
    if (Pred(flags, 9u)) goto L20B0E4;  // 20B058 bls 0x20b0e4 <_ZNK6rglIpoI7rglVec3E16get_interval_keyERiRfRKf+0x11c>
    goto L20B0A4;  // 20B05C b 0x20b0a4 <_ZNK6rglIpoI7rglVec3E16get_interval_keyERiRfRKf+0xdc>
L20B060:
    r[3] = r[5] + 0x1u;  // 20B060 add r3, r5, #1
    r[7] = r[12] + Lsl(r[3], 0x2u);  // 20B064 add r7, r12, r3, lsl #2
    s[2] = Float(c.Load32(r[7]));  // 20B068 vldr s2, [r7]
    s[2] = SignedFloat(Bits(s[2]));  // 20B06C vcvt.f32.s32 s2, s2
    c.nzcv = Compare(s[0], s[2]);  // 20B070 vcmpe.f32 s0, s2
    flags = c.nzcv;  // 20B074 vmrs APSR_nzcv, fpscr
    if (Pred(flags, 5u)) goto L20B084;  // 20B078 bpl 0x20b084 <_ZNK6rglIpoI7rglVec3E16get_interval_keyERiRfRKf+0xbc>
    r[3] = r[5];  // 20B07C mov r3, r5
    goto L20B0E8;  // 20B080 b 0x20b0e8 <_ZNK6rglIpoI7rglVec3E16get_interval_keyERiRfRKf+0x120>
L20B084:
    r[7] = r[4] - 0x2u;  // 20B084 sub r7, r4, #2
    // 20B088 cmp r5, r7
    if (static_cast<int32_t>(r[5]) >= static_cast<int32_t>(r[7])) goto L20B0A4;  // 20B08C bge 0x20b0a4 <_ZNK6rglIpoI7rglVec3E16get_interval_keyERiRfRKf+0xdc>
    s[2] = Float(c.Load32(r[6] + 0x8u));  // 20B090 vldr s2, [r6, #8]
    s[2] = SignedFloat(Bits(s[2]));  // 20B094 vcvt.f32.s32 s2, s2
    c.nzcv = Compare(s[0], s[2]);  // 20B098 vcmpe.f32 s0, s2
    flags = c.nzcv;  // 20B09C vmrs APSR_nzcv, fpscr
    if (Pred(flags, 4u)) goto L20B0E4;  // 20B0A0 bmi 0x20b0e4 <_ZNK6rglIpoI7rglVec3E16get_interval_keyERiRfRKf+0x11c>
L20B0A4:
    r[4] = r[4] - 0x2u;  // 20B0A4 sub r4, r4, #2
    r[3] = 0x0u;  // 20B0A8 mov r3, #0
    // 20B0AC cmp r4, #1
    if (static_cast<int32_t>(r[4]) < 1) goto L20B0E4;  // 20B0B0 blt 0x20b0e4 <_ZNK6rglIpoI7rglVec3E16get_interval_keyERiRfRKf+0x11c>
L20B0B4:
    r[4] = r[14] + r[3];  // 20B0B4 add r4, lr, r3
    r[5] = Asr(r[4], 0x1u);  // 20B0B8 asr r5, r4, #1
    r[5] = r[12] + Lsl(r[5], 0x2u);  // 20B0BC add r5, r12, r5, lsl #2
    s[2] = Float(c.Load32(r[5]));  // 20B0C0 vldr s2, [r5]
    s[2] = SignedFloat(Bits(s[2]));  // 20B0C4 vcvt.f32.s32 s2, s2
    c.nzcv = Compare(s[0], s[2]);  // 20B0C8 vcmpe.f32 s0, s2
    flags = c.nzcv;  // 20B0CC vmrs APSR_nzcv, fpscr
    if (Pred(flags, 4u)) r[14] = Asr(r[4], 0x1u);  // 20B0D0 asrmi lr, r4, #1
    if (Pred(flags, 5u)) r[3] = Asr(r[4], 0x1u);  // 20B0D4 asrpl r3, r4, #1
    r[5] = r[14] - 0x1u;  // 20B0D8 sub r5, lr, #1
    // 20B0DC cmp r3, r5
    if (static_cast<int32_t>(r[3]) < static_cast<int32_t>(r[5])) goto L20B0B4;  // 20B0E0 blt 0x20b0b4 <_ZNK6rglIpoI7rglVec3E16get_interval_keyERiRfRKf+0xec>
L20B0E4:
    c.Store32(r[1], r[3]);  // 20B0E4 str r3, [r1]
L20B0E8:
    r[1] = c.Load32(r[0] + 0x4u);  // 20B0E8 ldr r1, [r0, #0x4]
    r[0] = c.Load32((r[1] = r[1] + Lsl(r[3], 0x2u)));  // 20B0EC ldr r0, [r1, r3, lsl #2]!
    r[1] = c.Load32(r[1] + 0x4u);  // 20B0F0 ldr r1, [r1, #0x4]
    s[2] = Float(r[0]);  // 20B0F4 vmov s2, r0
    r[0] = r[1] - r[0];  // 20B0F8 sub r0, r1, r0
    s[2] = SignedFloat(Bits(s[2]));  // 20B0FC vcvt.f32.s32 s2, s2
    s[4] = Float(r[0]);  // 20B100 vmov s4, r0
    s[4] = SignedFloat(Bits(s[4]));  // 20B104 vcvt.f32.s32 s4, s4
    s[0] = Sub(s[0], s[2]);  // 20B108 vsub.f32 s0, s0, s2
    s[0] = Div(s[0], s[4]);  // 20B10C vdiv.f32 s0, s0, s4
    c.Store32(r[2], Bits(s[0]));  // 20B110 vstr s0, [r2]
    { r[4] = c.Load32(r[13] + 0u); r[5] = c.Load32(r[13] + 4u); r[6] = c.Load32(r[13] + 8u); r[7] = c.Load32(r[13] + 12u); r[11] = c.Load32(r[13] + 16u); r[15] = c.Load32(r[13] + 20u); r[13] += 24u; Publish<0xF8FBu, 0x0000000000000015ull>(c, r, s); return; }  // 20B114 pop {r4, r5, r6, r7, r11, pc}
}

inline void Global(Context& c) {
    auto r = c.r;
    auto s = c.s;
    auto& flags = c.flags;
    { r[13] -= 24u; c.Store32(r[13] + 0u, r[4]); c.Store32(r[13] + 4u, r[5]); c.Store32(r[13] + 8u, r[6]); c.Store32(r[13] + 12u, r[7]); c.Store32(r[13] + 16u, r[8]); c.Store32(r[13] + 20u, r[14]); }  // 2095B0 push {r4, r5, r6, r7, r8, lr}
    { r[13] -= 8u; std::memcpy(c.guest + r[13], s.data() + 16, 8); }  // 2095B4 vpush {d8}
    r[13] = r[13] - 0x30u;  // 2095B8 sub sp, sp, #48
    r[8] = r[13] + 0x20u;  // 2095BC add r8, sp, #32
    r[7] = r[3];  // 2095C0 mov r7, r3
    r[4] = r[0];  // 2095C4 mov r4, r0
    r[0] = 0x0u;  // 2095C8 mov r0, #0
    r[3] = r[13] + 0x1Cu;  // 2095CC add r3, sp, #28
    c.Store32(r[13] + 0x1Cu, r[0]);  // 2095D0 str r0, [sp, #0x1c]
    r[0] = r[8];  // 2095D4 mov r0, r8
    c.Store32(r[13], r[7]);  // 2095D8 str r7, [sp]
    r[5] = r[2];  // 2095DC mov r5, r2
    r[6] = r[1];  // 2095E0 mov r6, r1
    { r[14] = c.base + 0x2095E8u; Publish<0xFFFFu, 0x0000000F0000557Full>(c, r, s); Evaluate(c); Reload<0xFFFFu, 0x0000000F0000557Full>(c, r, s); }  // 2095E4 bl 0x20ac20 <_ZN11rglSkeleton17evaluate_intervalERKiS1_S1_>
    r[0] = c.Load32(r[7]);  // 2095E8 ldr r0, [r7]
    r[1] = 0x214u;  // 2095EC mov r1, #532
    r[2] = c.Load32(r[6] + 0x4u);  // 2095F0 ldr r2, [r6, #0x4]
    r[3] = r[0] * r[1] + r[2];  // 2095F4 mla r3, r0, r1, r2
    r[0] = c.Load32((r[3] = r[3] + 0x1F8u));  // 2095F8 ldr r0, [r3, #0x1f8]!
    flags = Cmn(r[0], 0x1u);  // 2095FC cmn r0, #1
    if (Pred(flags, 1u)) goto L209610;  // 209600 bne 0x209610 <_ZN11rglSkeleton15get_global_quatERKiS1_+0x60>
    std::memcpy(s.data() + 32, c.guest + r[8], 16);  // 209604 vld1.64 {d16, d17}, [r8]
    std::memcpy(c.guest + r[4], s.data() + 32, 16);  // 209608 vst1.32 {d16, d17}, [r4]
    goto L2096C0;  // 20960C b 0x2096c0 <_ZN11rglSkeleton15get_global_quatERKiS1_+0x110>
L209610:
    r[0] = r[13] + 0x8u;  // 209610 add r0, sp, #8
    r[1] = r[6];  // 209614 mov r1, r6
    r[2] = r[5];  // 209618 mov r2, r5
    { r[14] = c.base + 0x209620u; Publish<0xFFFFu, 0x0000000F0003FFFFull>(c, r, s); Global(c); Reload<0xFFFFu, 0x0000000F0003FFFFull>(c, r, s); }  // 20961C bl 0x2095b0 <_ZN11rglSkeleton15get_global_quatERKiS1_>
    s[6] = Float(c.Load32(r[13] + 0xCu));  // 209620 vldr s6, [sp, #12]
    s[14] = Float(c.Load32(r[13] + 0x24u));  // 209624 vldr s14, [sp, #36]
    s[2] = Float(c.Load32(r[13] + 0x10u));  // 209628 vldr s2, [sp, #16]
    s[12] = Float(c.Load32(r[13] + 0x28u));  // 20962C vldr s12, [sp, #40]
    s[1] = Mul(s[14], s[6]);  // 209630 vmul.f32 s1, s14, s6
    s[0] = Float(c.Load32(r[13] + 0x14u));  // 209634 vldr s0, [sp, #20]
    s[7] = Mul(s[2], s[14]);  // 209638 vmul.f32 s7, s2, s14
    s[10] = Float(c.Load32(r[13] + 0x2Cu));  // 20963C vldr s10, [sp, #44]
    s[11] = Mul(s[2], s[12]);  // 209640 vmul.f32 s11, s2, s12
    s[16] = Mul(s[12], s[6]);  // 209644 vmul.f32 s16, s12, s6
    s[8] = Float(c.Load32(r[13] + 0x8u));  // 209648 vldr s8, [sp, #8]
    s[3] = Mul(s[0], s[12]);  // 20964C vmul.f32 s3, s0, s12
    s[4] = Float(c.Load32(r[13] + 0x20u));  // 209650 vldr s4, [sp, #32]
    s[13] = Mul(s[2], s[10]);  // 209654 vmul.f32 s13, s2, s10
    s[5] = Mul(s[10], s[6]);  // 209658 vmul.f32 s5, s10, s6
    s[15] = Mul(s[0], s[14]);  // 20965C vmul.f32 s15, s0, s14
    s[9] = Mul(s[10], s[0]);  // 209660 vmul.f32 s9, s10, s0
    s[1] = Add(s[11], s[1]);  // 209664 vadd.f32 s1, s11, s1
    s[14] = Mul(s[8], s[14]);  // 209668 vmul.f32 s14, s8, s14
    s[12] = Mul(s[12], s[8]);  // 20966C vmul.f32 s12, s12, s8
    s[3] = Sub(s[13], s[3]);  // 209670 vsub.f32 s3, s13, s3
    s[10] = Mul(s[10], s[8]);  // 209674 vmul.f32 s10, s10, s8
    s[5] = Sub(s[15], s[5]);  // 209678 vsub.f32 s5, s15, s5
    s[7] = Sub(s[16], s[7]);  // 20967C vsub.f32 s7, s16, s7
    s[8] = Mul(s[8], s[4]);  // 209680 vmul.f32 s8, s8, s4
    s[1] = Add(s[9], s[1]);  // 209684 vadd.f32 s1, s9, s1
    s[6] = Mul(s[6], s[4]);  // 209688 vmul.f32 s6, s6, s4
    s[14] = Add(s[3], s[14]);  // 20968C vadd.f32 s14, s3, s14
    s[2] = Mul(s[2], s[4]);  // 209690 vmul.f32 s2, s2, s4
    s[12] = Add(s[12], s[5]);  // 209694 vadd.f32 s12, s12, s5
    s[0] = Mul(s[0], s[4]);  // 209698 vmul.f32 s0, s0, s4
    s[4] = Add(s[10], s[7]);  // 20969C vadd.f32 s4, s10, s7
    s[8] = Sub(s[8], s[1]);  // 2096A0 vsub.f32 s8, s8, s1
    s[6] = Add(s[14], s[6]);  // 2096A4 vadd.f32 s6, s14, s6
    s[2] = Add(s[2], s[12]);  // 2096A8 vadd.f32 s2, s2, s12
    s[0] = Add(s[0], s[4]);  // 2096AC vadd.f32 s0, s0, s4
    c.Store32(r[4], Bits(s[8]));  // 2096B0 vstr s8, [r4]
    c.Store32(r[4] + 0x4u, Bits(s[6]));  // 2096B4 vstr s6, [r4, #4]
    c.Store32(r[4] + 0x8u, Bits(s[2]));  // 2096B8 vstr s2, [r4, #8]
    c.Store32(r[4] + 0xCu, Bits(s[0]));  // 2096BC vstr s0, [r4, #12]
L2096C0:
    r[13] = r[13] + 0x30u;  // 2096C0 add sp, sp, #48
    { std::memcpy(s.data() + 16, c.guest + r[13], 8); r[13] += 8u; }  // 2096C4 vpop {d8}
    { r[4] = c.Load32(r[13] + 0u); r[5] = c.Load32(r[13] + 4u); r[6] = c.Load32(r[13] + 8u); r[7] = c.Load32(r[13] + 12u); r[8] = c.Load32(r[13] + 16u); r[15] = c.Load32(r[13] + 20u); r[13] += 24u; Publish<0xFFFFu, 0x0000000F0003FFFFull>(c, r, s); return; }  // 2096C8 pop {r4, r5, r6, r7, r8, pc}
}

inline void Matrix(Context& c) {
    auto r = c.r;
    auto s = c.s;
    { r[13] -= 64u; std::memcpy(c.guest + r[13], s.data() + 16, 64); }  // 6E824 vpush {d8, d9, d10, d11, d12, d13, d14, d15}
    s[0] = Float(c.Load32(r[1]));  // 6E828 vldr s0, [r1]
    s[8] = Float(c.Load32(r[2] + 0x30u));  // 6E82C vldr s8, [r2, #48]
    s[4] = Float(c.Load32(r[1] + 0x10u));  // 6E830 vldr s4, [r1, #16]
    s[12] = Float(c.Load32(r[2] + 0x34u));  // 6E834 vldr s12, [r2, #52]
    s[11] = Mul(s[0], s[8]);  // 6E838 vmul.f32 s11, s0, s8
    s[2] = Float(c.Load32(r[1] + 0x4u));  // 6E83C vldr s2, [r1, #4]
    s[10] = Float(c.Load32(r[1] + 0x8u));  // 6E840 vldr s10, [r1, #8]
    s[14] = Mul(s[4], s[12]);  // 6E844 vmul.f32 s14, s4, s12
    s[6] = Float(c.Load32(r[1] + 0x14u));  // 6E848 vldr s6, [r1, #20]
    s[9] = Mul(s[8], s[2]);  // 6E84C vmul.f32 s9, s8, s2
    s[1] = Float(c.Load32(r[1] + 0x18u));  // 6E850 vldr s1, [r1, #24]
    s[13] = Mul(s[8], s[10]);  // 6E854 vmul.f32 s13, s8, s10
    s[5] = Mul(s[12], s[6]);  // 6E858 vmul.f32 s5, s12, s6
    s[3] = Float(c.Load32(r[2] + 0x38u));  // 6E85C vldr s3, [r2, #56]
    s[7] = Mul(s[12], s[1]);  // 6E860 vmul.f32 s7, s12, s1
    s[12] = Float(c.Load32(r[1] + 0x20u));  // 6E864 vldr s12, [r1, #32]
    s[8] = Float(c.Load32(r[1] + 0x24u));  // 6E868 vldr s8, [r1, #36]
    s[15] = Mul(s[12], s[3]);  // 6E86C vmul.f32 s15, s12, s3
    s[11] = Add(s[11], s[14]);  // 6E870 vadd.f32 s11, s11, s14
    s[14] = Float(c.Load32(r[1] + 0x28u));  // 6E874 vldr s14, [r1, #40]
    s[16] = Mul(s[3], s[8]);  // 6E878 vmul.f32 s16, s3, s8
    s[5] = Add(s[9], s[5]);  // 6E87C vadd.f32 s5, s9, s5
    s[9] = Mul(s[3], s[14]);  // 6E880 vmul.f32 s9, s3, s14
    s[7] = Add(s[13], s[7]);  // 6E884 vadd.f32 s7, s13, s7
    s[13] = Float(c.Load32(r[1] + 0x38u));  // 6E888 vldr s13, [r1, #56]
    s[3] = Add(s[11], s[15]);  // 6E88C vadd.f32 s3, s11, s15
    s[11] = Float(c.Load32(r[1] + 0x34u));  // 6E890 vldr s11, [r1, #52]
    s[5] = Add(s[5], s[16]);  // 6E894 vadd.f32 s5, s5, s16
    s[7] = Add(s[7], s[9]);  // 6E898 vadd.f32 s7, s7, s9
    s[9] = Float(c.Load32(r[1] + 0x30u));  // 6E89C vldr s9, [r1, #48]
    s[3] = Add(s[9], s[3]);  // 6E8A0 vadd.f32 s3, s9, s3
    s[5] = Add(s[11], s[5]);  // 6E8A4 vadd.f32 s5, s11, s5
    s[7] = Add(s[13], s[7]);  // 6E8A8 vadd.f32 s7, s13, s7
    c.Store32(r[0] + 0x30u, Bits(s[3]));  // 6E8AC vstr s3, [r0, #48]
    c.Store32(r[0] + 0x34u, Bits(s[5]));  // 6E8B0 vstr s5, [r0, #52]
    c.Store32(r[0] + 0x38u, Bits(s[7]));  // 6E8B4 vstr s7, [r0, #56]
    s[3] = Float(c.Load32(r[2] + 0x8u));  // 6E8B8 vldr s3, [r2, #8]
    std::memcpy(s.data() + 15, c.guest + r[2], 8);  // 6E8BC vldmia r2, {s15, s16}
    s[30] = Mul(s[3], s[8]);  // 6E8C0 vmul.f32 s30, s3, s8
    s[20] = Float(c.Load32(r[2] + 0x14u));  // 6E8C4 vldr s20, [r2, #20]
    s[9] = Mul(s[16], s[1]);  // 6E8C8 vmul.f32 s9, s16, s1
    s[11] = Float(c.Load32(r[2] + 0x10u));  // 6E8CC vldr s11, [r2, #16]
    s[24] = Mul(s[15], s[10]);  // 6E8D0 vmul.f32 s24, s15, s10
    s[18] = Mul(s[6], s[20]);  // 6E8D4 vmul.f32 s18, s6, s20
    s[7] = Float(c.Load32(r[2] + 0x24u));  // 6E8D8 vldr s7, [r2, #36]
    s[26] = Mul(s[2], s[11]);  // 6E8DC vmul.f32 s26, s2, s11
    s[28] = Float(c.Load32(r[2] + 0x28u));  // 6E8E0 vldr s28, [r2, #40]
    s[13] = Mul(s[0], s[11]);  // 6E8E4 vmul.f32 s13, s0, s11
    s[5] = Mul(s[4], s[20]);  // 6E8E8 vmul.f32 s5, s4, s20
    s[22] = Mul(s[1], s[20]);  // 6E8EC vmul.f32 s22, s1, s20
    s[11] = Mul(s[10], s[11]);  // 6E8F0 vmul.f32 s11, s10, s11
    s[20] = Mul(s[1], s[7]);  // 6E8F4 vmul.f32 s20, s1, s7
    s[1] = Add(s[24], s[9]);  // 6E8F8 vadd.f32 s1, s24, s9
    s[9] = Add(s[26], s[18]);  // 6E8FC vadd.f32 s9, s26, s18
    s[18] = Float(c.Load32(r[2] + 0x20u));  // 6E900 vldr s18, [r2, #32]
    s[24] = Mul(s[16], s[6]);  // 6E904 vmul.f32 s24, s16, s6
    s[10] = Mul(s[10], s[18]);  // 6E908 vmul.f32 s10, s10, s18
    s[5] = Add(s[13], s[5]);  // 6E90C vadd.f32 s5, s13, s5
    s[13] = Mul(s[15], s[2]);  // 6E910 vmul.f32 s13, s15, s2
    s[15] = Mul(s[0], s[15]);  // 6E914 vmul.f32 s15, s0, s15
    s[16] = Mul(s[4], s[16]);  // 6E918 vmul.f32 s16, s4, s16
    s[11] = Add(s[11], s[22]);  // 6E91C vadd.f32 s11, s11, s22
    s[22] = Float(c.Load32(r[2] + 0x18u));  // 6E920 vldr s22, [r2, #24]
    s[26] = Mul(s[3], s[14]);  // 6E924 vmul.f32 s26, s3, s14
    s[10] = Add(s[10], s[20]);  // 6E928 vadd.f32 s10, s10, s20
    s[20] = Mul(s[8], s[22]);  // 6E92C vmul.f32 s20, s8, s22
    s[13] = Add(s[13], s[24]);  // 6E930 vadd.f32 s13, s13, s24
    s[24] = Mul(s[12], s[22]);  // 6E934 vmul.f32 s24, s12, s22
    s[16] = Add(s[15], s[16]);  // 6E938 vadd.f32 s16, s15, s16
    s[15] = Mul(s[14], s[22]);  // 6E93C vmul.f32 s15, s14, s22
    s[22] = Mul(s[14], s[28]);  // 6E940 vmul.f32 s22, s14, s28
    s[14] = Mul(s[12], s[28]);  // 6E944 vmul.f32 s14, s12, s28
    s[12] = Mul(s[12], s[3]);  // 6E948 vmul.f32 s12, s12, s3
    s[3] = Mul(s[2], s[18]);  // 6E94C vmul.f32 s3, s2, s18
    s[6] = Mul(s[6], s[7]);  // 6E950 vmul.f32 s6, s6, s7
    s[2] = Mul(s[4], s[7]);  // 6E954 vmul.f32 s2, s4, s7
    s[0] = Mul(s[0], s[18]);  // 6E958 vmul.f32 s0, s0, s18
    s[4] = Mul(s[8], s[28]);  // 6E95C vmul.f32 s4, s8, s28
    s[1] = Add(s[1], s[26]);  // 6E960 vadd.f32 s1, s1, s26
    s[8] = Add(s[16], s[12]);  // 6E964 vadd.f32 s8, s16, s12
    s[12] = Add(s[13], s[30]);  // 6E968 vadd.f32 s12, s13, s30
    s[6] = Add(s[3], s[6]);  // 6E96C vadd.f32 s6, s3, s6
    s[3] = Add(s[5], s[24]);  // 6E970 vadd.f32 s3, s5, s24
    s[2] = Add(s[0], s[2]);  // 6E974 vadd.f32 s2, s0, s2
    s[5] = Add(s[9], s[20]);  // 6E978 vadd.f32 s5, s9, s20
    s[7] = Add(s[11], s[15]);  // 6E97C vadd.f32 s7, s11, s15
    s[0] = Add(s[10], s[22]);  // 6E980 vadd.f32 s0, s10, s22
    c.Store32(r[0], Bits(s[8]));  // 6E984 vstr s8, [r0]
    c.Store32(r[0] + 0x4u, Bits(s[12]));  // 6E988 vstr s12, [r0, #4]
    s[4] = Add(s[6], s[4]);  // 6E98C vadd.f32 s4, s6, s4
    c.Store32(r[0] + 0x8u, Bits(s[1]));  // 6E990 vstr s1, [r0, #8]
    c.Store32(r[0] + 0x10u, Bits(s[3]));  // 6E994 vstr s3, [r0, #16]
    s[2] = Add(s[2], s[14]);  // 6E998 vadd.f32 s2, s2, s14
    c.Store32(r[0] + 0x14u, Bits(s[5]));  // 6E99C vstr s5, [r0, #20]
    c.Store32(r[0] + 0x18u, Bits(s[7]));  // 6E9A0 vstr s7, [r0, #24]
    c.Store32(r[0] + 0x20u, Bits(s[2]));  // 6E9A4 vstr s2, [r0, #32]
    c.Store32(r[0] + 0x24u, Bits(s[4]));  // 6E9A8 vstr s4, [r0, #36]
    c.Store32(r[0] + 0x28u, Bits(s[0]));  // 6E9AC vstr s0, [r0, #40]
    { std::memcpy(s.data() + 16, c.guest + r[13], 64); r[13] += 64u; }  // 6E9B0 vpop {d8, d9, d10, d11, d12, d13, d14, d15}
    Publish<0x2000u, 0x00000000FFFFFFFFull>(c, r, s); return;  // 6E9B4 bx lr
}

struct Range { uint64_t begin, bytes; };
inline bool Overlap(Range a, Range b) {
    return a.begin < b.begin + b.bytes && b.begin < a.begin + a.bytes;
}

// Conservative supported layout: 1..64 topologically ordered bones, 1..2
// channels, and 2..8192 keys per present IPO. Unsupported valid structures retain
// their original loop. All checks precede writes and FP instructions.
inline bool Prepare(const uint8_t* guest, size_t size, uint32_t base,
                    const std::array<uint32_t, 16>& r) {
    auto valid = [size](Range range) {
        return range.begin && range.bytes && range.begin + range.bytes <= size;
    };
    const uint32_t skeleton = r[10], sp = r[13];
    if (!guest || r[11] || r[9] != 532 || r[8] != 200 ||
        !valid({skeleton, 0x64}) || !valid({sp, 320})) return false;
    const uint32_t count = Read<uint32_t>(guest + skeleton);
    const uint32_t channels_count = Read<uint32_t>(guest + skeleton + 0x4C);
    const uint32_t bones = Read<uint32_t>(guest + skeleton + 4);
    const uint32_t channels = Read<uint32_t>(guest + skeleton + 0x48);
    if (!count || count > 64 || !channels_count || channels_count > 2 || r[1] != channels_count)
        return false;
    const uint32_t stack_bytes = count * 80 + 72; // global-quat depth80 plus evaluate/key leaf72.
    if (sp < stack_bytes) return false;
    const Range writes[] = {{skeleton, 0x64}, {bones, count * 532},
                            {sp - stack_bytes, stack_bytes + 320}};
    for (size_t i = 0; i < 3; ++i) {
        if (!valid(writes[i])) return false;
        for (size_t j = 0; j < i; ++j) if (Overlap(writes[i], writes[j])) return false;
    }
    auto readonly = [&](Range range) {
        if (!valid(range)) return false;
        for (Range write : writes) if (Overlap(range, write)) return false;
        return true;
    };
    // The port reads the original PC-relative literal pools with its runtime ELF
    // base. Installation must verify kGuards once before writing the hook words.
    for (const CodeGuard& guard : kGuards)
        if (!readonly({uint64_t(base) + guard.offset, guard.bytes})) return false;
    if (!readonly({uint64_t(base) + kSqrtGotOffset, 4}) ||
        !readonly({channels, channels_count * 200})) return false;
    const uint32_t mask = Read<uint32_t>(guest + sp + 0x24);
    const uint32_t model = Read<uint32_t>(guest + skeleton + 8);
    if (!readonly({mask, count * 8}) || !readonly({model, 0xAC})) return false;
    if (!readonly({Read<uint32_t>(guest + model + 0xA8), 72})) return false;
    for (uint32_t bone = 0; bone < count; ++bone) {
        const uint32_t parent = Read<uint32_t>(guest + bones + bone * 532 + 0x1F8);
        if (parent != UINT32_MAX && parent >= bone) return false;
    }
    auto ipo = [&](uint32_t object, uint32_t stride) {
        const uint32_t data = Read<uint32_t>(guest + object);
        const uint32_t keys = Read<uint32_t>(guest + object + 4);
        const uint32_t key_count = Read<uint32_t>(guest + object + 8);
        return key_count >= 2 && key_count <= 8192 &&
            readonly({data, key_count * stride}) && readonly({keys, key_count * 4});
    };
    for (uint32_t channel = 0; channel < channels_count; ++channel) {
        const uint32_t object = channels + channel * 200;
        const uint32_t animation = Read<uint32_t>(guest + object + 0x4C);
        if (animation) {
            if (!readonly({animation, 0xCC})) return false;
            if (!readonly({Read<uint32_t>(guest + object + 0x8C), 16})) return false;
            const uint32_t ipos = Read<uint32_t>(guest + animation + 0xA0);
            if (!readonly({ipos, count * 28})) return false;
            for (uint32_t bone = 0; bone < count; ++bone)
                if (!ipo(ipos + bone * 28, 16)) return false;
            if (Read<uint32_t>(guest + object + 0x90) == 13 && !ipo(animation + 0xB0, 16))
                return false;
        }
        const uint32_t lo = Read<uint32_t>(guest + object + 0x10);
        const uint32_t hi = Read<uint32_t>(guest + object + 0x14);
        bool modifier = false;
        for (uint32_t bone = 0; bone < count; ++bone)
            modifier |= (lo & Read<uint32_t>(guest + mask + bone * 8)) ||
                        (hi & Read<uint32_t>(guest + mask + bone * 8 + 4));
        if (modifier && !readonly({Read<uint32_t>(guest + object + 0x58), count * 16}))
            return false;
    }
    return true;
}

// False leaves guest/VM/host state unchanged. Success updates the original
// observable loop state; r15 stays at SVC-next until the caller explicitly sets
// success PC to the unchanged tail209574 or fallback PC to original body2087C8.
// Caller also applies result_fpscr and result_nzcv to the Jit.
inline bool Run(uint8_t* guest, size_t size, uint32_t base,
                std::array<uint32_t, 16>& registers,
                std::array<uint32_t, 64>& extended_registers, uint32_t fpscr,
                uint32_t& result_fpscr, uint32_t& result_nzcv) {
    if (fpscr & 0x00370000u || !Prepare(guest, size, base, registers)) return false;
    Context c{guest, base, 0, fpscr & 0xF0000000u, registers, {}};
    std::memcpy(c.s.data(), extended_registers.data(), sizeof(c.s));
    uint64_t previous_fpcr, previous_fpsr;
    const uint64_t fpcr = fpscr & 0x07C00000u;
    asm volatile("mrs %0, fpcr\n\tmrs %1, fpsr\n\tmsr fpcr, %2\n\tmsr fpsr, xzr"
                 : "=&r"(previous_fpcr), "=&r"(previous_fpsr) : "r"(fpcr) : "memory");
    Loop(c);
    uint64_t raised;
    asm volatile("mrs %0, fpsr\n\tmsr fpcr, %1\n\tmsr fpsr, %2"
                 : "=&r"(raised) : "r"(previous_fpcr), "r"(previous_fpsr) : "memory");
    c.r[15] = registers[15];
    registers = c.r;
    std::memcpy(extended_registers.data(), c.s.data(), sizeof(c.s));
    result_fpscr = (fpscr & ~0xF0000000u) | c.nzcv | (static_cast<uint32_t>(raised) & 0x0800009Fu);
    result_nzcv = c.flags;
    return true;
}

}  // namespace warband_animation
