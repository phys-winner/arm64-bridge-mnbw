#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/backend/arm64/a32_address_space.h>
#include <dynarmic/frontend/A32/a32_location_descriptor.h>
#include <dynarmic/frontend/A32/a32_types.h>

namespace {
constexpr uint32_t kMain=0x1000, kExit=0x2000, kThunk=0x3000;
constexpr uint32_t kNested=0x4000, kAlternate=0x5000, kThumbExit=0x6000, kLoop=0x8000;
enum class Action { Normal, Pc, Thumb, It, Fpscr, StickyFlags, CpsrFlags, Lr, ThumbLr, Halt, AsyncHalt, Nested, Invalidate, Clear };
enum class Form { Arm, Conditional, Zero, NotBx, Thumb };
uint32_t Branch(uint32_t from,uint32_t to,bool link) {
    return (link ? 0xeb000000u : 0x1a000000u) |
           ((static_cast<uint32_t>((static_cast<int32_t>(to)-static_cast<int32_t>(from)-8)/4)) & 0x00ffffffu);
}
struct State {
    std::array<uint32_t,16> regs;
    std::array<uint32_t,64> ext;
    uint32_t cpsr, fpscr;
    std::vector<std::array<uint32_t,4>> callbacks;
    std::vector<uint32_t> exceptions;
    std::array<uint8_t,16> trampoline;
    uint64_t ticks;
    bool operator==(const State&) const = default;
};
struct Machine : Dynarmic::A32::UserCallbacks {
    std::array<uint8_t,65536> memory{};
    Dynarmic::A32::UserConfig config;
    std::unique_ptr<Dynarmic::A32::Jit> jit, nested;
    Dynarmic::A32::Jit* active=nullptr;
    Action action;
    Form form;
    std::vector<std::array<uint32_t,4>> callbacks;
    std::vector<uint32_t> exceptions;
    uint64_t ticks=0;
    bool trace=true;
    bool tail_svc=false;
    Machine(bool fuse,Action a,Form f=Form::Arm,bool cycles=false,
            Dynarmic::OptimizationFlag opt=Dynarmic::all_safe_optimizations) : action(a),form(f) {
        const uint32_t main[]={0xe1a0400e,Branch(kMain+4,kThunk,true),0xee300a00,0xeef12a10,
            0xe2800001,0xe1a0e004,0xe12fff1e};
        std::memcpy(memory.data()+kMain,main,sizeof(main));
        Write<uint32_t>(kExit,0xef000000);
        Write<uint32_t>(kThunk,0xef000001);
        Write<uint32_t>(kThunk+4,0xe12fff1e);
        Write<uint32_t>(kThunk+8,0xe12fff1e);
        Write<uint32_t>(kNested,0xef000002);
        Write<uint32_t>(kNested+4,0xe12fff1e);
        Write<uint32_t>(kAlternate,0xe3a0004d);
        Write<uint32_t>(kAlternate+4,0xe12fff1e);
        Write<uint16_t>(kThumbExit,0xdf00);
        const uint32_t loop[]={0xe1a0400e,0xe1a05001,Branch(kLoop+8,kThunk,true),
            0xe2555001,Branch(kLoop+16,kLoop+8,false),0xe1a0e004,0xe12fff1e};
        std::memcpy(memory.data()+kLoop,loop,sizeof(loop));
        if(f==Form::Conditional) Write<uint32_t>(kThunk,0x1f000001);
        if(f==Form::Zero) Write<uint32_t>(kThunk,0xef000000);
        if(f==Form::NotBx) Write<uint32_t>(kThunk+4,0xe3a0002a);
        if(f==Form::Thumb) Write<uint32_t>(kThunk,0x4770df01);
        config.callbacks=this;
        config.arch_version=Dynarmic::A32::ArchVersion::v7;
        config.enable_cycle_counting=cycles;
        config.fuse_svc_trampolines=fuse;
        config.optimizations=opt;
        config.code_cache_size=8*1024*1024;
        jit=std::make_unique<Dynarmic::A32::Jit>(config);
        active=jit.get();
    }
    template<class T> T Read(uint32_t a) {
        assert(static_cast<uint64_t>(a)+sizeof(T)<=memory.size());
        T value; std::memcpy(&value,memory.data()+a,sizeof(value)); return value;
    }
    template<class T> void Write(uint32_t a,T value) {
        assert(static_cast<uint64_t>(a)+sizeof(T)<=memory.size());
        std::memcpy(memory.data()+a,&value,sizeof(value));
    }
    std::optional<uint32_t> MemoryReadCode(uint32_t a) override {
        if(tail_svc && a==0xfffffffcu) return 0xef000001;
        if(static_cast<uint64_t>(a)+4>memory.size()) return std::nullopt;
        return Read<uint32_t>(a);
    }
    uint8_t MemoryRead8(uint32_t a) override { return Read<uint8_t>(a); }
    uint16_t MemoryRead16(uint32_t a) override { return Read<uint16_t>(a); }
    uint32_t MemoryRead32(uint32_t a) override { return Read<uint32_t>(a); }
    uint64_t MemoryRead64(uint32_t a) override { return Read<uint64_t>(a); }
    void MemoryWrite8(uint32_t a,uint8_t v) override { Write(a,v); }
    void MemoryWrite16(uint32_t a,uint16_t v) override { Write(a,v); }
    void MemoryWrite32(uint32_t a,uint32_t v) override { Write(a,v); }
    void MemoryWrite64(uint32_t a,uint64_t v) override { Write(a,v); }
    void InterpreterFallback(uint32_t,size_t) override { assert(false); }
    void ExceptionRaised(uint32_t pc,Dynarmic::A32::Exception e) override {
        exceptions.push_back(pc); exceptions.push_back(static_cast<uint32_t>(e));
        active->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    }
    void CallSVC(uint32_t svc) override {
        if(trace) callbacks.push_back({svc,active->Regs()[15],active->Cpsr(),active->Fpscr()});
        if(svc==0) { active->HaltExecution(Dynarmic::HaltReason::UserDefined1); return; }
        const uint64_t poison=0x9d;
        asm volatile("msr fpsr, %0" :: "r"(poison));
        if(svc==2) { active->Regs()[0]+=2; return; }
        assert(svc==1);
        active->Regs()[0]+=3;
        switch(action) {
        case Action::Normal: break;
        case Action::Pc: active->Regs()[15]=kAlternate; break;
        case Action::Thumb: active->SetCpsr(active->Cpsr()|0x20); break;
        case Action::It: active->SetCpsr(active->Cpsr()|0x2000); break;
        case Action::Fpscr: active->SetFpscr(0x01c0009f); break;
        case Action::StickyFlags: active->SetFpscr(active->Fpscr()|0x0800009f); break;
        case Action::CpsrFlags: active->SetCpsr(active->Cpsr()|0xf80f0000); break;
        case Action::Lr: active->Regs()[14]=kExit; break;
        case Action::ThumbLr: active->Regs()[14]=kThumbExit|1; break;
        case Action::Halt: active->HaltExecution(Dynarmic::HaltReason::UserDefined1); break;
        case Action::AsyncHalt: {
            auto* target=active;
            std::thread sender([target] { target->HaltExecution(Dynarmic::HaltReason::UserDefined1); });
            sender.join();
            break;
        }
        case Action::Nested: {
            if(!nested) nested=std::make_unique<Dynarmic::A32::Jit>(config);
            auto* outer=active;
            active=nested.get();
            active->Regs().fill(0); active->ExtRegs().fill(0);
            active->Regs()[0]=outer->Regs()[0]+5;
            active->Regs()[14]=kExit; active->Regs()[15]=kNested;
            active->SetCpsr(0x10); active->SetFpscr(outer->Fpscr());
            active->ClearHalt();
            assert(Dynarmic::Has(active->Run(),Dynarmic::HaltReason::UserDefined1));
            const uint32_t result=active->Regs()[0];
            active=outer; active->Regs()[0]=result;
            break;
        }
        case Action::Invalidate:
            Write<uint32_t>(kThunk+4,0xe3a0002a);
            active->InvalidateCacheRange(kThunk+4,4);
            break;
        case Action::Clear: active->ClearCache(); break;
        }
    }
    void AddTicks(uint64_t n) override { ticks+=n; }
    uint64_t GetTicksRemaining() override { return 100000000; }
    void Reset(uint32_t pc=kMain,uint32_t count=0,uint32_t cpsr=0x10,uint32_t fpscr=0) {
        active=jit.get(); jit->Regs().fill(0); jit->ExtRegs().fill(0xa5a55a5a);
        jit->Regs()[0]=10; jit->Regs()[1]=count; jit->Regs()[13]=0xf000;
        jit->Regs()[14]=kExit; jit->Regs()[15]=pc;
        if(form==Form::Thumb) { jit->Regs()[15]=kThunk; jit->Regs()[14]=kThumbExit|1; cpsr|=0x20; }
        jit->SetCpsr(cpsr); jit->SetFpscr(fpscr); jit->ClearHalt();
        callbacks.clear(); exceptions.clear(); ticks=0;
    }
    void Run() {
        for(unsigned retry=0;retry<5;++retry) {
            const auto hr=jit->Run();
            if(Dynarmic::Has(hr,Dynarmic::HaltReason::CacheInvalidation)) continue;
            assert(Dynarmic::Has(hr,Dynarmic::HaltReason::UserDefined1)); return;
        }
        assert(false);
    }
    State Result() {
        State result{jit->Regs(),jit->ExtRegs(),jit->Cpsr(),jit->Fpscr(),callbacks,exceptions,{},ticks};
        std::memcpy(result.trampoline.data(),memory.data()+kThunk,result.trampoline.size());
        return result;
    }
};
void Check(bool step,Action action,Form form=Form::Arm,bool cycles=false,
           Dynarmic::OptimizationFlag opt=Dynarmic::all_safe_optimizations,uint32_t cpsr=0x10,uint32_t fpscr=0) {
    Machine reference(false,action,form,cycles,opt),candidate(true,action,form,cycles,opt);
    reference.Reset(kMain,0,cpsr,fpscr); candidate.Reset(kMain,0,cpsr,fpscr);
    if(step) {
        for(unsigned i=0;i<20;++i) {
            const auto a=reference.jit->Step(), b=candidate.jit->Step();
            assert(a==b); assert(reference.Result()==candidate.Result());
            if(Dynarmic::Has(a,Dynarmic::HaltReason::UserDefined1)) return;
        }
        assert(false);
    } else {
        reference.Run(); candidate.Run(); assert(reference.Result()==candidate.Result());
        assert(!candidate.callbacks.empty());
        if(action==Action::Halt || action==Action::AsyncHalt) {
            assert(candidate.jit->Regs()[15]==kThunk+4);
            reference.jit->ClearHalt(); candidate.jit->ClearHalt();
            reference.Run(); candidate.Run(); assert(reference.Result()==candidate.Result());
        }
        if(action==Action::Normal && form==Form::Arm) assert(candidate.jit->Regs()[0]==14);
        if(action==Action::Invalidate) assert(candidate.jit->Regs()[0]==43);
    }
}
void CheckDependency(bool fuse) {
    Machine machine(fuse,Action::Normal);
    Dynarmic::Backend::Arm64::A32AddressSpace process(machine.config);
    const Dynarmic::A32::LocationDescriptor descriptor{kThunk,Dynarmic::A32::PSR{0x10},Dynarmic::A32::FPSCR{0}};
    assert(process.GetOrEmit(descriptor));
    boost::icl::interval_set<uint32_t> ranges;
    ranges.add(boost::icl::discrete_interval<uint32_t>::closed(kThunk+4,kThunk+7));
    process.InvalidateCacheRanges(ranges);
    assert((process.Get(descriptor)==nullptr)==fuse);
}
void CheckTail() {
    Machine reference(false,Action::Normal),candidate(true,Action::Normal);
    reference.tail_svc=candidate.tail_svc=true;
    reference.Write<uint32_t>(0,0xe12fff1e); candidate.Write<uint32_t>(0,0xe12fff1e);
    reference.Reset(0xfffffffcu); candidate.Reset(0xfffffffcu);
    reference.Run(); candidate.Run(); assert(reference.Result()==candidate.Result());
    assert(candidate.jit->Regs()[0]==13);
    Dynarmic::Backend::Arm64::A32AddressSpace process(candidate.config);
    const Dynarmic::A32::LocationDescriptor descriptor{0xfffffffcu,Dynarmic::A32::PSR{0x10},Dynarmic::A32::FPSCR{0}};
    assert(process.GetOrEmit(descriptor));
    boost::icl::interval_set<uint32_t> ranges;
    ranges.add(boost::icl::discrete_interval<uint32_t>::closed(0,3));
    process.InvalidateCacheRanges(ranges);
    assert(process.Get(descriptor));
}
}
int main() {
    unsigned checks=0;
    for(auto action : {Action::Normal,Action::Pc,Action::Thumb,Action::It,Action::Fpscr,Action::StickyFlags,Action::CpsrFlags,
            Action::Lr,Action::ThumbLr,Action::Halt,Action::AsyncHalt,Action::Nested,Action::Invalidate,Action::Clear}) {
        Check(false,action); ++checks;
    }
    for(auto form : {Form::Conditional,Form::Zero,Form::NotBx,Form::Thumb}) {
        Check(false,Action::Normal,form); ++checks;
    }
    Check(true,Action::Normal); ++checks;
    Check(false,Action::Normal,Form::Arm,true); ++checks;
    Check(false,Action::Normal,Form::Arm,false,Dynarmic::no_optimizations); ++checks;
    Check(false,Action::Normal,Form::Arm,false,
          Dynarmic::OptimizationFlag::ConstProp|Dynarmic::OptimizationFlag::GetSetElimination); ++checks;
    for(auto cpsr : {0x10u,0xf8000010u}) for(auto fpscr : {0u,0x0800009fu,0x03400000u}) {
        Check(false,Action::Normal,Form::Arm,false,Dynarmic::all_safe_optimizations,cpsr,fpscr); ++checks;
    }
    CheckDependency(false); CheckDependency(true); checks+=2;
    CheckTail(); ++checks;
    std::printf("SVC trampoline: PASS %u differential/dependency checks, registers/CPSR/FPSCR/callback trace, FP-pollution/PC/T/IT/LR/halt/async/nested/invalidation/clear/Step/cycles/RSB-disabled/wrap-edge\n",checks);
    for(bool fuse : {false,true}) {
        Machine machine(fuse,Action::Normal); machine.trace=false;
        machine.Reset(kLoop,100); machine.Run();
        machine.Reset(kLoop,1000000);
        const auto start=std::chrono::steady_clock::now(); machine.Run();
        const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
        assert(machine.jit->Regs()[0]==3000010);
        std::printf("SVC loop fuse=%d: 1000000 guest/host calls in %lld us\n",fuse,static_cast<long long>(us));
    }
    return 0;
}
