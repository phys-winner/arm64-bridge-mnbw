#include "../src/apk_reader.h"
#include "../src/elf_loader.h"
#include "../src/guest_apk_slice.h"

int ApkOpenEntry(const char* /*path*/, off_t* out_size) {
    if (out_size) *out_size = 0;
    return -1;
}

#include <android/asset_manager.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Link this test with:
//   -Wl,--wrap=dup -Wl,--wrap=close -Wl,--wrap=fstat
//   -Wl,--wrap=mmap -Wl,--wrap=munmap
// The wrappers below inject failures without changing production code.
extern "C" int __real_dup(int);
extern "C" int __real_close(int);
extern "C" int __real_fstat(int, struct stat*);
extern "C" void* __real_mmap(void*, size_t, int, int, int, off_t);
extern "C" int __real_munmap(void*, size_t);

struct HookState {
    bool fail_dup = false;
    bool fail_close = false;
    bool fail_fstat = false;
    bool fail_mmap = false;
    bool fail_munmap = false;
    int dup_calls = 0;
    int close_calls = 0;
    int fstat_calls = 0;
    int mmap_calls = 0;
    int munmap_calls = 0;
    std::unordered_set<int> live_fds;
    std::unordered_map<void*, size_t> live_maps;
};

static HookState g_hooks;

extern "C" int __wrap_dup(int fd) {
    ++g_hooks.dup_calls;
    if (g_hooks.fail_dup) {
        errno = EMFILE;
        return -1;
    }
    const int result = __real_dup(fd);
    if (result >= 0) g_hooks.live_fds.insert(result);
    return result;
}

extern "C" int __wrap_close(int fd) {
    ++g_hooks.close_calls;
    if (g_hooks.fail_close) {
        errno = EIO;
        return -1;
    }
    const int result = __real_close(fd);
    if (result == 0) g_hooks.live_fds.erase(fd);
    return result;
}

extern "C" int __wrap_fstat(int fd, struct stat* status) {
    ++g_hooks.fstat_calls;
    if (g_hooks.fail_fstat) {
        errno = EIO;
        return -1;
    }
    return __real_fstat(fd, status);
}

extern "C" void* __wrap_mmap(void* address, size_t length, int prot, int flags,
                              int fd, off_t offset) {
    ++g_hooks.mmap_calls;
    if (g_hooks.fail_mmap) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    void* result = __real_mmap(address, length, prot, flags, fd, offset);
    if (result != MAP_FAILED) g_hooks.live_maps.emplace(result, length);
    return result;
}

extern "C" int __wrap_munmap(void* address, size_t length) {
    ++g_hooks.munmap_calls;
    if (g_hooks.fail_munmap) {
        errno = EIO;
        return -1;
    }
    const int result = __real_munmap(address, length);
    if (result == 0) g_hooks.live_maps.erase(address);
    return result;
}

// Minimal fake Android asset API. The fake accepts only the two fixed guestlib
// labels and returns descriptors into the caller-supplied APK.
struct AAsset {
    int internal_fd = -1;
    off64_t offset = 0;
    off64_t length = 0;
    bool compressed = false;
};

struct AAssetManager {
    int apk_fd = -1;
    off64_t fmod_offset = 0;
    off64_t fmod_size = 0;
    off64_t game_offset = 0;
    off64_t game_size = 0;
    bool fmod_compressed = false;
    bool game_compressed = false;
    bool fail_next_open = false;
};

static int g_asset_objects = 0;
static int g_asset_releases = 0;
static int g_live_assets = 0;

static constexpr const char* kFmodAsset = "guestlib/libfmodex.so";
static constexpr const char* kGameAsset = "guestlib/libMBExpMobile.so";

extern "C" AAsset* AAssetManager_open(AAssetManager* manager, const char* asset,
                                       int /*mode*/) {
    if (!manager || !asset) return nullptr;
    if (manager->fail_next_open) {
        manager->fail_next_open = false;
        return nullptr;
    }
    off64_t offset = 0;
    off64_t length = 0;
    bool compressed = false;
    if (std::strcmp(asset, kFmodAsset) == 0) {
        offset = manager->fmod_offset;
        length = manager->fmod_size;
        compressed = manager->fmod_compressed;
    } else if (std::strcmp(asset, kGameAsset) == 0) {
        offset = manager->game_offset;
        length = manager->game_size;
        compressed = manager->game_compressed;
    } else {
        return nullptr;
    }

    const int fd = fcntl(manager->apk_fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return nullptr;
    g_hooks.live_fds.insert(fd);
    auto* result = new (std::nothrow) AAsset;
    if (!result) {
        g_hooks.live_fds.erase(fd);
        __real_close(fd);
        return nullptr;
    }
    result->internal_fd = fd;
    result->offset = offset;
    result->length = length;
    result->compressed = compressed;
    ++g_asset_objects;
    ++g_live_assets;
    return result;
}

extern "C" int AAsset_openFileDescriptor64(AAsset* asset, off64_t* out_start,
                                             off64_t* out_length) {
    if (!asset || asset->compressed || !out_start || !out_length) {
        errno = EINVAL;
        return -1;
    }
    const int raw_fd = fcntl(asset->internal_fd, F_DUPFD_CLOEXEC, 0);
    if (raw_fd < 0) return -1;
    g_hooks.live_fds.insert(raw_fd);
    *out_start = asset->offset;
    *out_length = asset->length;
    return raw_fd;
}

extern "C" void AAsset_close(AAsset* asset) {
    if (!asset) return;
    if (asset->internal_fd >= 0 && g_hooks.live_fds.erase(asset->internal_fd) != 0) {
        __real_close(asset->internal_fd);
    }
    ++g_asset_releases;
    --g_live_assets;
    delete asset;
}

struct TestState {
    int checks = 0;
    int failures = 0;

    void Check(bool condition, const char* expression, const char* file, int line) {
        ++checks;
        if (!condition) {
            ++failures;
            std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        }
    }
};

#define CHECK(state, expression) (state).Check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

static void DrainWrappedResources() {
    for (const int fd : g_hooks.live_fds) __real_close(fd);
    g_hooks.live_fds.clear();
    for (const auto& [address, length] : g_hooks.live_maps) __real_munmap(address, length);
    g_hooks.live_maps.clear();
}

static void ResetHooks() {
    DrainWrappedResources();
    g_hooks.fail_dup = false;
    g_hooks.fail_close = false;
    g_hooks.fail_fstat = false;
    g_hooks.fail_mmap = false;
    g_hooks.fail_munmap = false;
    g_hooks.dup_calls = 0;
    g_hooks.close_calls = 0;
    g_hooks.fstat_calls = 0;
    g_hooks.mmap_calls = 0;
    g_hooks.munmap_calls = 0;
}

static void CheckResourcesBalanced(TestState& state) {
    CHECK(state, g_hooks.live_fds.empty());
    CHECK(state, g_hooks.live_maps.empty());
    CHECK(state, g_live_assets == 0);
    CHECK(state, g_asset_objects == g_asset_releases);
}

static void TestMapArithmetic(TestState& state) {
    GuestApkRange range{7, 8, 9};
    CHECK(state, GuestApkMapRange(0x10000, 0x1234, 0x200, 4096, range));
    CHECK(state, range.offset == 0x1000);
    CHECK(state, range.bias == 0x234);
    CHECK(state, range.size == 0x434);

    CHECK(state, GuestApkMapRange(0x20000, 0x3fff, 0x1000, 16384, range));
    CHECK(state, range.offset == 0);
    CHECK(state, range.bias == 0x3fff);
    CHECK(state, range.size == 0x4fff);

    const GuestApkRange unchanged = range;
    CHECK(state, !GuestApkMapRange(-1, 0, 1, 4096, range));
    CHECK(state, range.offset == unchanged.offset && range.bias == unchanged.bias &&
                    range.size == unchanged.size);
    CHECK(state, !GuestApkMapRange(100, -1, 1, 4096, range));
    CHECK(state, !GuestApkMapRange(100, 0, 0, 4096, range));
    CHECK(state, !GuestApkMapRange(100, 99, 2, 4096, range));
    CHECK(state, !GuestApkMapRange(100, 101, 1, 4096, range));
    CHECK(state, !GuestApkMapRange(100, 0, 64 * 1024 * 1024 + 1, 4096, range));
    CHECK(state, !GuestApkMapRange(100, 0, 1, 0, range));
    CHECK(state, !GuestApkMapRange(100, 0, 1, 3000, range));
}

static void TestSliceFailures(TestState& state, AAssetManager& manager) {
    ResetHooks();

    GuestApkSlice missing;
    CHECK(state, !missing.Open(&manager, "guestlib/not-present.so"));
    CheckResourcesBalanced(state);

    manager.fmod_compressed = true;
    GuestApkSlice compressed;
    CHECK(state, !compressed.Open(&manager, kFmodAsset));
    manager.fmod_compressed = false;
    CheckResourcesBalanced(state);

    g_hooks.fail_dup = true;
    GuestApkSlice dup_failed;
    CHECK(state, !dup_failed.Open(&manager, kFmodAsset));
    g_hooks.fail_dup = false;
    CheckResourcesBalanced(state);

    g_hooks.fail_fstat = true;
    GuestApkSlice fstat_failed;
    CHECK(state, !fstat_failed.Open(&manager, kFmodAsset));
    g_hooks.fail_fstat = false;
    CheckResourcesBalanced(state);

    g_hooks.fail_mmap = true;
    GuestApkSlice mmap_failed;
    CHECK(state, !mmap_failed.Open(&manager, kFmodAsset));
    g_hooks.fail_mmap = false;
    CheckResourcesBalanced(state);

    GuestApkSlice close_failed;
    CHECK(state, close_failed.Open(&manager, kFmodAsset));
    g_hooks.fail_close = true;
    CHECK(state, !close_failed.Close(0x1000));
    CHECK(state, !g_hooks.live_fds.empty());
    g_hooks.fail_close = false;
    DrainWrappedResources();
    CheckResourcesBalanced(state);

    GuestApkSlice munmap_failed;
    CHECK(state, munmap_failed.Open(&manager, kFmodAsset));
    g_hooks.fail_munmap = true;
    CHECK(state, !munmap_failed.Close(0x2000));
    g_hooks.fail_munmap = false;
    DrainWrappedResources();
    CheckResourcesBalanced(state);

    GuestApkSlice second_open;
    CHECK(state, second_open.Open(&manager, kFmodAsset));
    const uint8_t* data = second_open.Data();
    const size_t size = second_open.Size();
    CHECK(state, !second_open.Open(&manager, kGameAsset));
    CHECK(state, second_open.Data() == data && second_open.Size() == size);
    CHECK(state, second_open.Close(0x3000));
    CHECK(state, second_open.Close(0x3000));
    CheckResourcesBalanced(state);

    manager.fail_next_open = false;
    {
        GuestApkSlice first_asset;
        CHECK(state, first_asset.Open(&manager, kFmodAsset));
        manager.fail_next_open = true;
        {
            GuestApkSlice second_asset;
            CHECK(state, !second_asset.Open(&manager, kGameAsset));
        }
        CHECK(state, first_asset.Data() != nullptr && first_asset.Size() > 0);
    }
    CheckResourcesBalanced(state);
}

static std::vector<uint8_t> MakeSyntheticElf() {
    constexpr size_t kFileSize = 0x5000;
    constexpr uint32_t kDynamic = 0x1000;
    constexpr uint32_t kStrtab = 0x2000;
    constexpr uint32_t kSymtab = 0x2100;
    constexpr uint32_t kHash = 0x2200;
    constexpr uint32_t kRel = 0x2300;
    constexpr uint32_t kJmpRel = 0x2310;
    constexpr uint32_t kInitArray = 0x2400;
    std::vector<uint8_t> bytes(kFileSize, 0);

    auto put = [&bytes](size_t offset, const auto& value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };

    Elf32_Ehdr ehdr{};
    std::memcpy(ehdr.e_ident, ELFMAG, SELFMAG);
    ehdr.e_ident[EI_CLASS] = ELFCLASS32;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_type = ET_DYN;
    ehdr.e_machine = EM_ARM;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_phoff = sizeof(Elf32_Ehdr);
    ehdr.e_ehsize = sizeof(Elf32_Ehdr);
    ehdr.e_phentsize = sizeof(Elf32_Phdr);
    ehdr.e_phnum = 2;
    put(0, ehdr);

    Elf32_Phdr load{};
    load.p_type = PT_LOAD;
    load.p_offset = 0;
    load.p_vaddr = 0;
    load.p_filesz = kFileSize;
    load.p_memsz = kFileSize;
    load.p_flags = PF_R | PF_W | PF_X;
    load.p_align = 0x1000;
    put(ehdr.e_phoff, load);

    Elf32_Phdr dynamic{};
    dynamic.p_type = PT_DYNAMIC;
    dynamic.p_offset = kDynamic;
    dynamic.p_vaddr = kDynamic;
    dynamic.p_filesz = 16 * sizeof(Elf32_Dyn);
    dynamic.p_memsz = dynamic.p_filesz;
    dynamic.p_align = 4;
    put(ehdr.e_phoff + sizeof(Elf32_Phdr), dynamic);

    const std::string strings("\0libsynthetic.so\0exported\0missing\0", 35);
    std::memcpy(bytes.data() + kStrtab, strings.data(), strings.size());
    const uint32_t needed_name = 1;
    const uint32_t exported_name = needed_name + 16;
    const uint32_t missing_name = exported_name + 9;

    Elf32_Sym symbols[3]{};
    symbols[1].st_name = exported_name;
    symbols[1].st_value = 0x2800;
    symbols[1].st_size = 4;
    symbols[1].st_info = ELF32_ST_INFO(STB_GLOBAL, STT_FUNC);
    symbols[1].st_shndx = 1;
    symbols[2].st_name = missing_name;
    symbols[2].st_info = ELF32_ST_INFO(STB_GLOBAL, STT_FUNC);
    symbols[2].st_shndx = SHN_UNDEF;
    std::memcpy(bytes.data() + kSymtab, symbols, sizeof(symbols));

    const uint32_t hash_words[] = {1, 3, 1, 0, 2, 0};
    std::memcpy(bytes.data() + kHash, hash_words, sizeof(hash_words));

    Elf32_Rel rels[2]{};
    rels[0].r_offset = 0x1800;
    rels[0].r_info = ELF32_R_INFO(0, R_ARM_RELATIVE);
    rels[1].r_offset = 0x1804;
    rels[1].r_info = ELF32_R_INFO(2, R_ARM_GLOB_DAT);
    std::memcpy(bytes.data() + kRel, rels, sizeof(rels));
    Elf32_Rel jmp{};
    jmp.r_offset = 0x1808;
    jmp.r_info = ELF32_R_INFO(2, R_ARM_JUMP_SLOT);
    std::memcpy(bytes.data() + kJmpRel, &jmp, sizeof(jmp));

    const uint32_t initial_relative = 0x1234;
    std::memcpy(bytes.data() + 0x1800, &initial_relative, sizeof(initial_relative));
    const uint32_t init_values[] = {0x2800, 0xffffffffu, 0};
    std::memcpy(bytes.data() + kInitArray, init_values, sizeof(init_values));

    std::vector<Elf32_Dyn> dynamic_entries;
    const auto add_dyn = [&dynamic_entries](Elf32_Sword tag, Elf32_Word value) {
        Elf32_Dyn entry{};
        entry.d_tag = tag;
        entry.d_un.d_val = value;
        dynamic_entries.push_back(entry);
    };
    add_dyn(DT_HASH, kHash);
    add_dyn(DT_STRTAB, kStrtab);
    add_dyn(DT_STRSZ, static_cast<Elf32_Word>(strings.size()));
    add_dyn(DT_SYMTAB, kSymtab);
    add_dyn(DT_SYMENT, sizeof(Elf32_Sym));
    add_dyn(DT_REL, kRel);
    add_dyn(DT_RELSZ, sizeof(rels));
    add_dyn(DT_RELENT, sizeof(Elf32_Rel));
    add_dyn(DT_JMPREL, kJmpRel);
    add_dyn(DT_PLTRELSZ, sizeof(jmp));
    add_dyn(DT_PLTREL, DT_REL);
    add_dyn(DT_INIT_ARRAY, kInitArray);
    add_dyn(DT_INIT_ARRAYSZ, sizeof(init_values));
    add_dyn(DT_NEEDED, needed_name);
    dynamic_entries.push_back(Elf32_Dyn{});
    std::memcpy(bytes.data() + kDynamic, dynamic_entries.data(),
                dynamic_entries.size() * sizeof(Elf32_Dyn));
    return bytes;
}

template <typename T>
static T ReadObject(const std::vector<uint8_t>& bytes, size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

template <typename T>
static void WriteObject(std::vector<uint8_t>& bytes, size_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

static size_t DynamicEntryOffset(const std::vector<uint8_t>& bytes, Elf32_Sword wanted) {
    const Elf32_Ehdr ehdr = ReadObject<Elf32_Ehdr>(bytes, 0);
    const Elf32_Phdr dynamic = ReadObject<Elf32_Phdr>(bytes, ehdr.e_phoff + sizeof(Elf32_Phdr));
    for (size_t offset = dynamic.p_offset;; offset += sizeof(Elf32_Dyn)) {
        const Elf32_Dyn entry = ReadObject<Elf32_Dyn>(bytes, offset);
        if (entry.d_tag == wanted) return offset;
        if (entry.d_tag == DT_NULL) return std::numeric_limits<size_t>::max();
    }
}

static bool SentinelUntouched(const std::vector<uint8_t>& memory, uint8_t value) {
    for (const uint8_t byte : memory) {
        if (byte != value) return false;
    }
    return true;
}

static void TestSyntheticElf(TestState& state) {
    constexpr uint32_t kSlot = 64 * 1024 * 1024;
    const std::vector<uint8_t> valid = MakeSyntheticElf();
    std::vector<uint8_t> memory(kSlot, 0xa5);
    ElfLoader loader(memory.data(), 0, kSlot);
    CHECK(state, loader.LoadFromMemory(valid.data(), valid.size()));
    const auto& needed = loader.GetNeededLibraries();
    const auto& undefined = loader.GetUndefinedSymbols();
    const auto& init_array = loader.GetInitArray();
    CHECK(state, needed.size() == 1);
    if (needed.size() == 1) CHECK(state, needed[0] == "libsynthetic.so");
    CHECK(state, loader.GetSymbolAddress("exported") == 0x2800);
    CHECK(state, undefined.size() == 1);
    CHECK(state, init_array.size() == 1);
    if (init_array.size() == 1) CHECK(state, init_array[0] == 0x2800);
    CHECK(state, loader.Relocate([](const std::string&) { return 0x90000000u; }));

    const uint32_t relative = ReadObject<uint32_t>(memory, 0x1800);
    const uint32_t glob = ReadObject<uint32_t>(memory, 0x1804);
    const uint32_t jump = ReadObject<uint32_t>(memory, 0x1808);
    CHECK(state, relative == 0x1234);
    CHECK(state, glob == 0x90000000u);
    CHECK(state, jump == 0x90000000u);

    std::vector<uint8_t> small_memory(0x2000, 0xa5);
    ElfLoader small_loader(small_memory.data(), 0, 0x2000);
    CHECK(state, !small_loader.LoadFromMemory(valid.data(), valid.size()));
    CHECK(state, SentinelUntouched(small_memory, 0xa5));

    struct Mutation {
        const char* name;
        std::function<void(std::vector<uint8_t>&)> mutate;
    };
    std::vector<Mutation> mutations;
    mutations.push_back({"truncated-header", [](auto& bytes) {
        bytes.resize(sizeof(Elf32_Ehdr) - 1);
    }});
    mutations.push_back({"truncated-program-headers", [](auto& bytes) {
        const auto ehdr = ReadObject<Elf32_Ehdr>(bytes, 0);
        bytes.resize(ehdr.e_phoff + sizeof(Elf32_Phdr) - 1);
    }});
    mutations.push_back({"header-class", [](auto& bytes) {
        bytes[EI_CLASS] = ELFCLASS64;
    }});
    mutations.push_back({"program-header-range", [](auto& bytes) {
        auto ehdr = ReadObject<Elf32_Ehdr>(bytes, 0);
        ehdr.e_phoff = 0x7ffffff0u;
        WriteObject(bytes, 0, ehdr);
    }});
    mutations.push_back({"load-file-range", [](auto& bytes) {
        auto ehdr = ReadObject<Elf32_Ehdr>(bytes, 0);
        auto phdr = ReadObject<Elf32_Phdr>(bytes, ehdr.e_phoff);
        phdr.p_offset = 0x4ff0;
        phdr.p_filesz = 0x100;
        WriteObject(bytes, ehdr.e_phoff, phdr);
    }});
    mutations.push_back({"filesz-exceeds-memsz", [](auto& bytes) {
        auto ehdr = ReadObject<Elf32_Ehdr>(bytes, 0);
        auto phdr = ReadObject<Elf32_Phdr>(bytes, ehdr.e_phoff);
        phdr.p_filesz = phdr.p_memsz + 1;
        WriteObject(bytes, ehdr.e_phoff, phdr);
    }});
    mutations.push_back({"segment-address-overflow", [](auto& bytes) {
        auto ehdr = ReadObject<Elf32_Ehdr>(bytes, 0);
        auto phdr = ReadObject<Elf32_Phdr>(bytes, ehdr.e_phoff);
        phdr.p_vaddr = 0xfffff000u;
        phdr.p_memsz = 0x200;
        WriteObject(bytes, ehdr.e_phoff, phdr);
    }});
    mutations.push_back({"load-slot-capacity", [](auto& bytes) {
        auto ehdr = ReadObject<Elf32_Ehdr>(bytes, 0);
        auto phdr = ReadObject<Elf32_Phdr>(bytes, ehdr.e_phoff);
        phdr.p_memsz = 64 * 1024 * 1024 + 1;
        WriteObject(bytes, ehdr.e_phoff, phdr);
    }});
    mutations.push_back({"dynamic-range", [](auto& bytes) {
        auto ehdr = ReadObject<Elf32_Ehdr>(bytes, 0);
        auto phdr = ReadObject<Elf32_Phdr>(bytes, ehdr.e_phoff + sizeof(Elf32_Phdr));
        phdr.p_offset = 0x4ff0;
        phdr.p_vaddr = 0x4ff0;
        phdr.p_filesz = sizeof(Elf32_Dyn) * 2;
        phdr.p_memsz = phdr.p_filesz;
        WriteObject(bytes, ehdr.e_phoff + sizeof(Elf32_Phdr), phdr);
    }});
    mutations.push_back({"string-table-range", [](auto& bytes) {
        const size_t offset = DynamicEntryOffset(bytes, DT_STRTAB);
        auto entry = ReadObject<Elf32_Dyn>(bytes, offset);
        entry.d_un.d_ptr = 0x4ff0;
        WriteObject(bytes, offset, entry);
    }});
    mutations.push_back({"needed-string-index", [](auto& bytes) {
        const size_t offset = DynamicEntryOffset(bytes, DT_NEEDED);
        auto entry = ReadObject<Elf32_Dyn>(bytes, offset);
        entry.d_un.d_val = 0x4ff0;
        WriteObject(bytes, offset, entry);
    }});
    mutations.push_back({"unterminated-string-table", [](auto& bytes) {
        bytes[0x2000 + 33] = 'X';
        bytes[0x2000 + 34] = 'X';
    }});
    mutations.push_back({"hash-range", [](auto& bytes) {
        const size_t offset = DynamicEntryOffset(bytes, DT_HASH);
        auto entry = ReadObject<Elf32_Dyn>(bytes, offset);
        entry.d_un.d_ptr = 0x4ff0;
        WriteObject(bytes, offset, entry);
    }});
    mutations.push_back({"hash-count-range", [](auto& bytes) {
        const uint32_t huge_chain_count = 0x7fffffffu;
        WriteObject(bytes, 0x2200 + sizeof(uint32_t), huge_chain_count);
    }});
    mutations.push_back({"bad-symbol-span", [](auto& bytes) {
        const size_t offset = DynamicEntryOffset(bytes, DT_SYMTAB);
        auto entry = ReadObject<Elf32_Dyn>(bytes, offset);
        entry.d_un.d_ptr = 0x4ff0;
        WriteObject(bytes, offset, entry);
    }});
    mutations.push_back({"unaligned-relocation-table", [](auto& bytes) {
        const size_t offset = DynamicEntryOffset(bytes, DT_REL);
        auto entry = ReadObject<Elf32_Dyn>(bytes, offset);
        entry.d_un.d_ptr = 0x2301;
        WriteObject(bytes, offset, entry);
    }});
    mutations.push_back({"relocation-target-range", [](auto& bytes) {
        auto rel = ReadObject<Elf32_Rel>(bytes, 0x2300);
        rel.r_offset = 0x5000;
        WriteObject(bytes, 0x2300, rel);
    }});
    mutations.push_back({"relocation-target-nonwritable", [](auto& bytes) {
        const auto ehdr = ReadObject<Elf32_Ehdr>(bytes, 0);
        auto load = ReadObject<Elf32_Phdr>(bytes, ehdr.e_phoff);
        load.p_flags = PF_R | PF_X;
        WriteObject(bytes, ehdr.e_phoff, load);
    }});
    mutations.push_back({"relocation-symbol-index", [](auto& bytes) {
        auto rel = ReadObject<Elf32_Rel>(bytes, 0x2300);
        rel.r_info = ELF32_R_INFO(99, R_ARM_GLOB_DAT);
        WriteObject(bytes, 0x2300, rel);
    }});
    mutations.push_back({"unsupported-relocation", [](auto& bytes) {
        auto rel = ReadObject<Elf32_Rel>(bytes, 0x2300);
        rel.r_info = ELF32_R_INFO(0, 0x7f);
        WriteObject(bytes, 0x2300, rel);
    }});
    mutations.push_back({"init-array-range", [](auto& bytes) {
        const size_t offset = DynamicEntryOffset(bytes, DT_INIT_ARRAY);
        auto entry = ReadObject<Elf32_Dyn>(bytes, offset);
        entry.d_un.d_ptr = 0x4ff8;
        WriteObject(bytes, offset, entry);
    }});
    mutations.push_back({"missing-dynamic-null", [](auto& bytes) {
        const size_t null_offset = DynamicEntryOffset(bytes, DT_NULL);
        auto nonnull = ReadObject<Elf32_Dyn>(bytes, null_offset);
        nonnull.d_tag = DT_NEEDED;
        nonnull.d_un.d_val = 0x4ff0;
        WriteObject(bytes, null_offset, nonnull);
        WriteObject(bytes, null_offset + sizeof(Elf32_Dyn), nonnull);
    }});
    mutations.push_back({"forbidden-rela", [](auto& bytes) {
        const size_t offset = DynamicEntryOffset(bytes, DT_NULL);
        Elf32_Dyn forbidden{};
        forbidden.d_tag = DT_RELA;
        forbidden.d_un.d_ptr = 0x2300;
        WriteObject(bytes, offset, forbidden);
    }});

    for (const Mutation& mutation : mutations) {
        std::vector<uint8_t> mutated = valid;
        mutation.mutate(mutated);
        std::vector<uint8_t> sentinel(kSlot, 0xa5);
        ElfLoader rejected(sentinel.data(), 0, kSlot);
        const bool accepted = rejected.LoadFromMemory(mutated.data(), mutated.size());
        if (accepted) {
            std::fprintf(stderr, "FAIL synthetic mutation accepted: %s\n", mutation.name);
            ++state.failures;
        } else {
            ++state.checks;
        }
        CHECK(state, SentinelUntouched(sentinel, 0xa5));
    }
}

static void TestUnalignedMappedElf(TestState& state) {
    ResetHooks();
    constexpr off64_t kOffset = 0x3fff; // Odd byte address, unaligned on 4K and 16K.
    constexpr uint32_t kBase = 0x10000;
    constexpr uint32_t kSlot = 0x10000;
    const auto valid = MakeSyntheticElf();
    // Test-only synthetic input, never a production guest loading fallback.
    // Retain the unique fixture even on failure so the mapped bytes are inspectable.
    char path[] = "/data/local/tmp/phase7_slice20b_fixture_XXXXXX";
    const int fd = mkstemp(path);
    CHECK(state, fd >= 0);
    if (fd < 0) return;
    std::printf("synthetic fixture=%s offset=0x%llx size=%zu base=0x%x\n",
                path, static_cast<long long>(kOffset), valid.size(), kBase);
    std::fflush(stdout);
    const ssize_t written = pwrite(fd, valid.data(), valid.size(), kOffset);
    CHECK(state, written == static_cast<ssize_t>(valid.size()));
    if (written != static_cast<ssize_t>(valid.size())) {
        CHECK(state, __real_close(fd) == 0);
        return;
    }

    AAssetManager manager;
    manager.apk_fd = fd;
    manager.fmod_offset = kOffset;
    manager.fmod_size = static_cast<off64_t>(valid.size());
    GuestApkSlice slice;
    const bool opened = slice.Open(&manager, kFmodAsset);
    CHECK(state, opened);
    CHECK(state, __real_close(fd) == 0); // The slice owns its independent descriptor.
    if (!opened) return;
    CHECK(state, slice.Size() == valid.size());
    CHECK(state, std::memcmp(slice.Data(), valid.data(), valid.size()) == 0);
    const long page = sysconf(_SC_PAGESIZE);
    CHECK(state, page > 0);
    if (page <= 0) return;
    const size_t bias = static_cast<size_t>(kOffset % page);
    CHECK(state, bias != 0);
    CHECK(state, reinterpret_cast<uintptr_t>(slice.Data()) % page == bias);
    CHECK(state, g_hooks.live_maps.size() == 1);
    if (g_hooks.live_maps.size() == 1) {
        const auto& [address, length] = *g_hooks.live_maps.begin();
        CHECK(state, slice.Data() == static_cast<const uint8_t*>(address) + bias);
        CHECK(state, length == bias + valid.size());
    }

    std::vector<uint8_t> memory(kBase + kSlot, 0xa5);
    ElfLoader loader(memory.data(), kBase, kSlot);
    const bool loaded = loader.LoadFromMemory(slice.Data(), slice.Size());
    CHECK(state, loaded);
    const bool closed = slice.Close(kBase);
    CHECK(state, closed);
    CHECK(state, slice.Data() == nullptr && slice.Size() == 0);
    CheckResourcesBalanced(state);
    if (!loaded || !closed) return;

    CHECK(state, loader.GetSymbolAddress("exported") == kBase + 0x2800);
    CHECK(state, loader.GetNeededLibraries() == std::vector<std::string>{"libsynthetic.so"});
    CHECK(state, loader.GetInitArray() == std::vector<uint32_t>{kBase + 0x2800});
    CHECK(state, loader.Relocate([](const std::string&) { return 0x90000000u; }));
    CHECK(state, ReadObject<uint32_t>(memory, kBase + 0x1800) == kBase + 0x1234);
    CHECK(state, ReadObject<uint32_t>(memory, kBase + 0x1804) == 0x90000000u);
    CHECK(state, ReadObject<uint32_t>(memory, kBase + 0x1808) == 0x90000000u);
    CHECK(state, loader.GetSymbolAddress("exported") == kBase + 0x2800);
    CHECK(state, loader.GetInitArray() == std::vector<uint32_t>{kBase + 0x2800});
    CHECK(state, std::all_of(memory.begin(), memory.begin() + kBase,
                            [](uint8_t value) { return value == 0xa5; }));
    CHECK(state, std::all_of(memory.begin() + kBase + valid.size(), memory.end(),
                            [](uint8_t value) { return value == 0xa5; }));
    std::printf("unaligned mapped ELF: runtime_page=%ld bias=%zu nonzero_base=0x%x\n",
                page, bias, kBase);
}

static bool ParseInt64(const char* text, int64_t& value) {
    if (!text || !*text) return false;
    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(text, &end, 0);
    if (errno || !end || *end != '\0' || parsed < 0) return false;
    value = static_cast<int64_t>(parsed);
    return true;
}

static void TestRealGuests(TestState& state, AAssetManager& manager) {
    constexpr uint32_t kSlot = 64 * 1024 * 1024;
    struct GuestCase {
        const char* asset;
        const char* export_name;
        size_t expected_size;
    };
    const GuestCase guests[] = {
        {kFmodAsset, "FMOD_System_Create", static_cast<size_t>(manager.fmod_size)},
        {kGameAsset, "ANativeActivity_onCreate", static_cast<size_t>(manager.game_size)},
    };

    for (const GuestCase& guest : guests) {
        GuestApkSlice slice;
        CHECK(state, slice.Open(&manager, guest.asset));
        if (!slice.Data() || slice.Size() < SELFMAG) {
            std::fprintf(stderr, "FAIL real guest has no mapped bytes: %s\n", guest.asset);
            ++state.failures;
            continue;
        }
        CHECK(state, slice.Size() == guest.expected_size);
        CHECK(state, std::memcmp(slice.Data(), ELFMAG, SELFMAG) == 0);

        std::vector<uint8_t> memory(kSlot, 0);
        ElfLoader loader(memory.data(), 0, kSlot);
        CHECK(state, loader.LoadFromMemory(slice.Data(), slice.Size()));

        const std::vector<std::string> needed_before = loader.GetNeededLibraries();
        const std::vector<std::string> undefined_before = loader.GetUndefinedSymbols();
        const std::vector<uint32_t> init_before = loader.GetInitArray();
        const uint32_t export_before = loader.GetSymbolAddress(guest.export_name);
        const uint32_t init_function_before = loader.GetInitFunction();
        CHECK(state, export_before != 0);

        // Critical ownership proof: all parser metadata and copied segments must
        // survive after the APK mapping is closed, before relocation starts.
        CHECK(state, slice.Close(0x10000000u));
        CHECK(state, loader.GetNeededLibraries() == needed_before);
        CHECK(state, loader.GetUndefinedSymbols() == undefined_before);
        CHECK(state, loader.GetInitArray() == init_before);
        CHECK(state, loader.GetSymbolAddress(guest.export_name) == export_before);
        CHECK(state, loader.GetInitFunction() == init_function_before);

        CHECK(state, loader.Relocate([](const std::string&) { return 0x10000000u; }));
        CHECK(state, loader.GetNeededLibraries() == needed_before);
        CHECK(state, loader.GetUndefinedSymbols() == undefined_before);
        CHECK(state, loader.GetInitArray() == init_before);
        CHECK(state, loader.GetSymbolAddress(guest.export_name) == export_before);
        CHECK(state, loader.GetInitFunction() == init_function_before);
        CHECK(state, loader.GetImageSize() <= kSlot);
        std::printf("guest=%s needed=%zu undefined=%zu export=%s init=%zu init_func=0x%08x\n",
                    guest.asset, needed_before.size(), undefined_before.size(),
                    export_before ? "present" : "absent", init_before.size(),
                    init_function_before);
    }
    CheckResourcesBalanced(state);
}

int main(int argc, char** argv) {
    TestState state;
    TestMapArithmetic(state);
    TestSyntheticElf(state);
    if (!state.failures) TestUnalignedMappedElf(state);

    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) {
        std::printf("guest_slice_test self-test: %s (%d checks)\n",
                    state.failures ? "FAIL" : "PASS", state.checks);
        return state.failures ? 1 : 0;
    }
    if (argc != 6) {
        std::fprintf(stderr,
                     "usage: %s <APK_PATH> <FMOD_OFFSET> <FMOD_SIZE> "
                     "<GAME_OFFSET> <GAME_SIZE>\n",
                     argv[0]);
        return 2;
    }

    int64_t fmod_offset = 0;
    int64_t fmod_size = 0;
    int64_t game_offset = 0;
    int64_t game_size = 0;
    CHECK(state, ParseInt64(argv[2], fmod_offset));
    CHECK(state, ParseInt64(argv[3], fmod_size));
    CHECK(state, ParseInt64(argv[4], game_offset));
    CHECK(state, ParseInt64(argv[5], game_size));
    if (state.failures) return 1;

    const int apk_fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (apk_fd < 0) {
        std::fprintf(stderr, "cannot open APK: %s errno=%d\n", argv[1], errno);
        return 2;
    }
    struct stat apk_status{};
    if (__real_fstat(apk_fd, &apk_status) != 0) {
        std::fprintf(stderr, "cannot stat APK: errno=%d\n", errno);
        __real_close(apk_fd);
        return 2;
    }
    CHECK(state, fmod_offset >= 0 && fmod_size > 0 && fmod_offset <= apk_status.st_size &&
                    fmod_size <= apk_status.st_size - fmod_offset);
    CHECK(state, game_offset >= 0 && game_size > 0 && game_offset <= apk_status.st_size &&
                    game_size <= apk_status.st_size - game_offset);
    if (state.failures) {
        __real_close(apk_fd);
        return 1;
    }

    AAssetManager manager;
    manager.apk_fd = apk_fd;
    manager.fmod_offset = static_cast<off64_t>(fmod_offset);
    manager.fmod_size = static_cast<off64_t>(fmod_size);
    manager.game_offset = static_cast<off64_t>(game_offset);
    manager.game_size = static_cast<off64_t>(game_size);

    TestSliceFailures(state, manager);
    TestRealGuests(state, manager);
    ResetHooks();
    __real_close(apk_fd);

    std::printf("guest_slice_test: %s (%d checks)\n",
                state.failures ? "FAIL" : "PASS", state.checks);
    return state.failures ? 1 : 0;
}
