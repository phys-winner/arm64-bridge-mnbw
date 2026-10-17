"""Check relocation-backed strong guest imports against bridge providers.

Usage:
    python check_symbols.py <guest.so> [more_guest.so ...] [--src <loader_src_dir>]

Only undefined dynamic symbols referenced by an ELF32 `SHT_REL` relocation are
reported. Defined symbols in any supplied guest library and literal registrations
in the bridge source cover an import. This is a coverage check, not a replacement
for the target loader's own dependency-order and ABI validation.
"""

import glob
import os
import re
import struct
import sys


ELF32_HEADER_SIZE = 52
ELF32_SECTION_HEADER_SIZE = 40
ELF32_SYMBOL_SIZE = 16
ELF32_REL_SIZE = 8

EI_CLASS = 4
EI_DATA = 5
ELFCLASS32 = 1
ELFDATA2LSB = 1
EM_ARM = 40

SHN_UNDEF = 0
SHT_RELA = 4
SHT_REL = 9
SHT_DYNSYM = 11
STB_WEAK = 2
STB_GLOBAL = 1
SUPPORTED_ARM_RELOCATIONS = {0, 2, 3, 21, 22, 23}


def checked_slice(data, offset, size, label):
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise ValueError("%s is outside the ELF file" % label)
    return data[offset:offset + size]


def read_c_string(data, offset, label):
    if offset < 0 or offset >= len(data):
        raise ValueError("%s string offset is outside its table" % label)
    terminator = data.find(b"\0", offset)
    if terminator < 0:
        raise ValueError("%s string is unterminated" % label)
    return data[offset:terminator].decode("latin1", "ignore")


def get_symbols(path):
    """Return (defined exports, relocation-backed strong imports) from an ELF32 ARM guest."""
    with open(path, "rb") as stream:
        elf = stream.read()

    if len(elf) < ELF32_HEADER_SIZE or elf[:4] != b"\x7fELF":
        raise ValueError("%s is not an ELF file" % path)
    if elf[EI_CLASS] != ELFCLASS32 or elf[EI_DATA] != ELFDATA2LSB:
        raise ValueError("%s is not little-endian ELF32" % path)
    machine, = struct.unpack_from("<H", elf, 18)
    if machine != EM_ARM:
        raise ValueError("%s is not an ARM ELF" % path)

    section_offset, = struct.unpack_from("<I", elf, 32)
    section_entry_size, = struct.unpack_from("<H", elf, 46)
    section_count, = struct.unpack_from("<H", elf, 48)
    if section_entry_size < ELF32_SECTION_HEADER_SIZE:
        raise ValueError("%s has invalid ELF32 section headers" % path)
    checked_slice(
        elf,
        section_offset,
        section_entry_size * section_count,
        "section-header table",
    )

    sections = []
    for index in range(section_count):
        offset = section_offset + index * section_entry_size
        fields = struct.unpack_from("<IIIIIIIIII", elf, offset)
        sections.append(
            {
                "type": fields[1],
                "offset": fields[4],
                "size": fields[5],
                "link": fields[6],
                "entry_size": fields[9],
            }
        )

    dynsym_index = next(
        (index for index, section in enumerate(sections) if section["type"] == SHT_DYNSYM),
        None,
    )
    if dynsym_index is None:
        return set(), set()

    dynsym = sections[dynsym_index]
    if dynsym["link"] >= len(sections):
        raise ValueError("%s has an invalid dynamic string-table link" % path)
    dynstr = sections[dynsym["link"]]
    if dynsym["entry_size"] not in (0, ELF32_SYMBOL_SIZE):
        raise ValueError("%s has unsupported ELF32 dynamic-symbol entries" % path)
    if dynsym["size"] % ELF32_SYMBOL_SIZE:
        raise ValueError("%s has a truncated dynamic-symbol table" % path)

    dynsym_data = checked_slice(elf, dynsym["offset"], dynsym["size"], ".dynsym")
    dynstr_data = checked_slice(elf, dynstr["offset"], dynstr["size"], ".dynstr")
    symbols = []
    exports = set()
    for offset in range(0, len(dynsym_data), ELF32_SYMBOL_SIZE):
        name_offset, _value, _size, info, _other, section_index = struct.unpack_from(
            "<IIIBBH", dynsym_data, offset
        )
        name = "" if name_offset == 0 else read_c_string(dynstr_data, name_offset, ".dynstr")
        binding = info >> 4
        symbols.append((name, binding, section_index))
        if name and section_index != SHN_UNDEF and binding in (STB_GLOBAL, STB_WEAK):
            exports.add(name)

    imports = set()
    for section in sections:
        if section["type"] == SHT_RELA:
            raise ValueError("%s uses unsupported SHT_RELA relocations" % path)
        if section["type"] != SHT_REL:
            continue
        if section["link"] != dynsym_index:
            continue
        if section["entry_size"] not in (0, ELF32_REL_SIZE):
            raise ValueError("%s has unsupported ELF32 relocation entries" % path)
        if section["size"] % ELF32_REL_SIZE:
            raise ValueError("%s has a truncated relocation table" % path)
        relocation_data = checked_slice(elf, section["offset"], section["size"], "relocation table")
        for offset in range(0, len(relocation_data), ELF32_REL_SIZE):
            _target, info = struct.unpack_from("<II", relocation_data, offset)
            relocation_type = info & 0xFF
            if relocation_type not in SUPPORTED_ARM_RELOCATIONS:
                raise ValueError(
                    "%s uses unsupported ARM relocation type %d" % (path, relocation_type)
                )
            symbol_index = info >> 8
            if symbol_index == 0:
                continue
            if symbol_index >= len(symbols):
                raise ValueError("%s relocation references an invalid dynamic symbol" % path)
            name, binding, section_index = symbols[symbol_index]
            if name and section_index == SHN_UNDEF and binding != STB_WEAK:
                imports.add(name)

    return exports, imports


def strip_cpp_comments(source):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.DOTALL)


def registered_thunks(src_dir):
    names = set()
    patterns = (
        r"\bRegisterThunk\s*\(\s*\"([^\"]+)\"",
        r"\bRegisterSymbol\s*\(\s*\"([^\"]+)\"",
        r"\bRegisterDataSymbol\s*\(\s*\"([^\"]+)\"",
        r"\b(?:register|define)_[A-Za-z0-9_]+\s*\(\s*\"([^\"]+)\"",
        r"\bAllocAndRegisterIID\s*\([^,]+\s*,\s*[^,]+\s*,\s*\"([^\"]+)\"",
        r"\b(?:ctype_fn|unary_float|double_fn|define)\s*\(\s*\"([^\"]+)\"",
    )
    helper_pattern = re.compile(
        r"\bauto\s+(\w+)\s*=\s*\[[^\]]*\]\s*\(\s*"
        r"(?:const\s+)?(?:char\s*\*|std::string(?:\s*&)?)(?:\s+)(\w+)[^)]*\)\s*"
        r"\{(.*?)\};",
        re.DOTALL,
    )
    for path in glob.glob(os.path.join(src_dir, "*.cpp")):
        with open(path, "r", encoding="utf-8", errors="ignore") as stream:
            source = strip_cpp_comments(stream.read())
        for pattern in patterns:
            names.update(re.findall(pattern, source))
        for helper in helper_pattern.finditer(source):
            helper_name, name_parameter, body = helper.groups()
            provider_call = r"\b(?:RegisterThunk|RegisterSymbol|RegisterDataSymbol)\s*\(\s*" + re.escape(name_parameter) + r"\b"
            if re.search(provider_call, body):
                names.update(
                    re.findall(
                        r"\b" + re.escape(helper_name) + r"\s*\(\s*\"([^\"]+)\"",
                        source,
                    )
                )
        for literals in re.findall(
            r"\bfor\s*\([^)]*:\s*\{([^}]*)\}\)\s*RegisterThunk\s*\(\s*\w+",
            source,
            flags=re.DOTALL,
        ):
            names.update(re.findall(r'\"([^\"]+)\"', literals))
    return names


def main(argv):
    src_dir = "src"
    libraries = []
    index = 0
    while index < len(argv):
        if argv[index] == "--src" and index + 1 < len(argv):
            src_dir = argv[index + 1]
            index += 2
        else:
            libraries.append(argv[index])
            index += 1

    if not libraries:
        print(__doc__)
        return 2

    all_exports = set()
    per_library = []
    try:
        for library in libraries:
            exports, imports = get_symbols(library)
            all_exports |= exports
            per_library.append((library, imports))
            print(
                "%-40s exports %5d  relocation-backed strong imports %5d"
                % (os.path.basename(library), len(exports), len(imports))
            )
    except (OSError, ValueError) as error:
        print("ELF input error: %s" % error, file=sys.stderr)
        return 2

    thunks = registered_thunks(src_dir)
    print("\nDirectly registered providers in %s: %d" % (src_dir, len(thunks)))

    covered = thunks | all_exports
    total_missing = 0
    for library, imports in per_library:
        missing = sorted(imports - covered)
        total_missing += len(missing)
        print("\n--- %s: %d unhandled import(s) ---" % (os.path.basename(library), len(missing)))
        for name in missing:
            print("  [MISSING]", name)

    print("\nTotal unhandled: %d" % total_missing)
    return 1 if total_missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
