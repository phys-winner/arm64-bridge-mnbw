// Runnable on an A64 host; reuse the already validated SVC callback fixture.
#define main svc_trampoline_fixture_main
#include "svc_trampoline_test.cpp"
#undef main
#ifdef RSB_TEST_A64_GUEST
// Optional library regression for builds which also enable the A64 frontend.
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#endif

namespace {
constexpr uint32_t kDeep=0xa000, kStride=0x40;
constexpr unsigned kDepth=33;
constexpr uint32_t kAddW26=0x1100435a;

void ConfigureRsb(Machine& machine,bool cache) {
    machine.config.cache_rsb_pointer=cache;
    machine.jit=std::make_unique<Dynarmic::A32::Jit>(machine.config);
    machine.active=machine.jit.get();
}
void CompareRuns(Machine& reference,Machine& candidate,Action action,bool step=false) {
    if(step) {
        for(unsigned i=0;i<400;++i) {
            const auto a=reference.jit->Step(),b=candidate.jit->Step();
            assert(a==b); assert(reference.Result()==candidate.Result());
            if(Dynarmic::Has(a,Dynarmic::HaltReason::UserDefined1)) return;
        }
        assert(false);
    }
    reference.Run(); candidate.Run(); assert(reference.Result()==candidate.Result());
    if(action==Action::Halt || action==Action::AsyncHalt) {
        reference.jit->ClearHalt(); candidate.jit->ClearHalt();
        reference.Run(); candidate.Run(); assert(reference.Result()==candidate.Result());
    }
}
void CheckRsb(Action action,bool fuse,Form form=Form::Arm,bool step=false,bool cycles=false,
              Dynarmic::OptimizationFlag opt=Dynarmic::all_safe_optimizations) {
    Machine reference(fuse,action,form,cycles,opt),candidate(fuse,action,form,cycles,opt);
    ConfigureRsb(candidate,true);
    reference.Reset(); candidate.Reset(); CompareRuns(reference,candidate,action,step);
}
struct DeepMachine : Machine {
    bool thumb;
    DeepMachine(bool cache,bool t,Action a,bool fuse,bool cycles=false,
                Dynarmic::OptimizationFlag opt=Dynarmic::all_safe_optimizations)
        : Machine(fuse,a,Form::Arm,cycles,opt),thumb(t) {
        ConfigureRsb(*this,cache);
        for(unsigned i=0;i<kDepth;++i) {
            const uint32_t pc=kDeep+i*kStride,next=pc+kStride;
            if(thumb) {
                Write<uint16_t>(pc,0xb510); // PUSH {r4,lr}
                const uint32_t displacement=next-(pc+2+4);
                assert(displacement<2048);
                Write<uint16_t>(pc+2,0xf000);
                Write<uint16_t>(pc+4,static_cast<uint16_t>(0xf800|(displacement>>1)));
                Write<uint16_t>(pc+6,0x3001); // ADD r0,#1
                Write<uint16_t>(pc+8,0xbd10); // POP {r4,pc}
            } else {
                Write<uint32_t>(pc,0xe92d4010); // PUSH {r4,lr}
                Write<uint32_t>(pc+4,Branch(pc+4,next,true));
                Write<uint32_t>(pc+8,0xe2800001);
                Write<uint32_t>(pc+12,0xe8bd8010); // POP {r4,pc}
            }
        }
        const uint32_t leaf=kDeep+kDepth*kStride;
        if(thumb) {
            Write<uint16_t>(leaf,0xdf01); Write<uint16_t>(leaf+2,0x4770);
        } else {
            Write<uint32_t>(leaf,0xef000001); Write<uint32_t>(leaf+4,0xe12fff1e);
        }
    }
    void ResetDeep() {
        Reset(kDeep,0,thumb ? 0x30 : 0x10);
        if(thumb) jit->Regs()[14]=kThumbExit|1;
    }
    void CallSVC(uint32_t svc) override {
        if(svc==1 && action==Action::Invalidate) {
            action=Action::Normal; Machine::CallSVC(svc); action=Action::Invalidate;
            const uint32_t pc=kDeep+(thumb ? 6 : 8);
            if(thumb) Write<uint16_t>(pc,0x3002);
            else Write<uint32_t>(pc,0xe2800002);
            active->InvalidateCacheRange(pc,thumb ? 2 : 4);
        } else Machine::CallSVC(svc);
    }
};
void CheckDeep(bool thumb,Action action,bool fuse,bool step=false,bool cycles=false,
               Dynarmic::OptimizationFlag opt=Dynarmic::all_safe_optimizations) {
    DeepMachine reference(false,thumb,Action::Normal,fuse,cycles,opt);
    DeepMachine candidate(true,thumb,Action::Normal,fuse,cycles,opt);
    // Populate real return blocks before invalidating one from a native callback.
    reference.ResetDeep(); candidate.ResetDeep();
    reference.Run(); candidate.Run(); assert(reference.Result()==candidate.Result());
    reference.action=candidate.action=action;
    reference.ResetDeep(); candidate.ResetDeep();
    CompareRuns(reference,candidate,action,step);
    if(action==Action::Normal || action==Action::Halt || action==Action::AsyncHalt || action==Action::Clear)
        assert(candidate.jit->Regs()[0]==10+3+kDepth);
    if(action==Action::Invalidate) assert(candidate.jit->Regs()[0]==10+3+kDepth+1);
    if(action==Action::Nested) assert(candidate.jit->Regs()[0]==10+3+5+2+kDepth);
    assert(reference.memory==candidate.memory);
}
bool ContainsWord(const std::byte* code,uint32_t instruction) {
    for(unsigned i=0;i<64;++i) {
        uint32_t word; std::memcpy(&word,code+i*4,4);
        if(word==instruction) return true;
    }
    return false;
}
void CheckActivation(bool cache,bool cycles,Dynarmic::OptimizationFlag opt) {
    Machine machine(false,Action::Normal,Form::Arm,cycles,opt);
    ConfigureRsb(machine,cache);
    Dynarmic::Backend::Arm64::A32AddressSpace process(machine.config);
    const Dynarmic::A32::LocationDescriptor descriptor{kMain+4,Dynarmic::A32::PSR{0x10},Dynarmic::A32::FPSCR{0}};
    const auto* entry=process.GetOrEmit(descriptor);
    const bool expected=cache && !cycles && machine.config.HasOptimization(Dynarmic::OptimizationFlag::ReturnStackBuffer);
    assert(ContainsWord(entry,kAddW26)==expected);
}
void CheckMixedEntries() {
    DeepMachine reference(false,false,Action::Normal,true),candidate(true,false,Action::Normal,true);
    reference.ResetDeep(); candidate.ResetDeep();
    assert(reference.jit->Step()==candidate.jit->Step());
    assert(reference.Result()==candidate.Result());
    reference.Run(); candidate.Run(); assert(reference.Result()==candidate.Result());
    reference.ResetDeep(); candidate.ResetDeep();
    reference.jit->HaltExecution(); candidate.jit->HaltExecution();
    assert(reference.jit->Run()==candidate.jit->Run());
    assert(reference.Result()==candidate.Result());
    reference.jit->ClearHalt(); candidate.jit->ClearHalt();
    reference.Run(); candidate.Run(); assert(reference.Result()==candidate.Result());
}

#ifdef RSB_TEST_A64_GUEST
struct State64 {
    std::array<uint64_t,31> regs;
    std::array<Dynarmic::A64::Vector,32> vectors;
    uint64_t sp,pc,ticks;
    uint32_t pstate,fpcr,fpsr;
    std::vector<std::array<uint64_t,5>> callbacks;
    bool operator==(const State64&) const = default;
};
struct Machine64 : Dynarmic::A64::UserCallbacks {
    std::array<uint8_t,65536> memory{};
    Dynarmic::A64::UserConfig config;
    std::unique_ptr<Dynarmic::A64::Jit> jit,nested;
    Dynarmic::A64::Jit* active=nullptr;
    Action action;
    uint64_t ticks=0;
    std::vector<std::array<uint64_t,5>> callbacks;
    Machine64(bool rsb,bool cycles,Action a) : action(a) {
        Write<uint32_t>(kExit,0xd4000001); // SVC #0
        Write<uint32_t>(kNested,0xd4000041); // SVC #2
        Write<uint32_t>(kNested+4,0xd65f03c0); // RET
        for(unsigned i=0;i<kDepth;++i) {
            const uint32_t pc=kDeep+i*kStride;
            Write<uint32_t>(pc,0xa9bf7bfd); // STP x29,x30,[sp,#-16]!
            Write<uint32_t>(pc+4,0x94000000|((kStride-4)/4)); // BL next
            Write<uint32_t>(pc+8,0x91000400); // ADD x0,x0,#1
            Write<uint32_t>(pc+12,0xa8c17bfd); // LDP x29,x30,[sp],#16
            Write<uint32_t>(pc+16,0xd65f03c0);
        }
        Write<uint32_t>(kDeep+kDepth*kStride,0xd4000021); // SVC #1
        Write<uint32_t>(kDeep+kDepth*kStride+4,0xd65f03c0);
        config.callbacks=this;
        config.enable_cycle_counting=cycles;
        config.code_cache_size=8*1024*1024;
        if(!rsb) config.optimizations=static_cast<Dynarmic::OptimizationFlag>(
            static_cast<uint32_t>(Dynarmic::all_safe_optimizations) &
            ~static_cast<uint32_t>(Dynarmic::OptimizationFlag::ReturnStackBuffer));
        jit=std::make_unique<Dynarmic::A64::Jit>(config); active=jit.get();
    }
    template<class T> T Read(uint64_t a) {
        assert(a<=memory.size()-sizeof(T)); T result;
        std::memcpy(&result,memory.data()+a,sizeof(T)); return result;
    }
    template<class T> void Write(uint64_t a,T v) {
        assert(a<=memory.size()-sizeof(T)); std::memcpy(memory.data()+a,&v,sizeof(T));
    }
    uint8_t MemoryRead8(uint64_t a) override { return Read<uint8_t>(a); }
    uint16_t MemoryRead16(uint64_t a) override { return Read<uint16_t>(a); }
    uint32_t MemoryRead32(uint64_t a) override { return Read<uint32_t>(a); }
    uint64_t MemoryRead64(uint64_t a) override { return Read<uint64_t>(a); }
    Dynarmic::A64::Vector MemoryRead128(uint64_t a) override { return Read<Dynarmic::A64::Vector>(a); }
    void MemoryWrite8(uint64_t a,uint8_t v) override { Write(a,v); }
    void MemoryWrite16(uint64_t a,uint16_t v) override { Write(a,v); }
    void MemoryWrite32(uint64_t a,uint32_t v) override { Write(a,v); }
    void MemoryWrite64(uint64_t a,uint64_t v) override { Write(a,v); }
    void MemoryWrite128(uint64_t a,Dynarmic::A64::Vector v) override { Write(a,v); }
    void InterpreterFallback(uint64_t,size_t) override { assert(false); }
    void ExceptionRaised(uint64_t,Dynarmic::A64::Exception) override { assert(false); }
    void AddTicks(uint64_t n) override { ticks+=n; }
    uint64_t GetTicksRemaining() override { return 100000000; }
    uint64_t GetCNTPCT() override { return 0; }
    void Reset() {
        active=jit.get(); jit->SetRegisters({}); jit->SetVectors({});
        jit->SetRegister(0,10); jit->SetRegister(30,kExit);
        jit->SetPC(kDeep); jit->SetSP(0xf000);
        jit->SetPstate(0x60000000); jit->SetFpcr(0x00400000); jit->SetFpsr(0x0800009f);
        jit->ClearHalt(); callbacks.clear(); ticks=0;
    }
    void CallSVC(uint32_t svc) override {
        callbacks.push_back({svc,active->GetPC(),active->GetPstate(),active->GetFpcr(),active->GetFpsr()});
        if(svc==0) { active->HaltExecution(); return; }
        const uint64_t poison=0x9d; asm volatile("msr fpsr, %0" :: "r"(poison));
        active->SetRegister(0,active->GetRegister(0)+(svc==2 ? 2 : 3));
        if(svc==2) return;
        assert(svc==1);
        if(action==Action::Halt) active->HaltExecution();
        if(action==Action::Clear) active->ClearCache();
        if(action==Action::Invalidate) {
            Write<uint32_t>(kDeep+8,0x91000800); // ADD x0,x0,#2
            active->InvalidateCacheRange(kDeep+8,4);
        }
        if(action==Action::Nested) {
            if(!nested) nested=std::make_unique<Dynarmic::A64::Jit>(config);
            auto* outer=active; active=nested.get();
            active->SetRegisters({}); active->SetRegister(0,outer->GetRegister(0)+5);
            active->SetRegister(30,kExit); active->SetSP(0xe000); active->SetPC(kNested);
            active->SetPstate(outer->GetPstate()); active->SetFpcr(outer->GetFpcr()); active->SetFpsr(outer->GetFpsr());
            active->ClearHalt(); assert(Dynarmic::Has(active->Run(),Dynarmic::HaltReason::UserDefined1));
            const auto result=active->GetRegister(0); active=outer; active->SetRegister(0,result);
        }
    }
    Dynarmic::HaltReason Run() {
        for(unsigned retry=0;retry<5;++retry) {
            const auto hr=jit->Run();
            if(Dynarmic::Has(hr,Dynarmic::HaltReason::CacheInvalidation)) continue;
            assert(Dynarmic::Has(hr,Dynarmic::HaltReason::UserDefined1)); return hr;
        }
        assert(false); return static_cast<Dynarmic::HaltReason>(0);
    }
    State64 Result() {
        return {jit->GetRegisters(),jit->GetVectors(),jit->GetSP(),jit->GetPC(),ticks,
                jit->GetPstate(),jit->GetFpcr(),jit->GetFpsr(),callbacks};
    }
};
void CheckA64(bool step,bool cycles,Action action) {
    Machine64 reference(false,cycles,Action::Normal),candidate(true,cycles,Action::Normal);
    reference.Reset(); candidate.Reset(); reference.Run(); candidate.Run();
    assert(reference.Result()==candidate.Result());
    reference.action=candidate.action=action; reference.Reset(); candidate.Reset();
    if(step) {
        for(unsigned i=0;i<400;++i) {
            const auto a=reference.jit->Step(),b=candidate.jit->Step();
            assert(a==b); assert(reference.Result()==candidate.Result());
            if(Dynarmic::Has(a,Dynarmic::HaltReason::UserDefined1)) return;
        }
        assert(false);
    }
    assert(reference.Run()==candidate.Run()); assert(reference.Result()==candidate.Result());
    if(action==Action::Halt) {
        reference.jit->ClearHalt(); candidate.jit->ClearHalt();
        assert(reference.Run()==candidate.Run()); assert(reference.Result()==candidate.Result());
    }
    const uint64_t expected=10+3+kDepth+(action==Action::Nested ? 7 : action==Action::Invalidate ? 1 : 0);
    assert(candidate.jit->GetRegister(0)==expected); assert(reference.memory==candidate.memory);
}
#endif
}
int main() {
    unsigned checks=0;
    for(bool fuse : {false,true}) {
        for(auto action : {Action::Normal,Action::Pc,Action::Thumb,Action::It,Action::Fpscr,
                Action::StickyFlags,Action::CpsrFlags,Action::Lr,Action::ThumbLr,Action::Halt,
                Action::AsyncHalt,Action::Nested,Action::Invalidate,Action::Clear}) {
            CheckRsb(action,fuse); ++checks;
        }
        for(auto form : {Form::Conditional,Form::Zero,Form::NotBx,Form::Thumb}) {
            CheckRsb(Action::Normal,fuse,form); ++checks;
        }
        for(bool cycles : {false,true}) {
            CheckRsb(Action::Normal,fuse,Form::Arm,true,cycles); ++checks;
            CheckRsb(Action::Normal,fuse,Form::Arm,false,cycles,Dynarmic::no_optimizations); ++checks;
            for(bool thumb : {false,true}) {
                CheckDeep(thumb,Action::Normal,fuse,true,cycles); ++checks;
                CheckDeep(thumb,Action::Normal,fuse,false,cycles,
                    Dynarmic::OptimizationFlag::ConstProp|Dynarmic::OptimizationFlag::GetSetElimination); ++checks;
                for(auto action : {Action::Normal,Action::Nested,Action::Halt,Action::AsyncHalt,Action::Invalidate,Action::Clear}) {
                    CheckDeep(thumb,action,fuse,false,cycles); ++checks;
                }
            }
        }
    }
    for(bool cache : {false,true}) for(bool cycles : {false,true}) {
        CheckActivation(cache,cycles,Dynarmic::all_safe_optimizations); ++checks;
        CheckActivation(cache,cycles,Dynarmic::no_optimizations); ++checks;
    }
    CheckMixedEntries(); ++checks;
#ifdef RSB_TEST_A64_GUEST
    for(bool cycles : {false,true}) {
        CheckA64(true,cycles,Action::Normal); ++checks;
        for(auto action : {Action::Normal,Action::Nested,Action::Halt,Action::Invalidate,Action::Clear}) {
            CheckA64(false,cycles,action); ++checks;
        }
    }
    const char* frontend="A32/A64 guests on A64 host";
#else
    const char* frontend="A32 guest on A64 host";
#endif
    std::printf("RSB pointer: PASS %u differential/activation checks, ARM/Thumb depth=%u/wrap, callbacks/nested/halt-resume/invalidation/clear, Run/Step/mixed/prehalt, RSB-disabled/cycles, %s\n",checks,kDepth,frontend);
    for(bool cache : {false,true}) {
        Machine machine(true,Action::Normal); ConfigureRsb(machine,cache); machine.trace=false;
        machine.Write<uint32_t>(kThunk,0xe2800001); // Pure guest leaf: ADD r0,#1; BX LR
        machine.Reset(kLoop,100); machine.Run(); machine.Reset(kLoop,1000000);
        const auto start=std::chrono::steady_clock::now(); machine.Run();
        const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
        assert(machine.jit->Regs()[0]==1000010);
        std::printf("Guest call/return cache_rsb_pointer=%d: 1000000 calls in %lld us\n",cache,static_cast<long long>(us));
    }
    return 0;
}
