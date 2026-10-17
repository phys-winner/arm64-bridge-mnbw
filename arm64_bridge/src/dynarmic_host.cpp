#include "dynarmic_host.h"
#include "elf_loader.h"
#include "guest_apk_slice.h"
#include "thunk_manager.h"
#include "bridge_config.h"
#include "guest_signals.h"
#include "guest_cp15.h"
#include "thunk_types.h"
#include "warband_skinning.h"
#include "warband_geometry.h"
#include "warband_animation.h"
#include "warband_render_commit.h"
#include "warband_render_buffers.h"
#include <unistd.h>
#include <cstring>
#include <set>
#include <map>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <android/log.h>
#include <sys/system_properties.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define LOG_TAG "CMR_ARM64_Loader"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define EXIT_TRAP_ARM   0x00000004
#define EXIT_TRAP_THUMB 0x00000008

// An SVC number no thunk will ever be given.
static constexpr uint32_t kNullCallSvc = 0x00FFFFFF;
// Linux ARM exposes __kuser_cmpxchg at this fixed ARM-mode address.
static constexpr uint32_t kKuserCmpxchgAddress = 0xFFFF0FC0;
// Nothing but the two exit traps lives below this; the trampolines start at 0x10000.
static constexpr uint32_t kTrapPageEnd = 0x40;

namespace {

// Guest thread stacks live between the thunk trampolines and the library load base
// at 0x40000000, which nothing else claims. Guest thread 0 keeps the original top.
constexpr uint32_t kThreadStackTop  = 0x40000000;
constexpr uint32_t kThreadStackSize = DynarmicHost::GUEST_STACK_SIZE;

// Per-host-thread guest execution state. There is only ever one DynarmicHost, so
// these can be plain thread_locals rather than a map keyed by host thread id.
struct GuestThreadState {
    Dynarmic::A32::Jit* jit = nullptr;     // this thread's outermost Jit
    Dynarmic::A32::Jit* active = nullptr;  // innermost one currently executing
    uint32_t stack_top = 0;
    bool running = false;
    // Jits kept for nested guest calls made from inside a thunk, one per depth.
    std::vector<Dynarmic::A32::Jit*> nested;
    int depth = 0;
};
thread_local GuestThreadState t_thread;
// Borrow the owning TLS object's address once. Its destructor/ABI stays intact;
// hot callbacks avoid repeating its dynamic-initialization guard lookups.
thread_local GuestThreadState* t_thread_current = nullptr;

size_t RequestedJitCodeCacheSize() {
    // Dynarmic's ARM64 backend clears the whole translated-code cache once less
    // than 1 MiB remains, but exposes no cache-usage/clear counter. Keep the
    // conservative release default and make 64/128 MiB available for a clean
    // cold-start/RSS A/B test without rebuilding the APK.
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("debug.warband.jit_cache_mb", value);
    if (strcmp(value, "64") == 0) return 64 * 1024 * 1024;
    if (strcmp(value, "128") == 0) return 128 * 1024 * 1024;
    return 32 * 1024 * 1024;
}

}  // namespace

DynarmicHost::DynarmicHost() = default;

DynarmicHost::~DynarmicHost() {
    if (m_memory) {
#if defined(_WIN32)
        VirtualFree(m_memory, 0, MEM_RELEASE);
#else
        munmap(m_memory, m_memory_size);
#endif
        m_memory = nullptr;
    }
}

// Reserve guest memory. MAP_NORESERVE keeps the 4 GiB reservation virtual: untouched pages
// cost nothing and only written pages are ever committed.
static uint8_t* MapGuestMemory(size_t size) {
#if defined(_WIN32)
    return reinterpret_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : reinterpret_cast<uint8_t*>(p);
#endif
}

bool DynarmicHost::Initialize() {
    // Mapping the whole 32-bit space lets the JIT emit a single `ldr Wt, [Xfastmem, Waddr, uxtw]`
    // per guest access instead of a call into MemoryRead*/MemoryWrite*.
    m_memory_size = GUEST_ADDRESS_SPACE;
    m_memory = MapGuestMemory(m_memory_size);

    bool fastmem = m_memory != nullptr && GetBridgeConfig().fastmem != 0;
    if (m_memory == nullptr) {
        LOGE("Could not reserve %zu bytes for the guest address space; "
             "falling back to %zu bytes without fastmem.", GUEST_ADDRESS_SPACE, GUEST_FALLBACK_SIZE);
        m_memory_size = GUEST_FALLBACK_SIZE;
        m_memory = MapGuestMemory(m_memory_size);
        if (!m_memory) {
            LOGE("Failed to allocate guest memory (%zu bytes)", m_memory_size);
            m_memory_size = 0;
            return false;
        }
    }

    // A call through a null function pointer lands on address zero and falls
    // through into the exit trap, which looks exactly like the function returning
    // normally - the thread just quietly ends. Claiming address zero turns that
    // into a log line naming the caller.
    *reinterpret_cast<uint32_t*>(m_memory + 0) = 0xEF000000 | kNullCallSvc;

    // ARM exit trap at 0x00000004: SVC 0 (0xEF000000)
    *reinterpret_cast<uint32_t*>(m_memory + EXIT_TRAP_ARM) = 0xEF000000;

    // Thumb exit trap at 0x00000008: SVC 0 (0xDF00, 0xDF00)
    *reinterpret_cast<uint32_t*>(m_memory + EXIT_TRAP_THUMB) = 0xDF00DF00;

    m_thunk_manager = std::make_unique<ThunkManager>(*this);
    m_thunk_manager->RegisterAllThunks();

    // ARM Linux's __kuser_cmpxchg is a fixed ARM-mode helper, not a relocatable
    // symbol. Route its ABI through the existing atomic STREX implementation:
    // r0=expected, r1=new, r2=guest word, r0=0 on success and 1 on failure.
    m_thunk_manager->RegisterThunk(
        "__kuser_cmpxchg", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            const uint32_t expected = static_cast<uint32_t>(jit.Regs()[0]);
            const uint32_t desired = static_cast<uint32_t>(jit.Regs()[1]);
            const uint32_t address = static_cast<uint32_t>(jit.Regs()[2]);
            jit.Regs()[0] = host.MemoryWriteExclusive32(address, desired, expected) ? 0 : 1;
        });
    if (static_cast<uint64_t>(kKuserCmpxchgAddress) + 8 > m_memory_size) {
        LOGE("guest address space is too small for __kuser_cmpxchg at 0x%08X",
             kKuserCmpxchgAddress);
        return false;
    }
    const uint32_t kuser_trampoline =
        m_thunk_manager->GetOrCreateThunkAddress("__kuser_cmpxchg");
    MemoryWrite32(kKuserCmpxchgAddress, MemoryRead32(kuser_trampoline));
    MemoryWrite32(kKuserCmpxchgAddress + 4, 0xE12FFF1E);  // bx lr
    InvalidateGuestCode(kKuserCmpxchgAddress, 8);
    LOGI("installed __kuser_cmpxchg at 0x%08X", kKuserCmpxchgAddress);

    if (!m_thunk_manager->IsValid()) {
        LOGE("bridge provider initialization failed");
        return false;
    }
    if (!HeapSelfCheck()) return false;

    // One monitor shared by every guest thread; processor_id indexes into it, so it
    // has to be at least as large as the number of Jits that will ever exist.
    m_monitor = std::make_unique<Dynarmic::ExclusiveMonitor>(kMaxJits);

    m_jit_config = {};
    m_jit_config.callbacks = this;
    m_jit_config.global_monitor = m_monitor.get();
    m_jit_config.arch_version = Dynarmic::A32::ArchVersion::v7;  // guest libs are ARMv7-A + NEON
    m_jit_config.enable_cycle_counting = false;                  // no timing model; skips AddTicks emission
    m_jit_config.fuse_svc_trampolines = true;                    // exact SVC/BX LR thunks use one JIT transition
    m_jit_config.cache_rsb_pointer = true;                      // keep the guest return-ring cursor in free X26
    m_jit_config.always_little_endian = true;                    // guest never uses SETEND; drops CPSR.E handling
    m_jit_config.define_unpredictable_behaviour = true;          // define rather than halt on odd encodings
    m_jit_config.optimizations = Dynarmic::all_safe_optimizations;
    m_jit_config.code_cache_size = RequestedJitCodeCacheSize();
    LOGI("Dynarmic code cache: %zu MiB/Jit (override: debug.warband.jit_cache_mb=64|128)",
         m_jit_config.code_cache_size / (1024 * 1024));
    // Both bionic and Mono's generated code read the thread pointer out of CP15.
    // With no coprocessor attached Dynarmic turns that into an assertion inside
    // its emitter, which aborts the process rather than raising anything the
    // guest could see.
    // Every slot gets one: an MCR to a coprocessor with no handler is an
    // assertion inside the emitter, and which coprocessor the guest reaches for
    // is not knowable up front. Only 15 does anything; the rest report
    // themselves once and behave as a register that reads zero.
    // 10 and 11 are VFP/NEON and the frontend decodes those itself - but only the
    // encodings it knows. Anything it does not recognise still arrives here, so
    // those slots cannot be left empty either.
    for (unsigned n = 0; n < 16; ++n) {
        m_coprocessors[n] = std::make_shared<GuestCp15>(*this, n);
        m_jit_config.coprocessors[n] = m_coprocessors[n];
    }
    if (fastmem) {
        m_jit_config.fastmem_pointer = reinterpret_cast<uintptr_t>(m_memory);
    }

    CurrentJit();  // the calling thread becomes guest thread 0
    return true;
}

// The eager sound-effect path costs seconds per call under the guest FMOD
// decoder, so the game's own createSound import lands in the host-decode thunk
// instead of the guest export. Only the game ELF is rerouted; guest libraries
// (FMOD calling itself) keep the real implementations.
bool IsInterceptedGameImport(const std::string& name) {
    return name == "_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE" ||
           name == "_ZN4FMOD5Sound7releaseEv";
}

bool DynarmicHost::LoadGame(const std::string& elf_path) {
    LOGI("Loading main game ELF from file: %s", elf_path.c_str());
    m_elf_loader = std::make_unique<ElfLoader>(m_memory, 0x40000000);
    if (!m_elf_loader->Load(elf_path)) {
        LOGE("Failed to load ELF: %s", elf_path.c_str());
        return false;
    }

    auto resolver = [this](const std::string& name) -> uint32_t {
        if (IsInterceptedGameImport(name)) {
            return m_thunk_manager->GetOrCreateThunkAddress(name);
        }
        for (auto it = m_libs.rbegin(); it != m_libs.rend(); ++it) {
            uint32_t addr = it->second->GetSymbolAddress(name);
            if (addr != 0) return addr;
        }
        if (m_elf_loader) {
            uint32_t addr = m_elf_loader->GetSymbolAddress(name);
            if (addr != 0) return addr;
        }
        return m_thunk_manager->GetOrCreateThunkAddress(name);
    };

    if (!m_elf_loader->Relocate(resolver)) {
        LOGE("Failed to relocate ELF.");
        return false;
    }

    LOGI("Running %zu static initializers in .init_array...", m_elf_loader->GetInitArray().size());
    for (uint32_t init_func : m_elf_loader->GetInitArray()) {
        RunFunction(init_func);
    }

    LOGI("Game ELF loaded and initialized successfully!");
    return true;
}

bool DynarmicHost::LoadGameFromMemory(const uint8_t* data, size_t size) {
    LOGI("Loading main game ELF from memory (%zu bytes)...", size);
    m_elf_loader = std::make_unique<ElfLoader>(m_memory, 0x40000000);
    if (!m_elf_loader->LoadFromMemory(data, size)) {
        LOGE("Failed to load ELF from memory buffer");
        return false;
    }

    auto resolver = [this](const std::string& name) -> uint32_t {
        if (IsInterceptedGameImport(name)) {
            return m_thunk_manager->GetOrCreateThunkAddress(name);
        }
        for (auto it = m_libs.rbegin(); it != m_libs.rend(); ++it) {
            uint32_t addr = it->second->GetSymbolAddress(name);
            if (addr != 0) return addr;
        }
        if (m_elf_loader) {
            uint32_t addr = m_elf_loader->GetSymbolAddress(name);
            if (addr != 0) return addr;
        }
        return m_thunk_manager->GetOrCreateThunkAddress(name);
    };

    if (!m_elf_loader->Relocate(resolver)) {
        LOGE("Failed to relocate ELF.");
        return false;
    }

    LOGI("Running %zu static initializers in .init_array...", m_elf_loader->GetInitArray().size());
    for (uint32_t init_func : m_elf_loader->GetInitArray()) {
        RunFunction(init_func);
    }

    LOGI("Game ELF loaded from memory and initialized successfully!");
    return true;
}

bool DynarmicHost::LoadGuestLibraries(AAssetManager* asset_manager,
                                     const std::vector<GuestLibrary>& libs) {
    const auto startup_started = std::chrono::steady_clock::now();
    const auto elapsed_ms = [](std::chrono::steady_clock::time_point started) {
        return static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count());
    };
    // Warband's main image spans about 40 MiB. Slots must stop before the guest heap.
    constexpr uint32_t kLibrarySlotSize = 0x04000000;
    constexpr uint32_t kLibraryLimit = 0x50000000;
    uint32_t base = 0x40000000;
    if (!asset_manager || !m_memory || !m_libs.empty() || m_elf_loader || libs.empty() ||
        m_memory_size < kLibraryLimit || libs.size() > (kLibraryLimit - base) / kLibrarySlotSize) {
        LOGE("Invalid/repeated guest APK load or insufficient library slots");
        return false;
    }
    std::set<std::string> guest_names;
    for (const auto& lib : libs) {
        if (lib.name.empty() || lib.asset_path.empty() || !guest_names.insert(lib.name).second) {
            LOGE("Empty/duplicate guest library entry");
            return false;
        }
    }

    for (const auto& lib : libs) {
        const auto library_started = std::chrono::steady_clock::now();
        GuestApkSlice slice;
        if (!slice.Open(asset_manager, lib.asset_path.c_str())) return false;
        auto loader = std::make_unique<ElfLoader>(m_memory, base, kLibrarySlotSize);
        const bool loaded = loader->LoadFromMemory(slice.Data(), slice.Size());
        const bool closed = slice.Close(base);
        if (!loaded || !closed) {
            LOGE("Failed to load/release %s APK slice %s", lib.name.c_str(), lib.asset_path.c_str());
            return false;
        }
        LOGI("Loaded %s at 0x%08x (%u bytes) in %llu ms", lib.name.c_str(), base,
             loader->GetImageSize(), elapsed_ms(library_started));
        m_libs.emplace_back(lib.name, std::move(loader));
        base += kLibrarySlotSize;
    }
    const auto images_finished = std::chrono::steady_clock::now();

    std::set<std::pair<std::string, std::string>> unresolved;
    for (auto& entry : m_libs) {
        for (const std::string& needed : entry.second->GetNeededLibraries()) {
            if (guest_names.count(needed) && !GetLibrary(needed)) {
                LOGE("%s requires missing guest %s", entry.first.c_str(), needed.c_str());
                return false;
            }
        }

        auto resolver = [this, &entry, &unresolved](const std::string& name) -> uint32_t {
            // The game itself loads through this path, and its eager sound
            // creation has to land in the host-decode thunk rather than the
            // guest FMOD export. Guest libraries keep real implementations.
            if (entry.first == "libMBExpMobile.so" && IsInterceptedGameImport(name)) {
                return m_thunk_manager->GetOrCreateThunkAddress(name);
            }
            for (const std::string& needed : entry.second->GetNeededLibraries()) {
                ElfLoader* dependency = GetLibrary(needed);
                uint32_t address = dependency ? dependency->GetSymbolAddress(name) : 0;
                if (address) return address;
            }
            if (!m_thunk_manager->IsRegistered(name)) unresolved.emplace(entry.first, name);
            return m_thunk_manager->GetOrCreateThunkAddress(name);
        };

        if (!entry.second->Relocate(resolver)) {
            LOGE("Failed to relocate %s", entry.first.c_str());
            return false;
        }
    }
    const auto relocations_finished = std::chrono::steady_clock::now();

    // Mono reaches most metadata through bsearch. The two comparators below are
    // tiny, but calling either as guest code creates a second Dynarmic Jit inside
    // the libc thunk. Two rare cold-start stalls were captured in exactly those
    // nested calls. Execute the pure metadata readers and comparators directly
    // against the guest arena: that removes the fragile nesting point and avoids
    // translating thousands of prologues during startup.
    auto install_fast_path_at = [this](uint32_t address, const char* label,
                                       const char* thunk_name, uint32_t expected0,
                                       uint32_t expected1, ThunkHandler handler) {
        if (!address) {
            LOGE("Mono fast path skipped: %s was not found", label);
            return false;
        }
        uint32_t original0 = MemoryRead32(address);
        uint32_t original1 = MemoryRead32(address + 4);
        if (original0 != expected0 || original1 != expected1) {
            LOGE("Mono fast path skipped for %s: unexpected prologue %08X %08X",
                 label, original0, original1);
            return false;
        }
        m_thunk_manager->RegisterThunk(thunk_name, std::move(handler));
        uint32_t trampoline = m_thunk_manager->GetOrCreateThunkAddress(thunk_name);
        MemoryWrite32(address, MemoryRead32(trampoline));  // SVC #thunk-id
        MemoryWrite32(address + 4, 0xE12FFF1E);             // bx lr
        InvalidateGuestCode(address, 8);
        LOGI("Mono fast path installed: %s at 0x%08X", label, address);
        return true;
    };
    auto install_fast_path = [this, &install_fast_path_at](
                                 const char* symbol, const char* thunk_name,
                                 uint32_t expected0, uint32_t expected1,
                                 ThunkHandler handler) {
        return install_fast_path_at(FindSymbol(symbol), symbol, thunk_name,
                                    expected0, expected1, std::move(handler));
    };

    auto read16 = [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t address = jit.Regs()[0];
        jit.Regs()[0] = static_cast<uint64_t>(address) + 2 <= host.GetGuestMemorySize()
                            ? host.MemoryRead16(address)
                            : 0;
    };
    auto read32 = [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t address = jit.Regs()[0];
        jit.Regs()[0] = static_cast<uint64_t>(address) + 4 <= host.GetGuestMemorySize()
                            ? host.MemoryRead32(address)
                            : 0;
    };
    auto decode_metadata = [](DynarmicHost& host, uint32_t table, uint32_t row,
                              uint32_t column, uint32_t& value) -> const char* {
        if (static_cast<uint64_t>(table) + 12 > host.GetGuestMemorySize()) {
            return "table is outside guest memory";
        }
        const uint32_t base = host.MemoryRead32(table);
        const uint32_t rows_and_size = host.MemoryRead32(table + 4);
        const uint32_t layout = host.MemoryRead32(table + 8);
        const uint32_t rows = rows_and_size & 0x00FFFFFFu;
        const uint32_t row_size = host.MemoryRead8(table + 7);
        const uint32_t columns = layout >> 24;
        if (row >= rows || column >= columns || column >= 12 || row_size == 0) {
            return "row or column is outside the metadata table";
        }

        uint64_t address = static_cast<uint64_t>(base) +
                           static_cast<uint64_t>(row) * row_size;
        for (uint32_t i = 0; i < column; ++i)
            address += ((layout >> (i * 2)) & 3u) + 1u;
        const uint32_t width = ((layout >> (column * 2)) & 3u) + 1u;
        if (address + width > host.GetGuestMemorySize()) {
            return "column data is outside guest memory";
        }
        switch (width) {
            case 1: value = host.MemoryRead8(static_cast<uint32_t>(address)); break;
            case 2: value = host.MemoryRead16(static_cast<uint32_t>(address)); break;
            case 4: value = host.MemoryRead32(static_cast<uint32_t>(address)); break;
            default: return "metadata column has unsupported width";
        }
        return nullptr;
    };
    auto report_metadata_error = [](const char* operation, uint32_t table,
                                    uint32_t row, uint32_t column, const char* why) {
        static std::atomic<uint32_t> reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 8) {
            LOGE("%s(%08X, %u, %u): %s", operation, table, row, column, why);
        }
    };
    auto decode_row_col = [decode_metadata, report_metadata_error](
                              Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t table = jit.Regs()[0];
        const uint32_t row = jit.Regs()[1];
        const uint32_t column = jit.Regs()[2];
        uint32_t value = 0;
        if (const char* why = decode_metadata(host, table, row, column, value)) {
            report_metadata_error("mono_metadata_decode_row_col", table, row, column, why);
        }
        jit.Regs()[0] = value;
    };

    auto metadata_row = [](DynarmicHost& host, uint32_t key, uint32_t elem,
                           uint32_t& table, uint32_t& row, uint32_t& column,
                           uint32_t& target) -> const char* {
        if (static_cast<uint64_t>(key) + 16 > host.GetGuestMemorySize())
            return "search key is outside guest memory";
        target = host.MemoryRead32(key);
        column = host.MemoryRead32(key + 4);
        table = host.MemoryRead32(key + 8);
        if (static_cast<uint64_t>(table) + 12 > host.GetGuestMemorySize())
            return "table is outside guest memory";
        const uint32_t base = host.MemoryRead32(table);
        const uint32_t row_size = host.MemoryRead8(table + 7);
        if (!row_size || elem < base) return "element is outside the metadata table";
        row = (elem - base) / row_size;
        return nullptr;
    };

    // Hidden comparator at libmono .text+0x1CA220. It finds the metadata row
    // whose start value is <= the key and whose next start value is > the key.
    auto compare_metadata_range = [decode_metadata, metadata_row, report_metadata_error](
                                      Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t key = jit.Regs()[0];
        uint32_t table = 0, row = 0, column = 0, target = 0, current = 0;
        const char* why = metadata_row(host, key, jit.Regs()[1], table, row, column, target);
        if (!why) why = decode_metadata(host, table, row, column, current);
        if (why) {
            report_metadata_error("mono_metadata_range_compare", table, row, column, why);
            jit.Regs()[0] = 0;
            return;
        }
        if (target < current) {
            jit.Regs()[0] = 0xFFFFFFFFu;
            return;
        }
        const uint32_t rows = host.MemoryRead32(table + 4) & 0x00FFFFFFu;
        if (row + 1 < rows) {
            uint32_t next = 0;
            if (const char* next_why = decode_metadata(host, table, row + 1, column, next)) {
                report_metadata_error("mono_metadata_range_compare", table, row + 1,
                                      column, next_why);
                jit.Regs()[0] = 0;
                return;
            }
            if (target >= next || current == next) {
                jit.Regs()[0] = 1;
                return;
            }
        }
        host.MemoryWrite32(key + 12, row);
        jit.Regs()[0] = 0;
    };

    // Hidden comparator at libmono .text+0x1CA35C. It is the exact-match form
    // used by custom-attribute metadata lookups.
    auto compare_metadata_exact = [decode_metadata, metadata_row, report_metadata_error](
                                      Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t key = jit.Regs()[0];
        uint32_t table = 0, row = 0, column = 0, target = 0, current = 0;
        const char* why = metadata_row(host, key, jit.Regs()[1], table, row, column, target);
        if (!why) why = decode_metadata(host, table, row, column, current);
        if (why) {
            report_metadata_error("mono_metadata_exact_compare", table, row, column, why);
            jit.Regs()[0] = 0;
            return;
        }
        if (target == current) {
            host.MemoryWrite32(key + 12, row);
            jit.Regs()[0] = 0;
        } else {
            jit.Regs()[0] = target < current ? 0xFFFFFFFFu : 1u;
        }
    };

    const uint32_t metadata_decoder = FindSymbol("mono_metadata_decode_row_col");
    install_fast_path("mono_read16", "cmr_fast_mono_read16",
                      0xE52DB004, 0xE28DB000, read16);
    install_fast_path("mono_read32", "cmr_fast_mono_read32",
                      0xE52DB004, 0xE28DB000, read32);
    install_fast_path("mono_metadata_decode_row_col", "cmr_fast_mono_decode_row_col",
                      0xE92D4830, 0xE28DB00C, decode_row_col);
    if (metadata_decoder) {
        // Relative positions and prologues are checked against this exact libmono
        // before anything is overwritten; a different build safely keeps guest code.
        install_fast_path_at(metadata_decoder + 0x5424, "mono_metadata_range_compare",
                             "cmr_fast_mono_metadata_range_compare",
                             0xE92D4800, 0xE28DB004, compare_metadata_range);
        install_fast_path_at(metadata_decoder + 0x5560, "mono_metadata_exact_compare",
                             "cmr_fast_mono_metadata_exact_compare",
                             0xE92D4800, 0xE28DB004, compare_metadata_exact);
    }

    // Replace only a byte-verified, allocation-free fragment. The branch keeps
    // the original guest preparation and epilogue, including its dirty flags.
    auto fragment_hash = [this](uint32_t address, size_t bytes) {
        uint32_t hash = 2166136261u;
        for (size_t p = 0; p < bytes; ++p) hash = (hash ^ m_memory[address + p]) * 16777619u;
        return hash;
    };
    auto install_fragment = [this, &fragment_hash](uint32_t address, uint32_t tail, uint32_t expected_hash,
                                    const char* name, ThunkHandler handler, uint32_t patch_bytes = 8) {
        if (!address || uint64_t(address) + patch_bytes >= tail || tail > m_memory_size ||
            (patch_bytes != 4 && patch_bytes != 8)) return false;
        if (fragment_hash(address, tail - address) != expected_hash) {
            LOGE("Warband fast path skipped: %s has an unexpected fragment", name);
            return false;
        }
        m_thunk_manager->RegisterThunk(name, std::move(handler));
        const uint32_t trampoline = m_thunk_manager->GetOrCreateThunkAddress(name);
        MemoryWrite32(address, MemoryRead32(trampoline));
        if (patch_bytes == 8)
            MemoryWrite32(address + 4, 0xEA000000u | ((tail - address - 12) / 4));
        InvalidateGuestCode(address, patch_bytes);
        LOGI("Warband fast path installed: %s at 0x%08X", name, address);
        return true;
    };
    const uint32_t deform = FindSymbol("_ZN11rglSkeleton15deform_mesh_ImpEPS_P7rglMesh");
    if (deform) {
        const uint32_t hook = deform + 0x328;
        install_fragment(hook, deform + 0x584, 0x85C6BA4Bu, "warband_skinning_batch",
            [hook](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
                auto& r = jit.Regs();
                uint32_t fpscr;
                if (warband_skinning::Run(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                        r[9], host.MemoryRead32(r[13] + 0x0C), jit.Fpscr(), fpscr)) {
                    jit.SetFpscr(fpscr);
                    // Both the empty path and the last stock loop comparison are equal.
                    jit.SetCpsr((jit.Cpsr() & ~0xF0000000u) | 0x60000000u);
                } else {
                    // Emulate the two displaced LDRs, then resume the original loop.
                    r[0] = host.MemoryRead32(r[9] + 0x154);
                    r[1] = host.MemoryRead32(r[9] + 0x158);
                    r[15] = hook + 8;
                }
            });
    }

    const uint32_t render_mesh = FindSymbol("_ZN7rglMesh19render_mesh_to_meshEPKS_RK15rglMatrix_frame");
    if (render_mesh) {
        const uint32_t hook = render_mesh + 0x104;
        install_fragment(hook, render_mesh + 0x5C4, 0xFA92FF0Cu, "warband_geometry_batch",
            [hook](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
                auto& r = jit.Regs();
                uint32_t fpscr;
                if (warband_geometry::Run(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                        r[4], r[7], r[5], r[8], r[6], jit.Fpscr(), fpscr)) {
                    host.MemoryWrite32(r[13] + 0x14, r[4]);
                    jit.SetFpscr(fpscr);
                    jit.SetCpsr((jit.Cpsr() & ~0xF0000000u) | 0x60000000u);
                } else {
                    r[0] = host.MemoryRead32(r[7] + 0x16C);
                    r[11] = (r[11] & 0xFFFF0000u) | 0xAAABu;
                    r[15] = hook + 8;
                }
            });
    }

    const uint32_t setup_frames = FindSymbol("_ZN11rglSkeleton20setup_frames_dynamicEv");
    if (setup_frames >= 0x2086E0u) {
        const uint32_t base = setup_frames - 0x2086E0u;
        const uint64_t sqrt_slot = uint64_t(base) + warband_animation::kSqrtGotOffset;
        bool code_matches = sqrt_slot + 4 <= m_memory_size &&
            m_thunk_manager->IsRegistered("sqrtf") &&
            MemoryRead32(static_cast<uint32_t>(sqrt_slot)) ==
                m_thunk_manager->GetOrCreateThunkAddress("sqrtf");
        for (const auto& guard : warband_animation::kGuards) {
            const uint64_t address = uint64_t(base) + guard.offset;
            if (address + guard.bytes > m_memory_size ||
                fragment_hash(static_cast<uint32_t>(address), guard.bytes) != guard.hash) {
                code_matches = false;
                break;
            }
        }
        if (code_matches) {
            // Hook the one-time branch, keeping the repeated bone entry intact.
            const uint32_t hook = setup_frames + 0xE0, tail = setup_frames + 0xE94;
            install_fragment(hook, tail, warband_animation::kLoopHash,
                "warband_animation_batch", [hook, tail, base](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
                    auto& r = jit.Regs();
                    uint32_t fpscr, nzcv;
                    if (warband_animation::Run(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                            base, r, jit.ExtRegs(), jit.Fpscr(), fpscr, nzcv)) {
                        jit.SetFpscr(fpscr);
                        jit.SetCpsr((jit.Cpsr() & ~0xF0000000u) | nzcv);
                        r[15] = tail;
                    } else {
                        // The displaced B only selects this original loop target.
                        r[15] = hook + 8;
                    }
                }, 4);
        } else {
            LOGE("Warband animation fast path skipped: unexpected code or sqrtf binding");
        }
    }

    const uint32_t commit_changes = FindSymbol("_ZN16rglOpenGL_effect13CommitChangesEv");
    if (commit_changes >= 0x1E35A8u) {
        const uint32_t base = commit_changes - 0x1E35A8u;
        std::array<uint32_t, 5> thunks{};
        std::array<const ThunkHandler*, 5> handlers{};
        bool matches = true;
        for (const auto& guard : warband_render::kCommitGuards) {
            const uint64_t address = uint64_t(base) + guard.offset;
            if (address + guard.bytes > m_memory_size ||
                fragment_hash(static_cast<uint32_t>(address), guard.bytes) != guard.hash) {
                matches = false;
                break;
            }
        }
        for (size_t i = 0; matches && i < thunks.size(); ++i) {
            const auto& binding = warband_render::kBindings[i];
            if (!m_thunk_manager->IsRegistered(binding.name)) {
                matches = false;
                break;
            }
            thunks[i] = m_thunk_manager->GetOrCreateThunkAddress(binding.name);
            const uint64_t slot = uint64_t(base) + binding.got;
            const uint32_t svc = MemoryRead32(thunks[i]);
            matches = slot + 4 <= m_memory_size &&
                MemoryRead32(static_cast<uint32_t>(slot)) == thunks[i] &&
                (svc & 0xFF000000u) == 0xEF000000u && (svc & 0x00FFFFFFu) &&
                MemoryRead32(thunks[i] + 4) == 0xE12FFF1Eu;
            if (matches) {
                handlers[i] = m_thunk_manager->ResolveThunkHandler(svc & 0x00FFFFFFu);
                matches = handlers[i] != nullptr;
            }
        }
        if (matches) {
            const uint32_t hook = base + warband_render::kCommitHook;
            install_fragment(hook, base + warband_render::kCommitTail,
                warband_render::kCommitFragmentHash, "warband_render_commit_batch",
                [base, hook, thunks, handlers](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
                    auto& r = jit.Regs();
                    warband_render::Commit prepared{};
                    if (!warband_render::PrepareCommit(host.GetGuestMemoryBase(),
                            host.GetGuestMemorySize(), base, r[4], thunks, prepared)) {
                        // Execute the displaced LDR, then enter the original body.
                        r[2] = host.MemoryRead32(r[4] + 0x50);
                        r[15] = hook + 4;
                        return;
                    }
                    uint32_t nzcv = jit.Cpsr() & 0xF0000000u;
                    warband_render::CommitChanges(host.GetGuestMemoryBase(), base, prepared,
                        thunks, r, nzcv, [&](warband_render::Uniform kind) {
                            // These pinned void GL handlers only read their arguments.
                            // Publish flags if a signal observes this original SVC seam.
                            // The outer SVC restores guest FP state; this body performs
                            // integer/raw-bit transfers and never consumes host FPSR.
                            (*handlers[static_cast<size_t>(kind)])(jit, host);
                            if (!GuestSignalPending()) return true;
                            jit.SetCpsr((jit.Cpsr() & ~0xF0000000u) | nzcv);
                            GuestSignalDeliverPending(host);
                            nzcv = jit.Cpsr() & 0xF0000000u;
                            return false;
                        });
                    jit.SetCpsr((jit.Cpsr() & ~0xF0000000u) | nzcv);
                }, 4);
        }
    }

    const uint32_t set_vb = FindSymbol("_ZN7rglMesh15set_vb_callbackEPhRKm");
    if (set_vb >= 0x1F160Cu) {
        const uint32_t base = set_vb - 0x1F160Cu;
        bool matches = true;
        for (const auto& guard : warband_render_buffers::kGuards) {
            const uint64_t address = uint64_t(base) + guard.offset;
            if (address + guard.bytes > m_memory_size ||
                fragment_hash(static_cast<uint32_t>(address), guard.bytes) != guard.hash) {
                matches = false;
                break;
            }
        }
        if (matches) {
            const uint32_t hook = base + warband_render_buffers::kHook;
            const uint32_t tail = base + warband_render_buffers::kTail;
            install_fragment(hook, tail, warband_render_buffers::kFragmentHash,
                "warband_vertex_pack_batch", [base, hook, tail](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
                    auto& r = jit.Regs();
                    uint32_t nzcv = jit.Cpsr() & 0xF0000000u;
                    if (warband_render_buffers::Run(host.GetGuestMemoryBase(),
                            host.GetGuestMemorySize(), base, r, jit.ExtRegs(), jit.Fpscr(), nzcv)) {
                        jit.SetCpsr((jit.Cpsr() & ~0xF0000000u) | nzcv);
                        r[15] = tail;
                    } else {
                        // Execute the displaced LDR before entering the original body.
                        r[7] = host.MemoryRead32(r[11] + 0x154);
                        r[15] = hook + 4;
                    }
                }, 4);
        }
    }

    LOGI("%zu imports use diagnostic traps:", unresolved.size());
    for (const auto& [guest, name] : unresolved)
        LOGI("  unresolved %s: %s", guest.c_str(), name.c_str());

    for (auto& entry : m_libs) {
        const uint32_t init_function = entry.second->GetInitFunction();
        const auto& init_array = entry.second->GetInitArray();
        LOGI("Running DT_INIT=%s plus %zu init-array entries for %s...",
             init_function ? "yes" : "no", init_array.size(), entry.first.c_str());
        if (init_function) RunFunction(init_function);
        for (uint32_t fn : init_array) RunFunction(fn);
    }

    const auto initialized = std::chrono::steady_clock::now();
    const auto phase_ms = [](std::chrono::steady_clock::time_point begin,
                             std::chrono::steady_clock::time_point end) {
        return static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
    };
    LOGI("All %zu guest libraries ready: total_ms=%llu image_ms=%llu relocation_ms=%llu "
         "init_array_ms=%llu", m_libs.size(), elapsed_ms(startup_started),
         phase_ms(startup_started, images_finished),
         phase_ms(images_finished, relocations_finished),
         phase_ms(relocations_finished, initialized));
    return true;
}

ElfLoader* DynarmicHost::GetLibrary(const std::string& name) {
    for (auto& entry : m_libs) {
        if (entry.first == name) return entry.second.get();
    }
    return nullptr;
}

uint32_t DynarmicHost::FindSymbol(const std::string& name) {
    for (auto it = m_libs.rbegin(); it != m_libs.rend(); ++it) {
        uint32_t addr = it->second->GetSymbolAddress(name);
        if (addr) return addr;
    }
    if (m_elf_loader) return m_elf_loader->GetSymbolAddress(name);
    return 0;
}

bool DynarmicHost::IsJitRunning() const { return t_thread.running; }

Dynarmic::A32::Jit* DynarmicHost::MakeJit(uint32_t* out_index) {
    std::lock_guard<std::mutex> lock(m_jits_mutex);
    const uint32_t index = static_cast<uint32_t>(m_jits.size());
    if (index >= kMaxJits) {
        LOGE("more than %zu Jits; ldrex/strex would index past the exclusive monitor",
             kMaxJits);
    }
    Dynarmic::A32::UserConfig config = m_jit_config;
    config.processor_id = index < kMaxJits ? index : static_cast<uint32_t>(kMaxJits - 1);
    if (index == 0 || (index % 16) == 0) LOGI("guest Jit #%u created", index);
    m_jits.push_back(std::make_unique<Dynarmic::A32::Jit>(config));
    if (out_index) *out_index = index;
    return m_jits.back().get();
}

Dynarmic::A32::Jit& DynarmicHost::CurrentJit() {
    // During a nested call the innermost Jit is the one whose registers a thunk
    // must read and write. Returning the thread's outer Jit here would make the
    // guest comparator's arguments come from the frame that is suspended.
    auto* state = t_thread_current;
    if (!state) t_thread_current = state = &t_thread;
    if (state->active) return *state->active;
    if (state->jit) return *state->jit;

    // Stack slots are counted separately from Jits: nested-call Jits share the
    // stack of the thread that owns them, and numbering by m_jits.size() would
    // hand out a slot to each of them and shift every later thread's stack.
    //
    // ponytail: slots are never recycled, so a thread that exits keeps its 1 MiB
    // and its Jit for the life of the process. A 3.5 minute run reaches slot 35 of
    // kMaxGuestThreads; past 64 the region below kThreadStackTop is only free
    // because nothing else is mapped there, not because anything checks. Recycle
    // on thread exit if a longer session ever walks past the cap.
    const uint32_t slot = m_next_stack_slot++;
    t_thread.jit = MakeJit(nullptr);
    t_thread.active = t_thread.jit;
    t_thread.stack_top = kThreadStackTop - slot * kThreadStackSize;
    // Boehm addresses threads by the value gettid() gave it, so the signal table
    // is keyed the same way.
    GuestSignalRegisterThread(gettid(), t_thread.jit);
    LOGI("guest thread %u attached: stack top 0x%08X", slot, t_thread.stack_top);
    return *t_thread.jit;
}

// Runs guest code from inside a thunk, i.e. from inside Jit::Run.
//
// Jit::Run is not re-entrant, but that is a property of the Jit object, not of
// the thread - so the nested call gets a Jit of its own. Without this, any libc
// function that takes a guest callback has to be stubbed out, and a stubbed
// bsearch is not "slower", it answers "not found" to every metadata lookup Mono
// makes.
//
// The nested stack is carved below the suspended frame's own stack pointer,
// which is memory the interrupted function has already finished with.
// Clears every halt reason Run came back for and does what each one asks. Returns
// true if the Jit should be re-entered.
//
// One reason per return trip is the wrong shape: Run reports whatever accumulated
// while it was inside, and handling only the first of them leaves the rest set. The
// next Run then returns for those instead of executing anything - and if the first
// reason keeps arriving, the rest never get their turn at all.
bool DynarmicHost::HandleHalt(Dynarmic::A32::Jit& jit, Dynarmic::HaltReason hr) {
    bool resume = false;
    if (Dynarmic::Has(hr, Dynarmic::HaltReason::CacheInvalidation)) {
        jit.ClearHalt(Dynarmic::HaltReason::CacheInvalidation);
        resume = true;
    }
    // A signal can land while a nested call is running - the restart signal arrives
    // exactly like this, while the suspend handler is still on the stack. Delivering
    // it costs one more nesting level, which the depth cap bounds.
    if (Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined2)) {
        jit.ClearHalt(Dynarmic::HaltReason::UserDefined2);
        resume = true;
    }
    // Unconditional, not tied to the flag: the flag can be cleared by whichever Run
    // loop on this thread saw it first, while the queue entry it was announcing is
    // still there.
    if (GuestSignalPending()) GuestSignalDeliverPending(*this);
    return resume;
}

uint32_t DynarmicHost::CallGuestNested(uint32_t target_pc, const std::vector<uint32_t>& args) {
    constexpr int kMaxNestedDepth = 8;

    if (!target_pc) return 0;
    if (t_thread.depth >= kMaxNestedDepth) {
        LOGE("nested guest call to 0x%08X refused at depth %d", target_pc, t_thread.depth);
        return 0;
    }

    Dynarmic::A32::Jit* outer = t_thread.active ? t_thread.active : &CurrentJit();
    uint32_t outer_sp = outer->Regs()[13];

    const int depth = t_thread.depth;
    while (static_cast<int>(t_thread.nested.size()) <= depth) {
        t_thread.nested.push_back(MakeJit(nullptr));
        GuestSignalAddThreadJit(t_thread.nested.back());
    }
    Dynarmic::A32::Jit* jit = t_thread.nested[depth];

    // Just below the suspended frame: bytes under its stack pointer are dead, and
    // each deeper nested call starts below the one before it, so the frames never
    // overlap without reserving a slice per depth. Keeping the gap small matters
    // for signal handlers - Boehm records the handler's stack pointer as the top
    // of the region it scans conservatively.
    uint32_t sp = (outer_sp - 256) & ~7u;
    for (size_t i = 0; i < args.size() && i < 4; ++i) jit->Regs()[i] = args[i];
    // Arguments past the fourth go on the stack, same as RunFunction. Dropping them
    // is how nativeResize(env, self, w, h, fw, fh) used to arrive without its last
    // two words.
    if (args.size() > 4) {
        size_t extra_args = args.size() - 4;
        sp = (sp - static_cast<uint32_t>(extra_args * 4)) & ~7u;
        for (size_t i = 0; i < extra_args; ++i) {
            *reinterpret_cast<uint32_t*>(m_memory + sp + i * 4) = args[4 + i];
        }
    }
    jit->Regs()[13] = sp;

    bool is_thumb = (target_pc & 1) != 0;
    jit->Regs()[14] = is_thumb ? (EXIT_TRAP_THUMB | 1) : EXIT_TRAP_ARM;
    jit->Regs()[15] = target_pc & ~1u;
    jit->SetCpsr((jit->Cpsr() & ~0x20u) | (is_thumb ? 0x20u : 0u));

    t_thread.active = jit;
    ++t_thread.depth;
    jit->ClearHalt();  // same drop window as in RunFunction
    if (GuestSignalPending()) GuestSignalDeliverPending(*this);
    for (;;) {
        ApplyPendingInvalidations(*jit);
        Dynarmic::HaltReason hr = jit->Run();
        // Every reason Run came back for, not just the first one. Restarting after
        // the first match starved the others: Mono flushes the instruction cache
        // after every method it compiles, so CacheInvalidation arrives continuously
        // during start-up, and a thread whose next Run always came back for that
        // never got as far as its pending signal. It made no progress either - each
        // Run returned immediately - so it looked like guest code stuck in a loop.
        bool resume = HandleHalt(*jit, hr);
        if (!resume) break;
    }
    --t_thread.depth;
    t_thread.active = outer;

    return jit->Regs()[0];
}

uint32_t DynarmicHost::GuestStackTop() {
    CurrentJit();  // makes sure this thread has a stack assigned
    return t_thread.stack_top;
}

uint32_t DynarmicHost::GuestStackLimit() {
    CurrentJit();
    return t_thread.stack_top - kThreadStackSize;
}

// Deep enough for Dynarmic's IR passes on the largest block Unity has produced here;
// the default 1 MiB was not, and the overflow lands in the optimiser rather than
// anywhere that names the guest.
static constexpr size_t kHostThreadStack = 8u * 1024 * 1024;

void SpawnHostThread(std::function<void()> body) {
    auto* payload = new std::function<void()>(std::move(body));
    auto entry = [](void* p) -> void* {
        std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()>*>(p));
        (*fn)();
        return nullptr;
    };

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kHostThreadStack);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t tid{};
    int rc = pthread_create(&tid, &attr, entry, payload);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        LOGE("pthread_create for a guest thread failed (%d); falling back to the default stack", rc);
        std::thread([payload, entry] { entry(payload); }).detach();
    }
}

void DynarmicHost::SpawnGuestThread(uint32_t start_func, uint32_t arg) {
    SpawnHostThread([this, start_func, arg] {
        RunFunction(start_func, {arg});
    });
}

void DynarmicHost::ArmBootstrapProbe(uint32_t entry_pc) {
    std::lock_guard<std::mutex> lock(m_probe_mutex);
    m_bootstrap_probe = {};
    m_bootstrap_probe.requested_pc = entry_pc;
    m_bootstrap_probe_armed = true;
}

DynarmicHost::BootstrapProbeResult DynarmicHost::GetBootstrapProbeResult() const {
    std::lock_guard<std::mutex> lock(m_probe_mutex);
    return m_bootstrap_probe;
}

uint32_t DynarmicHost::RunFunction(uint32_t target_pc, const std::vector<uint32_t>& args) {
    if (t_thread.running) {
        // Jit::Run is not re-entrant on the same Jit (a32_interface.cpp asserts on it),
        // but that is a property of the Jit, not of the thread - so run it on a nested
        // one, which is what CallGuestNested exists for.
        //
        // This used to push the call onto a deferred queue instead, and nothing ever
        // drained that queue. Java calls nativeResize from inside a thread that is
        // already running guest code, so the engine was never told the surface size:
        // it kept the 128x128 default and drew every frame into the bottom-left corner
        // of the screen.
        return CallGuestNested(target_pc, args);
    }

    Dynarmic::A32::Jit& jit = CurrentJit();

    t_thread.running = true;
    struct JitRunningGuard {
        ~JitRunningGuard() { t_thread.running = false; }
    } running_guard;

    for (size_t i = 0; i < args.size() && i < 4; ++i) {
        jit.Regs()[i] = args[i];
    }

    // AAPCS wants SP 8-byte aligned at a public interface; an odd number of stacked
    // arguments would otherwise leave a doubleword argument straddling that boundary.
    uint32_t sp = t_thread.stack_top;
    if (args.size() > 4) {
        size_t extra_args = args.size() - 4;
        sp = (sp - static_cast<uint32_t>(extra_args * 4)) & ~7u;
        for (size_t i = 0; i < extra_args; ++i) {
            *reinterpret_cast<uint32_t*>(m_memory + sp + i * 4) = args[4 + i];
        }
    }
    jit.Regs()[13] = sp;

    // Return address -> exit trap (with Thumb bit if target is Thumb)
    bool is_thumb = (target_pc & 1) != 0;
    jit.Regs()[14] = is_thumb ? (EXIT_TRAP_THUMB | 1) : EXIT_TRAP_ARM;

    // Set PC
    jit.Regs()[15] = target_pc & ~1u;
    jit.SetCpsr((jit.Cpsr() & ~0x20u) | (is_thumb ? 0x20u : 0u));

    jit.ClearHalt();
    // ClearHalt wipes a UserDefined2 that GuestSignalSend set moments ago, so a signal
    // aimed at a thread that was still being set up was dropped: the deque kept the
    // entry and nothing ever looked at it again. The thread is visible in the signal
    // table from CurrentJit() above, which is before this point. Boehm's stop-the-world
    // then waited forever for an acknowledgement from a thread it had already reached.
    if (GuestSignalPending()) GuestSignalDeliverPending(*this);
    Dynarmic::HaltReason hr{};
    for (;;) {
        ApplyPendingInvalidations(jit);
            hr = jit.Run();
        // InvalidateCacheRange() called from inside Run - a thunk minting a new
        // trampoline (dlsym), or the guest's own cacheflush after Mono emits code -
        // stops execution instead of resuming it. The stale block is already gone by
        // the time Run returns, so re-entering continues from the same PC. Signals
        // and PC dumps come back the same way; HandleHalt takes all of them at once.
        if (!HandleHalt(jit, hr)) break;
    }
    uint32_t final_pc = jit.Regs()[15];
    {
        std::lock_guard<std::mutex> lock(m_probe_mutex);
        if (m_bootstrap_probe_armed &&
            (m_bootstrap_probe.requested_pc & ~1u) == (target_pc & ~1u)) {
            m_bootstrap_probe.final_pc = final_pc;
            m_bootstrap_probe.halt_reason = static_cast<uint32_t>(hr);
            m_bootstrap_probe.pc_advanced =
                m_bootstrap_probe.pc_advanced ||
                (m_bootstrap_probe.code_fetch_observed && final_pc != (target_pc & ~1u));
            m_bootstrap_probe_armed = false;
            LOGI("bootstrap probe requested=0x%08X fetched=0x%08X final=0x%08X "
                 "halt=0x%X advanced=%s",
                 m_bootstrap_probe.requested_pc, m_bootstrap_probe.fetched_pc,
                 m_bootstrap_probe.final_pc, m_bootstrap_probe.halt_reason,
                 m_bootstrap_probe.pc_advanced ? "yes" : "no");
        }
    }
    if (final_pc > kTrapPageEnd) {
        LOGI("RunFunction(0x%08X) unexpected halt with reason=0x%X, PC=0x%08X, LR=0x%08X, R0=0x%08X, SP=0x%08X",
             target_pc, static_cast<unsigned>(hr), final_pc, jit.Regs()[14], jit.Regs()[0], jit.Regs()[13]);
    }

    return jit.Regs()[0];
}

void DynarmicHost::InvalidateGuestCode(uint32_t addr, size_t size) {
    // Every thread holds its own translation of the range, so all of them have to
    // drop it - otherwise a thread that already ran the old code keeps running it.
    //
    // But InvalidateCacheRange rewrites the Jit's own block tables, and doing that
    // to a Jit another thread is currently executing corrupts them. Mono flushes
    // the instruction cache after every method it compiles, so once the managed
    // code got going this was happening constantly, and the process died in
    // Dynarmic's own emitter - a different place each run, which is what a
    // corrupted structure looks like.
    //
    // So each Jit is invalidated by the thread that owns it. Other threads are
    // handed the range and stopped; HaltExecution is an atomic flag and is safe
    // to set from here. They apply it before they next execute anything.
    std::vector<Dynarmic::A32::Jit*> mine = OwnedJits();
    for (auto* jit : mine)
        jit->InvalidateCacheRange(addr, size);

    std::lock_guard<std::mutex> lock(m_jits_mutex);
    for (auto& jit : m_jits) {
        Dynarmic::A32::Jit* raw = jit.get();
        if (std::find(mine.begin(), mine.end(), raw) != mine.end()) continue;
        m_pending_invalidations[raw].emplace_back(addr, static_cast<uint32_t>(size));
        raw->HaltExecution(Dynarmic::HaltReason::CacheInvalidation);
    }
}

// The Jits this thread owns: its own, plus any it is using for nested calls.
std::vector<Dynarmic::A32::Jit*> DynarmicHost::OwnedJits() {
    std::vector<Dynarmic::A32::Jit*> out;
    if (t_thread.jit) out.push_back(t_thread.jit);
    for (auto* jit : t_thread.nested) out.push_back(jit);
    return out;
}

// Applied by the owning thread, immediately before it enters Run.
void DynarmicHost::ApplyPendingInvalidations(Dynarmic::A32::Jit& jit) {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    {
        std::lock_guard<std::mutex> lock(m_jits_mutex);
        auto it = m_pending_invalidations.find(&jit);
        if (it == m_pending_invalidations.end() || it->second.empty()) return;
        ranges.swap(it->second);
    }
    for (auto& [addr, size] : ranges)
        jit.InvalidateCacheRange(addr, size);
}


// The guest heap runs from 0x50000000 up to the guest stacks at 0x7E000000.
// A bump allocator was enough to reach Mono's start-up and no further: Mono
// allocates and frees continuously once it is running, so with free() a no-op
// the 736 MiB arena drains in seconds and every allocation after that returns
// null.
//
// Blocks carry an eight-byte header - their own size and a magic word - and go
// back onto a free list bucketed by that size when released. Sizes are rounded
// so buckets actually get hits: powers of two up to 64 KiB, multiples of 64 KiB
// above it, which keeps the waste on Unity's large buffers bounded at 64 KiB
// instead of doubling them.
//
// The magic word is what makes free() safe. The guest also frees pointers that
// never came from here - static buffers, addresses inside a loaded image - and
// reading a "size" out of those would poison the free list.
namespace {
constexpr uint32_t kHeapBase = 0x50000000;
constexpr uint32_t kHeapLimit = 0xF0000000; // 2.5 GiB heap headroom in 4 GiB arena
constexpr uint32_t kBlockMagic = 0x4246;  // 'BF', in the top half of the tag word
constexpr uint32_t kHeaderSize = 8;

// The tag word carries the magic and how strictly the block was aligned, so
// free() can put it back on the list it came from without the caller saying.
uint32_t MakeTag(size_t alignment) {
    uint32_t log2 = 3;
    while ((static_cast<size_t>(1) << log2) < alignment) ++log2;
    return (kBlockMagic << 16) | log2;
}
uint64_t BlockKey(uint32_t block, uint32_t tag) {
    return (static_cast<uint64_t>(tag & 0xFFFFu) << 32) | block;
}

uint32_t RoundBlockSize(size_t size) {
    uint32_t n = static_cast<uint32_t>(size < 16 ? 16 : size);
    if (n <= 64 * 1024) {
        uint32_t p = 16;
        while (p < n) p <<= 1;
        return p;
    }
    return (n + 0xFFFFu) & ~0xFFFFu;
}
}  // namespace

void DynarmicHost::ZeroGuestMemory(uint32_t addr, size_t size) {
    if (!m_memory || !size || static_cast<size_t>(addr) >= m_memory_size) return;

    const size_t bounded = std::min(size, m_memory_size - static_cast<size_t>(addr));
#if defined(_WIN32)
    memset(m_memory + addr, 0, bounded);
#else
    // MADV_DONTNEED on this private anonymous arena has exactly the zero-fill
    // semantics the 32-bit guest expects from a new mmap, but it leaves the pages
    // uncommitted until they are actually used.  Touching a multi-gigabyte
    // MAP_FIXED range with memset instead made Android account every page in RSS
    // and Xiaomi's lmkd killed the foreground game during cold start.
    static const size_t page_size = [] {
        long value = sysconf(_SC_PAGESIZE);
        return value > 0 ? static_cast<size_t>(value) : static_cast<size_t>(4096);
    }();
    const uint64_t begin = addr;
    const uint64_t end = begin + bounded;
    const uint64_t page_begin = ((begin + page_size - 1) / page_size) * page_size;
    const uint64_t page_end = (end / page_size) * page_size;

    if (page_begin >= page_end) {
        memset(m_memory + begin, 0, bounded);
        return;
    }

    if (page_begin > begin)
        memset(m_memory + begin, 0, static_cast<size_t>(page_begin - begin));
    const size_t whole_pages = static_cast<size_t>(page_end - page_begin);
    if (madvise(m_memory + page_begin, whole_pages, MADV_DONTNEED) != 0)
        memset(m_memory + page_begin, 0, whole_pages);
    if (end > page_end)
        memset(m_memory + page_end, 0, static_cast<size_t>(end - page_end));
#endif
}

uint32_t DynarmicHost::GuestAlloc(size_t size, size_t alignment) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (alignment < 8) alignment = 8;
    if (size > static_cast<size_t>(kHeapLimit - kHeapBase - kHeaderSize)) {
        LOGE("GuestAlloc request is larger than the guest heap: %zu bytes", size);
        return 0;
    }
    uint32_t block = RoundBlockSize(size);

    // A recycled block keeps the alignment it was first handed out with, so the
    // free lists are keyed by both. Ignoring alignment here would either hand a
    // 4 KiB-aligned caller an 8-aligned block, or - as it did - refuse to recycle
    // page-aligned blocks at all, which is most of what Mono's JIT asks for.
    uint32_t tag = MakeTag(alignment);
    uint64_t key = BlockKey(block, tag);
    {
        auto it = m_free_blocks.find(key);
        if (it != m_free_blocks.end() && !it->second.empty()) {
            uint32_t addr = it->second.back();
            it->second.pop_back();
            // Fresh arena pages read as zero, so recycled ones have to as well:
            // callers that fill a structure only partly would otherwise see the
            // previous tenant's bytes.
            ZeroGuestMemory(addr, block);
            *reinterpret_cast<uint32_t*>(m_memory + addr - 8) = block;
            *reinterpret_cast<uint32_t*>(m_memory + addr - 4) = tag;
            return addr;
        }
    }

    uint32_t payload = (m_heap_current + kHeaderSize + static_cast<uint32_t>(alignment - 1)) &
                       ~static_cast<uint32_t>(alignment - 1);
    if (static_cast<uint64_t>(payload) + block > kHeapLimit) {
        LOGE("GuestAlloc out of guest heap: at 0x%08X, want %zu, limit 0x%08X", payload, size, kHeapLimit);
        return 0;
    }
    m_heap_current = payload + block;
    *reinterpret_cast<uint32_t*>(m_memory + payload - 8) = block;
    *reinterpret_cast<uint32_t*>(m_memory + payload - 4) = tag;
    return payload;
}

uint32_t DynarmicHost::GuestAllocSize(uint32_t addr) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (addr < kHeapBase + kHeaderSize || addr >= kHeapLimit) return 0;
    uint32_t tag = *reinterpret_cast<uint32_t*>(m_memory + addr - 4);
    if ((tag >> 16) != kBlockMagic) return 0;
    return *reinterpret_cast<uint32_t*>(m_memory + addr - 8);
}

void DynarmicHost::GuestFree(uint32_t addr) {
    if (!addr) return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    uint32_t block = GuestAllocSize(addr);
    if (!block) return;  // not ours, or freed twice
    // Any guest lock that lived in this block dies with it.
    GuestSyncForget(addr, block);
    uint32_t tag = *reinterpret_cast<uint32_t*>(m_memory + addr - 4);
    // Clearing the magic is what makes a double free a no-op instead of putting
    // the same address on the list twice.
    *reinterpret_cast<uint32_t*>(m_memory + addr - 4) = 0;
    m_free_blocks[BlockKey(block, tag)].push_back(addr);
}

bool DynarmicHost::HeapSelfCheck() {
    auto fail = [](const char* condition) {
        LOGE("HeapSelfCheck FAILED: %s", condition);
        return false;
    };

    uint32_t a = GuestAlloc(100, 8);
    uint32_t b = GuestAlloc(257, 64);
    if (!a || !b) return fail("allocation");
    if ((a & 7u) != 0 || (b & 63u) != 0) return fail("alignment");
    if (GuestAllocSize(a) < 100 || GuestAllocSize(b) < 257) return fail("size metadata");
    if (a + GuestAllocSize(a) > b && b + GuestAllocSize(b) > a)
        return fail("overlap");

    MemoryWrite32(a, 0xDEADBEEF);
    GuestFree(a);
    GuestFree(a);
    GuestFree(0x40000000);
    GuestFree(b + 8);
    uint32_t recycled = GuestAlloc(100, 8);
    if (recycled != a) return fail("recycling");
    if (MemoryRead32(recycled) != 0) return fail("recycled block zeroing");
    if (GuestAllocSize(b) < 257) return fail("foreign/interior free isolation");

    GuestFree(recycled);
    GuestFree(b);
    LOGI("HeapSelfCheck OK");
    return true;
}

std::optional<std::uint32_t> DynarmicHost::MemoryReadCode(std::uint32_t vaddr) {
    if (static_cast<uint64_t>(vaddr) + 4 > m_memory_size) return std::nullopt;

    const uint32_t word = *reinterpret_cast<uint32_t*>(m_memory + vaddr);
    {
        std::lock_guard<std::mutex> lock(m_probe_mutex);
        if (m_bootstrap_probe_armed && !m_bootstrap_probe.code_fetch_observed &&
            vaddr == (m_bootstrap_probe.requested_pc & ~1u)) {
            m_bootstrap_probe.code_fetch_observed = true;
            m_bootstrap_probe.fetched_pc = vaddr;
            LOGI("bootstrap first code fetch pc=0x%08X word=0x%08X", vaddr, word);
        } else if (m_bootstrap_probe_armed && m_bootstrap_probe.code_fetch_observed &&
                   !m_bootstrap_probe.pc_advanced && vaddr != m_bootstrap_probe.fetched_pc) {
            m_bootstrap_probe.pc_advanced = true;
            m_bootstrap_probe.final_pc = vaddr;
            LOGI("bootstrap guest PC advanced 0x%08X -> 0x%08X", m_bootstrap_probe.fetched_pc,
                 vaddr);
        }
    }
    return word;
}

std::uint8_t DynarmicHost::MemoryRead8(std::uint32_t vaddr) {
    if (static_cast<uint64_t>(vaddr) < m_memory_size) return m_memory[vaddr];
    return 0;
}

std::uint16_t DynarmicHost::MemoryRead16(std::uint32_t vaddr) {
    if (static_cast<uint64_t>(vaddr) + 2 <= m_memory_size) return *reinterpret_cast<uint16_t*>(m_memory + vaddr);
    return 0;
}

std::uint32_t DynarmicHost::MemoryRead32(std::uint32_t vaddr) {
    if (static_cast<uint64_t>(vaddr) + 4 <= m_memory_size) return *reinterpret_cast<uint32_t*>(m_memory + vaddr);
    return 0;
}

std::uint64_t DynarmicHost::MemoryRead64(std::uint32_t vaddr) {
    if (static_cast<uint64_t>(vaddr) + 8 <= m_memory_size) return *reinterpret_cast<uint64_t*>(m_memory + vaddr);
    return 0;
}

void DynarmicHost::MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) {
    if (static_cast<uint64_t>(vaddr) < m_memory_size) m_memory[vaddr] = value;
}

void DynarmicHost::MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) {
    if (static_cast<uint64_t>(vaddr) + 2 <= m_memory_size) *reinterpret_cast<uint16_t*>(m_memory + vaddr) = value;
}

void DynarmicHost::MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) {
    if (static_cast<uint64_t>(vaddr) + 4 <= m_memory_size) *reinterpret_cast<uint32_t*>(m_memory + vaddr) = value;
}

void DynarmicHost::MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) {
    if (static_cast<uint64_t>(vaddr) + 8 <= m_memory_size) *reinterpret_cast<uint64_t*>(m_memory + vaddr) = value;
}

void DynarmicHost::InterpreterFallback(std::uint32_t pc, std::size_t num_instructions) {
    LOGE("Interpreter Fallback at 0x%08X", pc);
    CurrentJit().HaltExecution(Dynarmic::HaltReason::UserDefined1);
}

// The ARM trap word is `svc #0` only when it is executed as ARM; as Thumb it is
// two harmless halfwords and the guest walks straight off the end of it. So the
// mode has to be set along with the PC, not left as whatever the guest was in.
void GuestJumpToExitTrap(Dynarmic::A32::Jit& jit) {
    jit.SetCpsr(jit.Cpsr() & ~0x20u);
    jit.Regs()[15] = EXIT_TRAP_ARM;
}

// ARM private syscalls, issued as "svc #0" with the number in r7.
static constexpr uint32_t kArmNrCacheflush = 0x0f0002;
static constexpr uint32_t kArmNrSetTls     = 0x0f0005;

void DynarmicHost::CallSVC(std::uint32_t swi) {
    Dynarmic::A32::Jit& jit = CurrentJit();

    if (swi == kNullCallSvc) {
        LOGE("guest called through a null function pointer, return address 0x%08X",
             jit.Regs()[14]);
        GuestJumpToExitTrap(jit);
        return;
    }

    // Any SVC executed inside the trap page is a guest function returning, whatever
    // immediate the word happened to decode to. The two traps hold different words
    // and each is only an SVC 0 in one instruction set: read the other way round,
    // the Thumb trap is `svcle #0xDF00` and the ARM trap is not an SVC at all. A
    // guest that returns in the mode the trap was not written for therefore used to
    // be dispatched as thunk 57088 and then carried on past the trap - into the dead
    // page, where nothing could stop it again. Deciding by PC covers every
    // combination without caring which of them arrived.
    if (jit.Regs()[15] <= kTrapPageEnd) {
        // diag: how often the guest comes back in the other instruction set at all.
        // Once per call would drown the log - the traps are hit as often as guest
        // functions return.
        if (swi != 0) {
            static std::atomic<uint32_t> mismatched{0};
            uint32_t n = mismatched.fetch_add(1);
            if (n < 4 || (n % 256) == 0) {
                LOGI("[trap] guest returned to the exit trap in the other instruction set "
                     "(svc %u, PC 0x%08X, lr 0x%08X, #%u)", swi, jit.Regs()[15], jit.Regs()[14], n);
            }
        }
        jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
        return;
    }

    // swi != 0 is a bridge trampoline.
    if (swi != 0) {
        m_thunk_manager->DispatchThunk(swi, jit);
        return;
    }

    // A real "svc #0" from guest code. Mono emits code at runtime and then asks the
    // kernel to flush the icache for it; without honouring that, the JIT keeps
    // executing the translation of whatever used to be at those addresses, which
    // shows up much later as random crashes once the managed JIT warms up.
    const uint32_t nr = jit.Regs()[7];
    switch (nr) {
        case kArmNrCacheflush: {
            uint32_t start = jit.Regs()[0];
            uint32_t end = jit.Regs()[1];
            if (end > start) InvalidateGuestCode(start, end - start);
            jit.Regs()[0] = 0;
            return;
        }
        case kArmNrSetTls:
            // Guest code does read this back, through `mrc p15, 0, Rt, c13, c0, 3`,
            // so the syscall and the coprocessor have to share one value.
            GuestCp15SetCurrentTls(jit.Regs()[0]);
            jit.Regs()[0] = 0;
            return;
        default:
            LOGE("unhandled guest syscall %u (0x%X) at PC 0x%08X", nr, nr, jit.Regs()[15]);
            jit.Regs()[0] = static_cast<uint32_t>(-38);  // -ENOSYS
            return;
    }
}

void DynarmicHost::ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) {
    LOGE("Exception at PC: 0x%08X (type %d)", pc, static_cast<int>(exception));
    CurrentJit().HaltExecution(Dynarmic::HaltReason::UserDefined1);
}

void DynarmicHost::AddTicks(std::uint64_t ticks) {
    if (m_ticks_remaining > ticks) m_ticks_remaining -= ticks;
    else m_ticks_remaining = 0;
}

std::uint64_t DynarmicHost::GetTicksRemaining() {
    return m_ticks_remaining;
}

// STREX: compare-and-set against guest memory.
//
// This has to be one atomic operation, not a compare followed by a store. The
// ExclusiveMonitor's lock only serialises exclusive accesses against each other,
// and the other half of a guest lock is a plain store that fastmem writes straight
// into the arena without passing through here at all. Boehm releases its allocator
// lock exactly that way - GC_clear is `dmb sy; str #0, [r3]`.
//
// So with a read-compare-write, a spinning GC_test_and_set (which stores 1
// unconditionally and reports what it read) could compare against 1, have the
// owner's release land in the gap, and then put its own 1 back on top of the zero.
// The lock came back held with nobody holding it, and every thread queued on it
// spun until the process was killed. It took a few thousand strex/release pairs a
// second to hit, which is why roughly one launch in eight got through to the menu.
template<typename T>
static bool GuestCompareAndSwap(uint8_t* mem, size_t mem_size, uint32_t vaddr, T value, T expected) {
    if (static_cast<uint64_t>(vaddr) + sizeof(T) > mem_size) return false;
    // LDREX/STREX fault on an unaligned address on real hardware, and so would the
    // host's own atomics; there is nothing to be correct about in that case.
    if ((vaddr & (sizeof(T) - 1)) != 0) return false;
    auto* p = reinterpret_cast<std::atomic<T>*>(mem + vaddr);
    return p->compare_exchange_strong(expected, value, std::memory_order_seq_cst);
}

bool DynarmicHost::MemoryWriteExclusive8(std::uint32_t vaddr, std::uint8_t value, std::uint8_t expected) {
    return GuestCompareAndSwap(m_memory, m_memory_size, vaddr, value, expected);
}

bool DynarmicHost::MemoryWriteExclusive16(std::uint32_t vaddr, std::uint16_t value, std::uint16_t expected) {
    return GuestCompareAndSwap(m_memory, m_memory_size, vaddr, value, expected);
}

bool DynarmicHost::MemoryWriteExclusive32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t expected) {
    return GuestCompareAndSwap(m_memory, m_memory_size, vaddr, value, expected);
}

bool DynarmicHost::MemoryWriteExclusive64(std::uint32_t vaddr, std::uint64_t value, std::uint64_t expected) {
    return GuestCompareAndSwap(m_memory, m_memory_size, vaddr, value, expected);
}
