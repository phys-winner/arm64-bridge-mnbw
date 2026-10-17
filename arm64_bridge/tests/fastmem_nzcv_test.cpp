// Run on an A64 host and link against the current build's libdynarmic.a.
#if !defined(__aarch64__)
#error This regression must execute the real A64 backend.
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <sys/mman.h>
#include <unistd.h>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/frontend/A32/a32_types.h>

namespace {
constexpr size_t kSize = 0x100000000ull;
constexpr uint32_t kCode = 0x1000, kData = 0x8000, kHole = 0x10000;
constexpr uint32_t kNzcv = 0xf0000000;
constexpr uint32_t kNormal[] = {
    0xe1500001, // cmp r0,r1
    0xe59c4000, // ldr r4,[r12]
    0xe58c4004, // str r4,[r12,#4]
    0xe5dc5008, // ldrb r5,[r12,#8]
    0xe5cc5009, // strb r5,[r12,#9]
    0xe1dc60ba, // ldrh r6,[r12,#10]
    0xe1cc60bc, // strh r6,[r12,#12]
    0xed9c0b04, // vldr d0,[r12,#16]
    0xed8c0b06, // vstr d0,[r12,#24]
    0xe0a28003, // adc r8,r2,r3
    0xe0c27003, // sbc r7,r2,r3
    0xe10f9000, // mrs r9,cpsr
    0x23a0a001, // movcs r10,#1
    0x33a0a002, // movcc r10,#2
    0x03a0b003, // moveq r11,#3
    0x13a0b004, // movne r11,#4
    0xef000000, // svc #0
};
constexpr uint32_t kOrdered[] = {
    0xe1500001, // cmp r0,r1
    0xe19c4c9f, // lda r4,[r12]
    0xe28cb004, // add r11,r12,#4
    0xe18bfc94, // stl r4,[r11]
    0xe28cb008, // add r11,r12,#8
    0xe1db5c9f, // ldab r5,[r11]
    0xe28cb009, // add r11,r12,#9
    0xe1cbfc95, // stlb r5,[r11]
    0xe28cb00a, // add r11,r12,#10
    0xe1fb6c9f, // ldah r6,[r11]
    0xe28cb00c, // add r11,r12,#12
    0xe1ebfc96, // stlh r6,[r11]
    0xed9c0b04, // vldr d0,[r12,#16]
    0xed8c0b06, // vstr d0,[r12,#24]
    0xe0a28003, // adc r8,r2,r3
    0xe0c27003, // sbc r7,r2,r3
    0xe10f9000, // mrs r9,cpsr
    0x23a0a001, // movcs r10,#1
    0x33a0a002, // movcc r10,#2
    0x03a0b003, // moveq r11,#3
    0x13a0b004, // movne r11,#4
    0xef000000, // svc #0
};
const char* const kPaths[] = {"callback", "direct", "fault", "recompile", "page-table", "page-fallback"};
uint32_t CompareFlags(uint32_t a, uint32_t b) {
    const uint32_t result = a - b;
    return (result & 0x80000000) | (result == 0 ? 1u << 30 : 0) |
           (a >= b ? 1u << 29 : 0) | (((a ^ b) & (a ^ result)) >> 3 & (1u << 28));
}

struct Machine : Dynarmic::A32::UserCallbacks {
    uint8_t* memory = static_cast<uint8_t*>(mmap(nullptr, kSize, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    std::array<uint8_t, 16384> shadow{};
    using PageTable = std::array<uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;
    std::unique_ptr<PageTable> pages;
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    unsigned path, callbacks = 0;
    bool abort_read = false, ir_probe, ordered;

    Machine(unsigned path_, bool check_abort, bool ordered_, bool probe = false)
        : path(path_), ir_probe(probe), ordered(ordered_) {
        assert(memory != MAP_FAILED);
        if (ordered) std::memcpy(memory + kCode, kOrdered, sizeof(kOrdered));
        else std::memcpy(memory + kCode, kNormal, sizeof(kNormal));
        if (path == 2 || path == 3) assert(mprotect(memory + kHole, shadow.size(), PROT_NONE) == 0);
        Dynarmic::A32::UserConfig config;
        config.callbacks = this;
        config.arch_version = Dynarmic::A32::ArchVersion::v8;
        config.enable_cycle_counting = false;
        config.always_little_endian = true;
        config.check_halt_on_memory_access = check_abort;
        config.recompile_on_fastmem_failure = path == 3;
        config.code_cache_size = 8 * 1024 * 1024;
        if (path >= 1 && path <= 3) config.fastmem_pointer = reinterpret_cast<uintptr_t>(memory);
        if (path >= 4) {
            pages = std::make_unique<PageTable>();
            if (path == 4) (*pages)[kData >> 12] = memory + kData;
            config.page_table = pages.get();
        }
        jit = std::make_unique<Dynarmic::A32::Jit>(config);
    }
    ~Machine() { jit.reset(); assert(munmap(memory, kSize) == 0); }
    uint32_t DataAddress() const { return path == 2 || path == 3 ? kHole : kData; }
    uint8_t* Address(uint32_t addr, size_t size) {
        assert(uint64_t(addr) + size <= kSize);
        if ((path == 2 || path == 3) && addr >= kHole && uint64_t(addr) + size <= kHole + shadow.size())
            return shadow.data() + addr - kHole;
        return memory + addr;
    }
    void PolluteFlags() {
        const uint64_t poison = kNzcv;
        asm volatile("msr nzcv, %0" :: "r"(poison) : "cc");
    }
    template<class T> T Read(uint32_t addr) {
        ++callbacks;
        T value;
        std::memcpy(&value, Address(addr, sizeof(T)), sizeof(T));
        if (abort_read) jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
        PolluteFlags();
        return value;
    }
    template<class T> void Write(uint32_t addr, T value) {
        ++callbacks;
        std::memcpy(Address(addr, sizeof(T)), &value, sizeof(T));
        PolluteFlags();
    }
    bool PreCodeReadHook(bool thumb, uint32_t pc, Dynarmic::A32::IREmitter& ir) override {
        if (!ir_probe) return true;
        assert(!thumb && pc == kCode);
        using namespace Dynarmic;
        using A32::Reg;
        // Ordinary CMP stores CPSR before accesses. This valid IR deliberately
        // delays that store, so the callback really must preserve live host NZCV.
        const auto flags = ir.NZCVFrom(ir.SubWithCarry(ir.GetRegister(Reg::R0), ir.GetRegister(Reg::R1), ir.Imm1(true)));
        const auto base = ir.GetRegister(Reg::R12);
        const auto acc = ordered ? IR::AccType::ORDERED : IR::AccType::NORMAL;
        ir.current_location = ir.current_location.SetPC(kCode + 4);
        const auto word = ir.ReadMemory32(base, acc);
        ir.WriteMemory32(ir.Add(base, ir.Imm32(4)), word, acc);
        const auto byte = ir.ReadMemory8(ir.Add(base, ir.Imm32(8)), acc);
        ir.WriteMemory8(ir.Add(base, ir.Imm32(9)), byte, acc);
        const auto half = ir.ReadMemory16(ir.Add(base, ir.Imm32(10)), acc);
        ir.WriteMemory16(ir.Add(base, ir.Imm32(12)), half, acc);
        const auto wide = ir.ReadMemory64(ir.Add(base, ir.Imm32(16)), acc);
        ir.WriteMemory64(ir.Add(base, ir.Imm32(24)), wide, acc);
        ir.SetCpsrNZCV(flags);
        const auto carry = ir.GetCFlagFromNZCV(flags);
        ir.SetRegister(Reg::R8, ir.AddWithCarry(ir.GetRegister(Reg::R2), ir.GetRegister(Reg::R3), carry));
        ir.SetRegister(Reg::R7, ir.SubWithCarry(ir.GetRegister(Reg::R2), ir.GetRegister(Reg::R3), carry));
        ir.SetRegister(Reg::R9, ir.GetCpsr());
        ir.SetRegister(Reg::R10, ir.ConditionalSelect(IR::Cond::CS, ir.Imm32(1), ir.Imm32(2)));
        ir.SetRegister(Reg::R11, ir.ConditionalSelect(IR::Cond::EQ, ir.Imm32(3), ir.Imm32(4)));
        ir.BranchWritePC(ir.Imm32(kCode + sizeof(kNormal)));
        ir.CallSupervisor(ir.Imm32(0));
        ir.SetTerm(IR::Term::CheckHalt{IR::Term::ReturnToDispatch{}});
        return false;
    }
    std::optional<uint32_t> MemoryReadCode(uint32_t addr) override {
        assert(addr >= kCode && addr < kCode + sizeof(kOrdered));
        uint32_t value;
        std::memcpy(&value, memory + addr, 4);
        return value;
    }
    uint8_t MemoryRead8(uint32_t a) override { return Read<uint8_t>(a); }
    uint16_t MemoryRead16(uint32_t a) override { return Read<uint16_t>(a); }
    uint32_t MemoryRead32(uint32_t a) override { return Read<uint32_t>(a); }
    uint64_t MemoryRead64(uint32_t a) override { return Read<uint64_t>(a); }
    void MemoryWrite8(uint32_t a, uint8_t v) override { Write(a, v); }
    void MemoryWrite16(uint32_t a, uint16_t v) override { Write(a, v); }
    void MemoryWrite32(uint32_t a, uint32_t v) override { Write(a, v); }
    void MemoryWrite64(uint32_t a, uint64_t v) override { Write(a, v); }
    void InterpreterFallback(uint32_t, size_t) override { assert(false); }
    void ExceptionRaised(uint32_t, Dynarmic::A32::Exception) override { assert(false); }
    void CallSVC(uint32_t) override { jit->HaltExecution(Dynarmic::HaltReason::UserDefined1); }
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return UINT64_MAX; }

    void Prepare(uint32_t a, uint32_t b) {
        std::array<uint8_t, 32> bytes{};
        for (unsigned i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(i * 7 + 3);
        std::memcpy(Address(DataAddress(), bytes.size()), bytes.data(), bytes.size());
        jit->Regs().fill(0);
        jit->Regs()[0] = a;
        jit->Regs()[1] = b;
        jit->Regs()[2] = 17;
        jit->Regs()[3] = 9;
        jit->Regs()[12] = DataAddress();
        jit->Regs()[15] = kCode;
        jit->SetCpsr(0x10);
        jit->ClearHalt(Dynarmic::HaltReason::UserDefined1 | Dynarmic::HaltReason::MemoryAbort);
        callbacks = 0;
    }
    void Run(uint32_t a, uint32_t b) {
        Prepare(a, b);
        assert(Dynarmic::Has(jit->Run(), Dynarmic::HaltReason::UserDefined1));
        const uint32_t flags = CompareFlags(a, b), carry = (flags >> 29) & 1;
        assert(jit->Regs()[8] == 26 + carry);
        assert(jit->Regs()[7] == 8 - (1 - carry));
        assert((jit->Regs()[9] & kNzcv) == flags);
        assert((jit->Cpsr() & kNzcv) == flags);
        assert(jit->Regs()[10] == (carry ? 1u : 2u));
        assert(jit->Regs()[11] == (a == b ? 3u : 4u));
        const uint8_t* bytes = Address(DataAddress(), 32);
        assert(std::memcmp(bytes, bytes + 4, 4) == 0);
        assert(bytes[8] == bytes[9]);
        assert(std::memcmp(bytes + 10, bytes + 12, 2) == 0);
        assert(std::memcmp(bytes + 16, bytes + 24, 8) == 0);
        // The ARM VLDR/VSTR pair becomes four ATOMIC 32-bit accesses;
        // the delayed-IR probe deliberately emits two genuine 64-bit accesses.
        const unsigned expected_calls = path == 1 || path == 4 ? 0 : ir_probe ? 8 : 10;
        if (callbacks != expected_calls)
            std::fprintf(stderr, "callbacks=%u expected=%u path=%s delayed_IR=%u ordered=%u\n",
                         callbacks, expected_calls, kPaths[path], ir_probe, ordered);
        assert(callbacks == expected_calls);
    }
};
} // namespace

int main() {
    unsigned runs = 0;
    constexpr std::array<std::array<uint32_t, 2>, 5> pairs = {{{0, 0}, {0, 1}, {1, 0}, {0x80000000, 1}, {0x7fffffff, 0xffffffff}}};
    for (bool check_abort : {false, true}) {
        for (bool ordered : {false, true}) {
            for (bool probe : {false, true}) {
                for (unsigned path = 0; path < std::size(kPaths); ++path) {
                    Machine m(path, check_abort, ordered, probe);
                    for (unsigned repeat = 0; repeat < 3; ++repeat) {
                        for (const auto& pair : pairs) { m.Run(pair[0], pair[1]); ++runs; }
                    }
                    // Fault once with a fresh cache: a recompiled callback-only
                    // instruction no longer takes this fastmem abort-check path.
                    if (!probe && check_abort && (path == 2 || path == 3 || path == 5)) {
                        Machine aborted(path, true, ordered);
                        aborted.Prepare(0x80000000, 1);
                        aborted.abort_read = true;
                        assert(Dynarmic::Has(aborted.jit->Run(), Dynarmic::HaltReason::MemoryAbort));
                        assert(aborted.jit->Regs()[15] == kCode + 4);
                        assert((aborted.jit->Cpsr() & kNzcv) == CompareFlags(0x80000000, 1));
                        assert(aborted.callbacks == 1);
                        aborted.abort_read = false;
                        aborted.Run(0, 1);
                    }
                    std::printf("NZCV check_abort=%u ordered=%u delayed_IR=%u path=%s: PASS\n", check_abort, ordered, probe, kPaths[path]);
                }
            }
        }
    }
    std::printf("fastmem NZCV: PASS %u CMP/multi-access/ADC/SBC/predication runs; callback pollution, fault/recompile, 8/16/32/64-bit, normal/ordered, abort checks false/true; host page=%ld\n", runs, sysconf(_SC_PAGESIZE));
}
