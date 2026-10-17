#include "russian_text_processor.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using warband::localization::ContainsRussianTextMarkup;
using warband::localization::GuestRussianTextStatus;
using warband::localization::ProcessGuestRussianText;
using warband::localization::ProcessRussianText;

int failures = 0;

void Fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

void ExpectText(std::string_view input, std::string_view expected,
                bool expected_markup = true) {
    const auto result = ProcessRussianText(input);
    if (result.text != expected) {
        std::cerr << "FAIL: input:    " << input << "\n"
                  << "      expected: " << expected << "\n"
                  << "      actual:   " << result.text << "\n";
        ++failures;
    }
    if (result.had_markup != expected_markup) Fail("had_markup mismatch");
    if (ContainsRussianTextMarkup(result.text)) Fail("processed text retains official markup");
}

bool IsValidUtf8(std::string_view text) {
    for (std::size_t cursor = 0; cursor < text.size();) {
        const auto first = static_cast<unsigned char>(text[cursor]);
        if (first < 0x80) {
            ++cursor;
            continue;
        }
        unsigned count = 0;
        std::uint32_t value = 0;
        if ((first & 0xE0) == 0xC0) {
            count = 2;
            value = first & 0x1F;
        } else if ((first & 0xF0) == 0xE0) {
            count = 3;
            value = first & 0x0F;
        } else if ((first & 0xF8) == 0xF0) {
            count = 4;
            value = first & 0x07;
        } else {
            return false;
        }
        if (count > text.size() - cursor) return false;
        for (unsigned i = 1; i < count; ++i) {
            const auto next = static_cast<unsigned char>(text[cursor + i]);
            if ((next & 0xC0) != 0x80) return false;
            value = (value << 6) | (next & 0x3F);
        }
        if ((count == 2 && value < 0x80) || (count == 3 && value < 0x800) ||
            (count == 4 && value < 0x10000) || value > 0x10FFFF ||
            (value >= 0xD800 && value <= 0xDFFF)) {
            return false;
        }
        cursor += count;
    }
    return true;
}

void TestPluralMarkers() {
    ExpectText("ordinary text", "ordinary text", false);
    ExpectText("@I[25] @L[человек;;а;]", "25 человек");
    ExpectText("@I[1] @L[динар;;а;ов]", "1 динар");
    ExpectText("@I[2] @L[динар;;а;ов]", "2 динара");
    ExpectText("@I[4] @L[динар;;а;ов]", "4 динара");
    ExpectText("@I[5] @L[динар;;а;ов]", "5 динаров");
    ExpectText("@I[11] @L[динар;;а;ов]", "11 динаров");
    ExpectText("@I[12] @L[динар;;а;ов]", "12 динаров");
    ExpectText("@I[14] @L[динар;;а;ов]", "14 динаров");
    ExpectText("@I[21] @L[динар;;а;ов]", "21 динар");
    ExpectText("@I[22] @L[динар;;а;ов]", "22 динара");
    ExpectText("@I[25] @L[динар;;а;ов]", "25 динаров");
    ExpectText("@I[-22] @L[динар;;а;ов]", "-22 динара");
    ExpectText("@I[101] @L[динар;;а;ов]", "101 динар");
    ExpectText("@I[111] @L[динар;;а;ов]", "111 динаров");
    ExpectText("@L+[нуж;ен;ны;ны] @I[2] @L[пленник;;а;ов]",
               "нужны 2 пленника");
    ExpectText("@I[2] @L-[бо;ец;йца;йцов], @L-[готов;ый;ых;ых]",
               "2 бойца, готовых");
    ExpectText("@L+[ваш;его;их;их] @I[1]", "вашего 1");
}

void TestCaseAndAgreementMarkers() {
    ExpectText("@Pi[Красная деревня]", "Красная деревня");
    ExpectText("@Pr[Красная деревня]", "Красной деревни");
    ExpectText("@Pd[Короткий меч]", "Короткому мечу");
    ExpectText("@Pv[Красная деревня]", "Красную деревню");
    ExpectText("@Pt[Короткий меч]", "Коротким мечом");
    ExpectText("@Pp[Красная деревня]", "Красной деревне");
    ExpectText("@Pr[Король Харлаус]", "Короля Харлауса");
    ExpectText("@Pt[Леди Изолла де Суно]", "Леди Изоллой де Суно");
    ExpectText("@Pr[Замок Альтриак]", "Замка Альтриака");
    ExpectText("@Pd[Боец]", "Бойцу");
    ExpectText("@Pt[Боец]", "Бойцом");
    ExpectText("@Pp[Боец]", "Бойце");

    ExpectText("@R+[Прост;ой;ая;ое;ые] @Cl[@Pi[Дубинка]]",
               "Простая дубинка");
    ExpectText("@Pi[Королевство Свадия] @R-[пал;;а;о;и]", 
               "Королевство Свадия пало");
    ExpectText("@R[мо;им;ей;им;ими] @Pt[Леди Изолла]",
               "моей Леди Изоллой");
    ExpectText("@R-[мо;его;ю;его;их] @Pv[Боец]", "моего Бойца");

    // Structural recovery for the one official @Pr token that is visibly a
    // mistyped four-field @R marker (the plural field is absent in the CSV).
    ExpectText("@Pi[Король] @Pr[сказал;;а;о]", "Король сказал");
    ExpectText("@Pi[Деревня] @Pr[сказал;;а;о]", "Деревня сказала");
    ExpectText("@Pi[Войска] @Pr[сказал;;а;о]", "Войска сказали");

    ExpectText("@Pi[Деревня] @R-[был;;а;о;и] @#+@Pd[Король]@#- "
               "@R-[разорен;;а;о;ы]",
               "Деревня была Королю разорена");
}

void TestLowercaseAndMalformedInput() {
    ExpectText("@Cl[Ёлка]", "ёлка");
    ExpectText("@Cl[Sword]", "sword");
    ExpectText("@Cl[  Деревня]", "  деревня");
    ExpectText("Похоже, @Cl[Деревня]] еще не заметили", "Похоже, деревня еще не заметили");
    ExpectText("email@example.org @unknown[x]", "email@example.org @unknown[x]", false);

    const std::array<std::string_view, 5> malformed = {
        "@L[динар;;а", "@R[нуж;ен;на", "@Pr[Деревня", "@Cl[@Pi[Деревня]",
        "@#+text@#-@#-",
    };
    for (const std::string_view input : malformed) {
        const auto result = ProcessRussianText(input);
        if (!result.had_markup || !result.used_fallback) Fail("malformed marker did not report fallback");
        if (ContainsRussianTextMarkup(result.text)) Fail("malformed marker leaked to output");
    }

    std::string nested = "Текст";
    for (unsigned i = 0; i < 80; ++i) nested = "@Cl[" + nested + "]";
    const auto deep = ProcessRussianText(nested);
    if (ContainsRussianTextMarkup(deep.text) || !deep.used_fallback) {
        Fail("nesting limit did not fail closed");
    }
}

void WriteLe32(std::span<std::uint8_t> memory, std::size_t offset, std::uint32_t value) {
    memory[offset] = static_cast<std::uint8_t>(value);
    memory[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    memory[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    memory[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

std::uint32_t ReadLe32(std::span<const std::uint8_t> memory, std::size_t offset) {
    return static_cast<std::uint32_t>(memory[offset]) |
           (static_cast<std::uint32_t>(memory[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(memory[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(memory[offset + 3]) << 24);
}

void PutString(std::span<std::uint8_t> memory, std::uint32_t object,
               std::uint32_t data, std::uint32_t capacity, std::string_view text) {
    WriteLe32(memory, object + 4, data);
    WriteLe32(memory, object + 8, capacity);
    WriteLe32(memory, object + 12, static_cast<std::uint32_t>(text.size()));
    std::copy(text.begin(), text.end(), memory.begin() + data);
    memory[data + text.size()] = 0;
}

void TestGuestStringBridge() {
    std::vector<std::uint8_t> memory(512, 0xA5);
    constexpr std::uint32_t object = 32;
    constexpr std::uint32_t data = object + 16;
    const std::string input = "@I[25] @L[человек;;а;]";
    PutString(memory, object, data, 100, input);
    bool fallback = true;
    if (ProcessGuestRussianText(memory, object, &fallback) != GuestRussianTextStatus::Processed) {
        Fail("inline rglString was not processed");
    }
    const std::string expected = "25 человек";
    if (ReadLe32(memory, object + 4) != data || ReadLe32(memory, object + 8) != 100 ||
        ReadLe32(memory, object + 12) != expected.size() ||
        std::string_view(reinterpret_cast<const char*>(memory.data() + data), expected.size()) != expected ||
        memory[data + expected.size()] != 0 || fallback) {
        Fail("inline rglString fields/content mismatch");
    }

    memory.assign(512, 0xA5);
    PutString(memory, object, 300, 100, "@Cl[Деревня]");
    if (ProcessGuestRussianText(memory, object) != GuestRussianTextStatus::Processed ||
        std::string(reinterpret_cast<const char*>(memory.data() + 300)) != "деревня") {
        Fail("external rglString was not processed");
    }

    memory.assign(512, 0xA5);
    PutString(memory, object, data, 100, "plain");
    const auto before_plain = memory;
    if (ProcessGuestRussianText(memory, object) != GuestRussianTextStatus::NoMarkup ||
        memory != before_plain) {
        Fail("no-markup rglString changed");
    }

    if (ProcessGuestRussianText(memory, 0) != GuestRussianTextStatus::InvalidObject ||
        ProcessGuestRussianText(memory, 505) != GuestRussianTextStatus::InvalidObject) {
        Fail("invalid rglString object accepted");
    }

    memory.assign(512, 0xA5);
    WriteLe32(memory, object + 4, 0);
    WriteLe32(memory, object + 8, 10);
    WriteLe32(memory, object + 12, 1);
    const auto before_null = memory;
    if (ProcessGuestRussianText(memory, object) != GuestRussianTextStatus::InvalidData ||
        memory != before_null) {
        Fail("null data pointer accepted or memory changed");
    }

    memory.assign(512, 0xA5);
    PutString(memory, object, data, 4, "@I[1]");
    const auto before_capacity = memory;
    if (ProcessGuestRussianText(memory, object) != GuestRussianTextStatus::InvalidData ||
        memory != before_capacity) {
        Fail("length greater than capacity accepted or memory changed");
    }

    memory.assign(512, 0xA5);
    PutString(memory, object, data, 100, "@I[1]");
    memory[data + 5] = 0x7F;
    const auto before_terminator = memory;
    if (ProcessGuestRussianText(memory, object) != GuestRussianTextStatus::InvalidData ||
        memory != before_terminator) {
        Fail("unterminated rglString accepted or memory changed");
    }

    memory.assign(512, 0xA5);
    PutString(memory, object, data, 5, "@I[1]");
    const auto before_missing_nul_capacity = memory;
    if (ProcessGuestRussianText(memory, object) != GuestRussianTextStatus::InvalidData ||
        memory != before_missing_nul_capacity) {
        Fail("capacity without room for NUL accepted or memory changed");
    }

    memory.assign(512, 0xA5);
    WriteLe32(memory, object + 4, object + 8);
    WriteLe32(memory, object + 8, 0);
    WriteLe32(memory, object + 12, 0);
    const auto before_alias = memory;
    if (ProcessGuestRussianText(memory, object) != GuestRussianTextStatus::InvalidData ||
        memory != before_alias) {
        Fail("header-aliasing data pointer accepted or memory changed");
    }
}

void TestDeterministicRobustness() {
    static constexpr std::array<std::string_view, 18> pieces = {
        "x", " ", "[", "]", ";", "25", "Деревня", "@I[", "@L[", "@L+[",
        "@L-[", "@Pi[", "@Pr[", "@R[", "@R+[", "@R-[", "@#+", "@#-",
    };
    std::uint32_t random = 0xC001D00Du;
    auto next = [&]() {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        return random;
    };
    for (unsigned iteration = 0; iteration < 10000; ++iteration) {
        std::string input;
        const unsigned count = next() % 32;
        for (unsigned i = 0; i < count; ++i) input.append(pieces[next() % pieces.size()]);
        const auto first = ProcessRussianText(input);
        if (ContainsRussianTextMarkup(first.text)) {
            Fail("deterministic malformed-input sweep leaked markup");
            return;
        }
        const auto second = ProcessRussianText(first.text);
        if (second.text != first.text || second.had_markup) {
            Fail("processor is not idempotent after marker removal");
            return;
        }
        if (first.text.size() > input.size() * 4 + 32) {
            Fail("malformed-input sweep produced unreasonable growth");
            return;
        }
    }

    // Random guest headers must always fail safely or describe a valid rewrite;
    // this sweep is also run under AddressSanitizer in the host test command.
    std::vector<std::uint8_t> arena(512);
    for (unsigned iteration = 0; iteration < 10000; ++iteration) {
        for (std::uint8_t& byte : arena) byte = static_cast<std::uint8_t>(next());
        const std::uint32_t object = next() % 600;
        (void)ProcessGuestRussianText(arena, object);
    }
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

static constexpr std::array<std::string_view, 16> kMarkerPrefixes = {
    "@#+", "@#-", "@Cl[", "@L+[", "@L-[", "@R+[", "@R-[", "@Pi[",
    "@Pr[", "@Pd[", "@Pv[", "@Pt[", "@Pp[", "@I[", "@L[", "@R[",
};

std::size_t CountMarkers(std::string_view text,
                         std::array<std::size_t, kMarkerPrefixes.size()>* families = nullptr) {
    std::size_t count = 0;
    for (std::size_t cursor = 0; cursor < text.size(); ++cursor) {
        for (std::size_t family = 0; family < kMarkerPrefixes.size(); ++family) {
            const std::string_view prefix = kMarkerPrefixes[family];
            if (prefix.size() <= text.size() - cursor &&
                text.compare(cursor, prefix.size(), prefix) == 0) {
                ++count;
                if (families) ++(*families)[family];
                break;
            }
        }
    }
    return count;
}

void TestOfficialCorpus(const std::filesystem::path& root) {
    const std::array<std::filesystem::path, 2> russian_roots = {
        root / "app/assets/languages/ru",
        root / "app/assets/Modules/Native/languages/ru",
    };
    std::size_t files = 0;
    std::size_t marked_lines = 0;
    std::size_t markers = 0;
    std::array<std::size_t, kMarkerPrefixes.size()> families{};
    for (const auto& directory : russian_roots) {
        if (!std::filesystem::is_directory(directory)) {
            Fail("Russian corpus directory missing");
            continue;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".csv") continue;
            ++files;
            const std::string contents = ReadFile(entry.path());
            std::size_t start = 0;
            while (start <= contents.size()) {
                const std::size_t newline = contents.find('\n', start);
                const std::string_view line(contents.data() + start,
                    (newline == std::string::npos ? contents.size() : newline) - start);
                const std::size_t line_markers = CountMarkers(line, &families);
                if (line_markers != 0) {
                    ++marked_lines;
                    markers += line_markers;
                    const auto result = ProcessRussianText(line);
                    if (ContainsRussianTextMarkup(result.text)) {
                        std::cerr << "FAIL: corpus markup survived in " << entry.path() << '\n';
                        ++failures;
                    }
                    if (!IsValidUtf8(result.text)) {
                        std::cerr << "FAIL: corpus output is not UTF-8 in " << entry.path() << '\n';
                        ++failures;
                    }
                    // rglString already owns at least the source length.  This
                    // corpus-wide invariant proves every official row can be
                    // rewritten in place without allocating guest memory.
                    if (result.text.size() > line.size()) {
                        std::cerr << "FAIL: corpus output grew in " << entry.path()
                                  << " input=" << line.size()
                                  << " output=" << result.text.size() << '\n';
                        ++failures;
                    }
                }
                if (newline == std::string::npos) break;
                start = newline + 1;
            }
        }
    }
    if (files != 19 || marked_lines != 2150 || markers != 3895) {
        std::cerr << "FAIL: corpus coverage unexpectedly small files=" << files
                  << " lines=" << marked_lines << " markers=" << markers << '\n';
        ++failures;
    }
    static constexpr std::array<std::size_t, kMarkerPrefixes.size()> expected_families = {
        10, 10, 147, 10, 6, 114, 124, 172, 763, 242, 613, 308, 256, 558, 560, 2,
    };
    if (families != expected_families) {
        Fail("official marker-family inventory changed or was not scanned completely");
    }

    static constexpr std::array<std::string_view, 11> other_languages = {
        "cns", "cnt", "cz", "de", "en", "es", "fr", "hu", "ja", "pl", "tr",
    };
    for (const std::string_view language : other_languages) {
        const std::array<std::filesystem::path, 2> language_roots = {
            root / "app/assets/languages" / language,
            root / "app/assets/Modules/Native/languages" / language,
        };
        for (const auto& directory : language_roots) {
            if (!std::filesystem::is_directory(directory)) continue;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".csv") continue;
                if (ContainsRussianTextMarkup(ReadFile(entry.path()))) {
                    std::cerr << "FAIL: Russian marker unexpectedly occurs in "
                              << entry.path() << '\n';
                    ++failures;
                }
            }
        }
    }
    std::cout << "corpus_files=" << files << " marked_lines=" << marked_lines
              << " markers=" << markers << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    TestPluralMarkers();
    TestCaseAndAgreementMarkers();
    TestLowercaseAndMalformedInput();
    TestGuestStringBridge();
    TestDeterministicRobustness();
    if (argc != 2) {
        Fail("usage: russian_text_processor_test <repository-root>");
    } else {
        TestOfficialCorpus(std::filesystem::path(argv[1]));
    }
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "russian_text_processor_test: PASS\n";
    return 0;
}
