#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <elf.h>

struct ElfSegment {
    uint32_t vaddr;
    uint32_t memsz;
    uint32_t filesz;
    uint32_t flags;
    uint32_t offset;
};

struct ElfSymbol {
    std::string name;
    uint32_t value;
    uint32_t size;
    uint8_t bind;
    uint8_t type;
    uint16_t shndx;
};

using SymbolResolver = std::function<uint32_t(const std::string& name)>;

class ElfLoader {
public:
    static constexpr uint32_t kDefaultImageCapacity = 0x04000000;
    ElfLoader(uint8_t* guest_memory_base, uint32_t load_base_addr = 0x40000000,
              uint32_t max_image_size = kDefaultImageCapacity);
    ~ElfLoader();

    bool Load(const std::string& filepath);
    bool LoadFromMemory(const uint8_t* data, size_t size);
    bool Relocate(const SymbolResolver& resolver);

    uint32_t GetSymbolAddress(const std::string& name) const;
    uint32_t GetBaseAddress() const { return m_load_base; }
    uint32_t GetImageSize() const { return m_image_size; }
    uint32_t GetInitFunction() const {
        return (m_init_func && m_init_func != 0xFFFFFFFFu) ? m_load_base + m_init_func : 0;
    }
    const std::vector<uint32_t>& GetInitArray() const { return m_init_array; }
    const std::vector<std::string>& GetNeededLibraries() const { return m_needed_libs; }
    const std::vector<std::string>& GetUndefinedSymbols() const { return m_undefined_symbols; }

private:
    bool PrepareImage(const uint8_t* data, size_t size);
    const uint8_t* FileAt(const uint8_t* data, uint32_t vaddr, uint64_t size) const;
    bool MemorySpan(uint32_t vaddr, uint64_t size, uint32_t flags = 0) const;

    uint8_t* m_guest_mem;
    uint32_t m_load_base;
    uint32_t m_max_image_size;
    uint32_t m_image_size = 0;
    bool m_loaded = false;
    bool m_relocated = false;

    std::vector<ElfSegment> m_segments;
    std::unordered_map<std::string, ElfSymbol> m_exported_symbols;
    std::vector<std::string> m_undefined_symbols;
    std::vector<std::string> m_needed_libs;
    std::vector<uint32_t> m_init_array;
    uint32_t m_init_func = 0;

    // Own validated metadata: neither source unmap nor guest relocation can alter it.
    std::vector<ElfSymbol> m_symbols;
    std::vector<Elf32_Rel> m_relocations;
};
