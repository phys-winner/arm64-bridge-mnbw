#include "elf_loader.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <map>
#include <new>
#include <unistd.h>
#include <android/log.h>

#include "apk_reader.h"

#define LOG_TAG "CMR_ElfLoader"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

bool Contains(uint64_t extent, uint64_t offset, uint64_t size) {
    return offset <= extent && size <= extent - offset;
}

template<typename T> T ReadValue(const uint8_t* bytes) {
    T value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

bool Reject(const char* why) {
    LOGE("ELF rejected before copy: %s", why);
    return false;
}

}  // namespace

ElfLoader::ElfLoader(uint8_t* guest_memory_base, uint32_t load_base_addr,
                     uint32_t max_image_size)
    : m_guest_mem(guest_memory_base), m_load_base(load_base_addr),
      m_max_image_size(max_image_size) {}

ElfLoader::~ElfLoader() = default;

// Legacy callers only. NativeActivity boot uses bounded GuestApkSlice views instead.
bool ElfLoader::Load(const std::string& filepath) {
    std::vector<uint8_t> buffer;
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (file.is_open()) {
        const auto length = file.tellg();
        if (length <= 0 || length > kDefaultImageCapacity) return Reject("file size");
        try {
            buffer.resize(static_cast<size_t>(length));
        } catch (const std::bad_alloc&) {
            return Reject("file allocation");
        }
        file.seekg(0, std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(buffer.data()), buffer.size())) {
            LOGE("Failed to read ELF file data: %s", filepath.c_str());
            return false;
        }
    } else {
        off_t entry_size = 0;
        int fd = ApkOpenEntry(filepath.c_str(), &entry_size);
        if (fd < 0) return false;
        if (entry_size <= 0 || entry_size > kDefaultImageCapacity) {
            close(fd);
            return Reject("legacy entry size");
        }
        try {
            buffer.resize(static_cast<size_t>(entry_size));
        } catch (const std::bad_alloc&) {
            close(fd);
            return Reject("legacy allocation");
        }
        size_t got = 0;
        while (got < buffer.size()) {
            ssize_t n = read(fd, buffer.data() + got, buffer.size() - got);
            if (n <= 0) break;
            got += static_cast<size_t>(n);
        }
        const int closed = close(fd);
        if (closed || got != buffer.size()) return Reject("legacy read/close");
    }
    return LoadFromMemory(buffer.data(), buffer.size());
}

bool ElfLoader::LoadFromMemory(const uint8_t* data, size_t size) {
    if (m_loaded) return Reject("image already loaded");
    m_segments.clear();
    m_exported_symbols.clear();
    m_undefined_symbols.clear();
    m_needed_libs.clear();
    m_init_array.clear();
    m_symbols.clear();
    m_relocations.clear();
    m_init_func = m_image_size = 0;
    try {
        if (!PrepareImage(data, size)) return false;
    } catch (const std::bad_alloc&) {
        return Reject("metadata allocation");
    }
    // Commit only after every consumed source range and guest destination is valid.
    for (const auto& seg : m_segments) {
        uint8_t* dest = m_guest_mem + m_load_base + seg.vaddr;
        memcpy(dest, data + seg.offset, seg.filesz);
        memset(dest + seg.filesz, 0, seg.memsz - seg.filesz);
    }
    m_loaded = true;
    LOGI("Loaded %zu segments, image size: 0x%08X; %zu exports, %zu undefined, %zu init funcs",
         m_segments.size(), m_image_size, m_exported_symbols.size(),
         m_undefined_symbols.size(), m_init_array.size());
    return true;
}

const uint8_t* ElfLoader::FileAt(const uint8_t* data, uint32_t vaddr, uint64_t size) const {
    for (const auto& seg : m_segments) {
        if (vaddr >= seg.vaddr && Contains(seg.filesz, vaddr - seg.vaddr, size))
            return data + seg.offset + (vaddr - seg.vaddr);
    }
    return nullptr;
}

bool ElfLoader::MemorySpan(uint32_t vaddr, uint64_t size, uint32_t flags) const {
    for (const auto& seg : m_segments) {
        if ((seg.flags & flags) == flags && vaddr >= seg.vaddr &&
            Contains(seg.memsz, vaddr - seg.vaddr, size)) return true;
    }
    return false;
}

bool ElfLoader::PrepareImage(const uint8_t* data, size_t size) {
    if (!data || !m_guest_mem || size < sizeof(Elf32_Ehdr) || size > kDefaultImageCapacity ||
        !m_max_image_size || m_max_image_size > kDefaultImageCapacity ||
        static_cast<uint64_t>(m_load_base) + m_max_image_size > 0x100000000ULL)
        return Reject("input/guest slot size");
    const auto eh = ReadValue<Elf32_Ehdr>(data);
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS32 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_ident[EI_VERSION] != EV_CURRENT ||
        eh.e_version != EV_CURRENT || eh.e_type != ET_DYN || eh.e_machine != EM_ARM ||
        eh.e_ehsize != sizeof(Elf32_Ehdr) || eh.e_phentsize != sizeof(Elf32_Phdr) ||
        !eh.e_phnum || !Contains(size, eh.e_phoff, uint64_t(eh.e_phnum) * sizeof(Elf32_Phdr)))
        return Reject("ELF32 ARM header/program table");

    Elf32_Phdr dynamic{};
    bool have_dynamic = false;
    uint64_t image_end = 0;
    for (uint32_t i = 0; i < eh.e_phnum; ++i) {
        const auto ph = ReadValue<Elf32_Phdr>(data + eh.e_phoff + i * sizeof(Elf32_Phdr));
        if (!Contains(size, ph.p_offset, ph.p_filesz)) return Reject("program file span");
        if (ph.p_type == PT_INTERP || ph.p_type == PT_TLS) return Reject("unsupported INTERP/TLS");
        if (ph.p_type == PT_DYNAMIC) {
            if (have_dynamic || !ph.p_filesz || ph.p_filesz > ph.p_memsz ||
                ph.p_filesz % sizeof(Elf32_Dyn) || ph.p_filesz / sizeof(Elf32_Dyn) > 4096)
                return Reject("dynamic program header");
            dynamic = ph;
            have_dynamic = true;
        }
        if (ph.p_type != PT_LOAD) continue;
        if (ph.p_filesz > ph.p_memsz || !Contains(m_max_image_size, ph.p_vaddr, ph.p_memsz) ||
            (ph.p_align > 1 && ((ph.p_align & (ph.p_align - 1)) ||
             ph.p_vaddr % ph.p_align != ph.p_offset % ph.p_align)))
            return Reject("segment size/alignment/guest slot");
        image_end = std::max(image_end, uint64_t(ph.p_vaddr) + ph.p_memsz);
        m_segments.push_back({ph.p_vaddr, ph.p_memsz, ph.p_filesz, ph.p_flags, ph.p_offset});
    }
    if (m_segments.empty() || !image_end || !have_dynamic) return Reject("missing LOAD/DYNAMIC");
    std::sort(m_segments.begin(), m_segments.end(), [](const auto& a, const auto& b) {
        return a.vaddr < b.vaddr;
    });
    for (size_t i = 1; i < m_segments.size(); ++i) {
        if (uint64_t(m_segments[i - 1].vaddr) + m_segments[i - 1].memsz > m_segments[i].vaddr)
            return Reject("overlapping load segments");
    }
    const uint64_t rounded = (image_end + 0xFFF) & ~uint64_t(0xFFF);
    if (rounded > m_max_image_size) return Reject("rounded guest image exceeds slot");
    m_image_size = static_cast<uint32_t>(rounded);
    const uint8_t* dyn = FileAt(data, dynamic.p_vaddr, dynamic.p_filesz);
    if (!dyn || dyn != data + dynamic.p_offset) return Reject("dynamic not in copied segment");

    // ponytail: bounded SysV-hash/REL subset; extend only for a verified new guest caller.
    std::map<int32_t, uint32_t> tags;
    std::vector<uint32_t> needed;
    bool terminated = false;
    for (size_t pos = 0; pos < dynamic.p_filesz; pos += sizeof(Elf32_Dyn)) {
        const auto d = ReadValue<Elf32_Dyn>(dyn + pos);
        if (d.d_tag == DT_NULL) { terminated = true; break; }
        if (d.d_tag == DT_NEEDED) { needed.push_back(d.d_un.d_val); continue; }
        switch (d.d_tag) {
            case DT_RELA: case DT_RELASZ: case DT_RELAENT:
            case 35: case 36: case 37: // RELRSZ / RELR / RELRENT
            case 0x6000000f: case 0x60000010: // Android packed REL
            case 0x60000011: case 0x60000012: // Android packed RELA
                return Reject("unsupported relocation format");
            case DT_STRTAB: case DT_STRSZ: case DT_SYMTAB: case DT_SYMENT: case DT_HASH:
            case DT_REL: case DT_RELSZ: case DT_RELENT: case DT_JMPREL: case DT_PLTRELSZ:
            case DT_PLTREL: case DT_INIT: case DT_INIT_ARRAY: case DT_INIT_ARRAYSZ:
            case DT_SONAME:
                if (!tags.emplace(d.d_tag, d.d_un.d_val).second)
                    return Reject("duplicate dynamic tag");
                break;
            default: break; // Unconsumed tags never supply a pointer to this loader.
        }
    }
    if (!terminated) return Reject("dynamic table missing DT_NULL");
    if (!tags.count(DT_STRTAB) || !tags[DT_STRSZ] || !tags.count(DT_SYMTAB) ||
        tags[DT_SYMENT] != sizeof(Elf32_Sym) || !tags.count(DT_HASH))
        return Reject("missing string/symbol/SysV hash metadata");
    const uint32_t str_size = tags[DT_STRSZ];
    const uint8_t* strings = FileAt(data, tags[DT_STRTAB], str_size);
    const uint8_t* hash = FileAt(data, tags[DT_HASH], 8);
    if (!strings || !hash) return Reject("string/hash span");
    const uint32_t buckets = ReadValue<uint32_t>(hash);
    const uint32_t sym_count = ReadValue<uint32_t>(hash + 4);
    const uint64_t hash_words = uint64_t(buckets) + sym_count;
    if (!buckets || !sym_count || sym_count > kDefaultImageCapacity / sizeof(ElfSymbol) ||
        !FileAt(data, tags[DT_HASH], (2 + hash_words) * 4)) return Reject("SysV hash extent/count");
    for (uint64_t i = 0; i < hash_words; ++i) {
        if (ReadValue<uint32_t>(hash + 8 + i * 4) >= sym_count) return Reject("SysV hash index");
    }
    const uint8_t* symbols = FileAt(data, tags[DT_SYMTAB], uint64_t(sym_count) * sizeof(Elf32_Sym));
    if (!symbols) return Reject("symbol table span");
    size_t string_bytes = 0;
    auto read_name = [&](uint32_t offset, std::string& name) {
        if (offset >= str_size) return false;
        const char* begin = reinterpret_cast<const char*>(strings + offset);
        const char* end = static_cast<const char*>(memchr(begin, 0, str_size - offset));
        if (!end || size_t(end - begin) > kDefaultImageCapacity - string_bytes) return false;
        string_bytes += size_t(end - begin);
        name.assign(begin, end);
        return true;
    };
    for (uint32_t offset : needed) {
        std::string name;
        if (!read_name(offset, name) || name.empty()) return Reject("DT_NEEDED string");
        m_needed_libs.push_back(std::move(name));
    }
    if (tags.count(DT_SONAME)) {
        std::string name;
        if (!read_name(tags[DT_SONAME], name)) return Reject("DT_SONAME string");
    }
    m_symbols.reserve(sym_count);
    for (uint32_t i = 0; i < sym_count; ++i) {
        const auto sym = ReadValue<Elf32_Sym>(symbols + uint64_t(i) * sizeof(Elf32_Sym));
        ElfSymbol out{};
        if (!read_name(sym.st_name, out.name)) return Reject("symbol name");
        out.size = sym.st_size;
        out.bind = ELF32_ST_BIND(sym.st_info);
        out.type = ELF32_ST_TYPE(sym.st_info);
        out.shndx = sym.st_shndx;
        if (sym.st_shndx != SHN_UNDEF) {
            const uint32_t address = out.type == STT_FUNC ? sym.st_value & ~1u : sym.st_value;
            if (sym.st_shndx == SHN_COMMON || out.type == STT_TLS ||
                !MemorySpan(address, sym.st_size, out.type == STT_FUNC ? PF_X : 0) ||
                uint64_t(m_load_base) + sym.st_value > UINT32_MAX)
                return Reject("defined symbol span/type");
            // Keep the frozen images' image-relative linker boundary symbols too (SHN_ABS).
            out.value = m_load_base + sym.st_value;
        }
        if (!out.name.empty()) {
            if (out.shndx == SHN_UNDEF) m_undefined_symbols.push_back(out.name);
            else m_exported_symbols[out.name] = out;
        }
        m_symbols.push_back(std::move(out));
    }

    m_init_func = tags[DT_INIT];
    if (m_init_func && m_init_func != UINT32_MAX && !MemorySpan(m_init_func & ~1u, 1, PF_X))
        return Reject("DT_INIT executable address");
    uint32_t init_address = 0, init_size = 0;
    if (tags.count(DT_INIT_ARRAY) || tags.count(DT_INIT_ARRAYSZ)) {
        if (!tags.count(DT_INIT_ARRAY) || !tags.count(DT_INIT_ARRAYSZ))
            return Reject("incomplete init array metadata");
        init_address = tags[DT_INIT_ARRAY];
        init_size = tags[DT_INIT_ARRAYSZ];
        const uint8_t* init = FileAt(data, init_address, init_size);
        if (init_size % 4 || !init) return Reject("init array span");
        for (uint32_t i = 0; i < init_size; i += 4) {
            const uint32_t value = ReadValue<uint32_t>(init + i);
            if (!value || value == UINT32_MAX) continue;
            if (!MemorySpan(value & ~1u, 1, PF_X)) return Reject("init array executable address");
            m_init_array.push_back(m_load_base + value);
        }
    }

    auto read_relocations = [&](int32_t address_tag, int32_t size_tag) {
        if (!tags.count(address_tag) && !tags.count(size_tag)) return true;
        if (!tags.count(address_tag) || !tags.count(size_tag) ||
            tags[DT_RELENT] != sizeof(Elf32_Rel) ||
            (address_tag == DT_JMPREL && tags[DT_PLTREL] != DT_REL)) return false;
        const uint32_t length = tags[size_tag];
        const uint8_t* rels = FileAt(data, tags[address_tag], length);
        if (length % sizeof(Elf32_Rel) || !rels) return false;
        for (uint32_t pos = 0; pos < length; pos += sizeof(Elf32_Rel)) {
            const auto rel = ReadValue<Elf32_Rel>(rels + pos);
            const uint32_t type = ELF32_R_TYPE(rel.r_info);
            const uint32_t symbol = ELF32_R_SYM(rel.r_info);
            if (symbol >= m_symbols.size()) return false;
            if (type == R_ARM_NONE) continue;
            if (type != R_ARM_RELATIVE && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT &&
                type != R_ARM_ABS32 && type != R_ARM_REL32) return false;
            if ((type == R_ARM_RELATIVE && symbol) || (rel.r_offset & 3) ||
                !MemorySpan(rel.r_offset, 4, PF_W)) return false;
            if (init_size && rel.r_offset >= init_address &&
                uint64_t(rel.r_offset) < uint64_t(init_address) + init_size && type != R_ARM_RELATIVE)
                return false; // Initializers in this supported subset are image-relative.
            m_relocations.push_back(rel);
        }
        return true;
    };
    if (!read_relocations(DT_REL, DT_RELSZ) || !read_relocations(DT_JMPREL, DT_PLTRELSZ))
        return Reject("relocation format/table/index/writable target");
    return true;
}

bool ElfLoader::Relocate(const SymbolResolver& resolver) {
    if (!m_loaded || m_relocated || !resolver) {
        LOGE("Relocate requires one validated, unrelocated image and a resolver");
        return false;
    }
    // Tables and names are immutable host copies; a relocation cannot rewrite a later entry.
    for (const auto& rel : m_relocations) {
        const uint32_t symbol = ELF32_R_SYM(rel.r_info);
        const auto& sym = m_symbols[symbol];
        const uint32_t value = !symbol ? 0 : sym.shndx != SHN_UNDEF ? sym.value : resolver(sym.name);
        uint8_t* target = m_guest_mem + m_load_base + rel.r_offset;
        uint32_t word = ReadValue<uint32_t>(target);
        switch (ELF32_R_TYPE(rel.r_info)) {
            case R_ARM_RELATIVE: word += m_load_base; break;
            case R_ARM_GLOB_DAT: case R_ARM_JUMP_SLOT: word = value; break;
            case R_ARM_ABS32: word += value; break;
            case R_ARM_REL32: word += value - (m_load_base + rel.r_offset); break;
            default: return false; // PrepareImage rejects unsupported types before any copy.
        }
        memcpy(target, &word, sizeof(word));
    }
    m_relocated = true;
    LOGI("Relocations completed successfully (%zu validated entries)!", m_relocations.size());
    return true;
}

uint32_t ElfLoader::GetSymbolAddress(const std::string& name) const {
    auto it = m_exported_symbols.find(name);
    return it != m_exported_symbols.end() ? it->second.value : 0;
}
