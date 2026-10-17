#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#include <string>
#include <optional>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

class ElfLoader;
class ThunkManager;
class GuestCp15;
struct AAssetManager;

class DynarmicHost : public Dynarmic::A32::UserCallbacks {
public:
    // The full 32-bit guest address space. Mapping all of it means fastmem can resolve any
    // guest address as base + zext32(vaddr) without ever faulting.
    static constexpr size_t GUEST_ADDRESS_SPACE = 0x100000000ULL; // 4 GiB
    // Fallback if the 4 GiB reservation is refused: no fastmem, bounds-checked callbacks.
    static constexpr size_t GUEST_FALLBACK_SIZE = 0x80000000ULL;  // 2 GiB
    // Unity, Mono and guest plugins together settle around a couple of dozen threads.
    static constexpr size_t kMaxGuestThreads = 64;
    // The exclusive monitor is indexed by processor_id, and every Jit needs its
    // own - including the extra ones nested guest calls run on. Sizing this by
    // thread count alone let processor_id run off the end of the monitor's arrays,
    // which is silent corruption and makes ldrex/strex answer nonsense.
    static constexpr size_t kMaxJits = 1024;
    static constexpr uint32_t GUEST_STACK_SIZE = 0x00100000;  // 1 MiB per guest thread

    DynarmicHost();
    ~DynarmicHost() override;

    bool Initialize();
    bool LoadGame(const std::string& elf_path);
    bool LoadGameFromMemory(const uint8_t* data, size_t size);

    // Loads STORED guest APK slices, relocates them together, then runs initializers.
    struct GuestLibrary {
        std::string name;        // soname, for logs and dlsym()
        std::string asset_path;  // relative to NativeActivity's AssetManager
    };
    bool LoadGuestLibraries(AAssetManager* asset_manager, const std::vector<GuestLibrary>& libs);

    // One Jit per guest thread, sharing the memory map and the exclusive monitor.
    // Unity's NativeActivity glue blocks in pthread_cond_wait until the thread it
    // just created signals back, so a single-Jit design deadlocks before the first
    // frame. Created lazily on whichever host thread asks for it.
    Dynarmic::A32::Jit& CurrentJit();

    // Calls guest code from inside a thunk. Uses a Jit of its own, so it works
    // where RunFunction has to defer: bsearch's comparator has to run before
    // bsearch can return an answer, it cannot be postponed.
    uint32_t CallGuestNested(uint32_t target_pc, const std::vector<uint32_t>& args = {});

    // Runs start_func(arg) on a fresh host thread with its own Jit and stack.
    void SpawnGuestThread(uint32_t start_func, uint32_t arg);

    // Top of the calling thread's guest stack; Boehm needs the range to scan it.
    uint32_t GuestStackTop();
    // Lowest address the calling thread's guest stack may reach. Slots are packed
    // with no gap and the whole 32-bit space is mapped, so running past this writes
    // into the next thread's stack instead of faulting.
    uint32_t GuestStackLimit();

    uint32_t RunFunction(uint32_t target_pc, const std::vector<uint32_t>& args = {});

    struct BootstrapProbeResult {
        uint32_t requested_pc = 0;
        uint32_t fetched_pc = 0;
        uint32_t final_pc = 0;
        uint32_t halt_reason = 0;
        bool code_fetch_observed = false;
        bool pc_advanced = false;
    };
    void ArmBootstrapProbe(uint32_t entry_pc);
    BootstrapProbeResult GetBootstrapProbeResult() const;

    uint8_t* GetGuestMemoryBase() const { return m_memory; }
    size_t GetGuestMemorySize() const { return m_memory_size; }
    // The last library in the list - the one that owns the game's entry points.
    ElfLoader& GetElfLoader() { return *m_elf_loader; }
    ElfLoader* GetLibrary(const std::string& name);
    ThunkManager& GetThunkManager() { return *m_thunk_manager; }
    std::recursive_mutex& GetMutex() { return m_mutex; }
    uint32_t FindSymbol(const std::string& name);
    // Drop any cached translation of a guest range that was just rewritten.
    void InvalidateGuestCode(uint32_t addr, size_t size);
    uint32_t GuestAlloc(size_t size, size_t alignment = 8);
    void GuestFree(uint32_t addr);
    // Restore anonymous-mapping semantics without faulting every page into RSS.
    // Full host pages are discarded and therefore read back as zero; only the
    // unaligned edges need an explicit memset.
    void ZeroGuestMemory(uint32_t addr, size_t size);
    // Usable bytes in a block this allocator handed out, or 0 if the address did
    // not come from here. realloc needs it to know how much is safe to copy.
    uint32_t GuestAllocSize(uint32_t addr);

    // Dynarmic UserCallbacks interface
    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t vaddr) override;

    std::uint8_t MemoryRead8(std::uint32_t vaddr) override;
    std::uint16_t MemoryRead16(std::uint32_t vaddr) override;
    std::uint32_t MemoryRead32(std::uint32_t vaddr) override;
    std::uint64_t MemoryRead64(std::uint32_t vaddr) override;

    void MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) override;
    void MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) override;
    void MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) override;
    void MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) override;

    // Needed now that global_monitor is set: the base-class defaults return false, which makes
    // every guest STREX report failure and spin its retry loop forever.

    bool MemoryWriteExclusive8(std::uint32_t vaddr, std::uint8_t value, std::uint8_t expected) override;
    bool MemoryWriteExclusive16(std::uint32_t vaddr, std::uint16_t value, std::uint16_t expected) override;
    bool MemoryWriteExclusive32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t expected) override;
    bool MemoryWriteExclusive64(std::uint32_t vaddr, std::uint64_t value, std::uint64_t expected) override;

    void InterpreterFallback(std::uint32_t pc, std::size_t num_instructions) override;
    void CallSVC(std::uint32_t swi) override;
    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) override;

    void AddTicks(std::uint64_t ticks) override;
    std::uint64_t GetTicksRemaining() override;

    bool IsJitRunning() const;

private:
    // Clears every reason Run halted for and acts on each. Returns true if the Jit
    // should be re-entered. Shared by RunFunction and CallGuestNested so the two
    // cannot drift apart on which reasons they honour.
    bool HandleHalt(Dynarmic::A32::Jit& jit, Dynarmic::HaltReason hr);
    bool HeapSelfCheck();

    uint8_t* m_memory = nullptr;
    size_t m_memory_size = 0;
    std::unique_ptr<Dynarmic::ExclusiveMonitor> m_monitor;
    std::shared_ptr<GuestCp15> m_coprocessors[16];
    Dynarmic::A32::UserConfig m_jit_config;   // template every per-thread Jit is built from
    std::vector<std::unique_ptr<Dynarmic::A32::Jit>> m_jits;
    std::mutex m_jits_mutex;
    std::vector<std::pair<std::string, std::unique_ptr<ElfLoader>>> m_libs;
    std::unique_ptr<ElfLoader> m_elf_loader;   // alias of the last entry in m_libs
    std::unique_ptr<ThunkManager> m_thunk_manager;
    std::recursive_mutex m_mutex;


    Dynarmic::A32::Jit* MakeJit(uint32_t* out_index);
    std::vector<Dynarmic::A32::Jit*> OwnedJits();
    void ApplyPendingInvalidations(Dynarmic::A32::Jit& jit);
    // Ranges another thread asked this Jit to drop, applied by its owner.
    std::unordered_map<Dynarmic::A32::Jit*, std::vector<std::pair<uint32_t, uint32_t>>>
        m_pending_invalidations;
    std::atomic<uint32_t> m_next_stack_slot{0};

    uint32_t m_heap_current = 0x50000000;
    // (alignment, block size) -> free addresses
    std::unordered_map<uint64_t, std::vector<uint32_t>> m_free_blocks;
    std::uint64_t m_ticks_remaining = 0;

    mutable std::mutex m_probe_mutex;
    BootstrapProbeResult m_bootstrap_probe;
    bool m_bootstrap_probe_armed = false;
};
