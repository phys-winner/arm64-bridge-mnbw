import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("check_symbols", ROOT / "check_symbols.py")
check_symbols = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(check_symbols)


def make_elf32(relocation_symbol_indexes=(1, 2), relocation_type=check_symbols.SHT_REL,
               arm_relocation_type=22):
    dynstr = b"\0needed\0weak_needed\0provided\0unused\0"
    name_needed = dynstr.index(b"needed")
    name_weak = dynstr.index(b"weak_needed")
    name_provided = dynstr.index(b"provided")
    name_unused = dynstr.index(b"unused")
    dynsym = b"".join(
        (
            b"\0" * 16,
            struct.pack("<IIIBBH", name_needed, 0, 0, 0x12, 0, 0),
            struct.pack("<IIIBBH", name_weak, 0, 0, 0x22, 0, 0),
            struct.pack("<IIIBBH", name_provided, 0, 0, 0x12, 0, 1),
            struct.pack("<IIIBBH", name_unused, 0, 0, 0x12, 0, 0),
        )
    )
    relocations = b"".join(
        struct.pack("<II", 0x1000 + index * 4, symbol_index << 8 | arm_relocation_type)
        for index, symbol_index in enumerate(relocation_symbol_indexes)
    )
    dynstr_offset = 0x80
    dynsym_offset = 0xC0
    relocation_offset = 0x140
    section_offset = 0x200
    elf = bytearray(section_offset + 4 * check_symbols.ELF32_SECTION_HEADER_SIZE)
    ident = b"\x7fELF\x01\x01\x01" + b"\0" * 9
    struct.pack_into(
        "<16sHHIIIIIHHHHHH",
        elf,
        0,
        ident,
        3,
        check_symbols.EM_ARM,
        1,
        0,
        0,
        section_offset,
        0,
        check_symbols.ELF32_HEADER_SIZE,
        0,
        0,
        check_symbols.ELF32_SECTION_HEADER_SIZE,
        4,
        0,
    )
    elf[dynstr_offset:dynstr_offset + len(dynstr)] = dynstr
    elf[dynsym_offset:dynsym_offset + len(dynsym)] = dynsym
    elf[relocation_offset:relocation_offset + len(relocations)] = relocations
    section = lambda index, values: struct.pack_into(
        "<IIIIIIIIII", elf, section_offset + index * check_symbols.ELF32_SECTION_HEADER_SIZE, *values
    )
    section(0, (0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
    section(1, (0, 3, 0, 0, dynstr_offset, len(dynstr), 0, 0, 1, 0))
    section(2, (0, check_symbols.SHT_DYNSYM, 0, 0, dynsym_offset, len(dynsym), 1, 0, 4, 16))
    section(3, (0, relocation_type, 0, 0, relocation_offset, len(relocations), 2, 0, 4, 8))
    return bytes(elf)


class CheckSymbolsTests(unittest.TestCase):
    def elf_file(self, data):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "guest.so"
        path.write_bytes(data)
        return path

    def test_uses_only_relocation_backed_strong_imports(self):
        exports, imports = check_symbols.get_symbols(self.elf_file(make_elf32()))
        self.assertEqual(exports, {"provided"})
        self.assertEqual(imports, {"needed"})

    def test_rejects_invalid_relocation_symbol(self):
        with self.assertRaisesRegex(ValueError, "invalid dynamic symbol"):
            check_symbols.get_symbols(self.elf_file(make_elf32((9,))))

    def test_rejects_rela_not_supported_by_loader(self):
        with self.assertRaisesRegex(ValueError, "SHT_RELA"):
            check_symbols.get_symbols(self.elf_file(make_elf32((1,), check_symbols.SHT_RELA)))

    def test_rejects_unsupported_arm_relocation(self):
        with self.assertRaisesRegex(ValueError, "unsupported ARM relocation type 99"):
            check_symbols.get_symbols(self.elf_file(make_elf32((1,), arm_relocation_type=99)))

    def test_finds_functions_data_and_opensl_iids_without_comments(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "providers.cpp"
            source.write_text(
                "// RegisterThunk(\"comment\", ignored)\n"
                "RegisterThunk(\"function\", handler);\n"
                "RegisterSymbol(\"data\", address);\n"
                "RegisterDataSymbol(\"other_data\", address);\n"
                "register_helper(\"helper_function\", handler);\n"
                "define_data(\"helper_data\", size);\n"
                "auto wrapped = [&](const char* name, auto handler) { RegisterThunk(name, handler); };\n"
                "wrapped(\"wrapped_function\", handler);\n"
                "AllocAndRegisterIID(manager, host, \"SL_IID_ENGINE\", value);\n",
                encoding="utf-8",
            )
            self.assertEqual(
                check_symbols.registered_thunks(directory),
                {"function", "data", "other_data", "helper_function", "helper_data", "wrapped_function", "SL_IID_ENGINE"},
            )


if __name__ == "__main__":
    unittest.main()
