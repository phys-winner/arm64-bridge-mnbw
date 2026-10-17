#include "russian_text_processor.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace warband::localization {
namespace {

constexpr std::size_t kMaximumGuestTextBytes = 1024u * 1024u;
constexpr unsigned kMaximumNesting = 48;

enum class MarkerKind {
    Text,
    Integer,
    Plural,
    Case,
    Agreement,
    Lowercase,
    PushContext,
    PopContext,
};

enum class RussianCase {
    Nominative,
    Genitive,
    Dative,
    Accusative,
    Instrumental,
    Prepositional,
};

enum class Agreement {
    Unknown,
    Masculine,
    Feminine,
    Neuter,
    Plural,
};

struct MarkerPrefix {
    MarkerKind kind = MarkerKind::Text;
    std::size_t bytes = 0;
    RussianCase grammatical_case = RussianCase::Nominative;
    char direction = 0;
    bool standalone = false;
};

struct Node {
    MarkerKind kind = MarkerKind::Text;
    std::string body;
    RussianCase grammatical_case = RussianCase::Nominative;
    char direction = 0;
    bool malformed = false;
};

struct GrammarState {
    std::optional<std::int64_t> number;
    Agreement agreement = Agreement::Unknown;
    std::vector<std::pair<std::optional<std::int64_t>, Agreement>> saved;
};

struct WordSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
    bool capitalized = false;
};

bool StartsWithAt(std::string_view text, std::size_t offset, std::string_view prefix) {
    return offset <= text.size() && prefix.size() <= text.size() - offset &&
           text.compare(offset, prefix.size(), prefix) == 0;
}

std::optional<MarkerPrefix> RecognizeMarker(std::string_view text, std::size_t offset) {
    if (StartsWithAt(text, offset, "@#+")) {
        return MarkerPrefix{MarkerKind::PushContext, 3, RussianCase::Nominative, 0, true};
    }
    if (StartsWithAt(text, offset, "@#-")) {
        return MarkerPrefix{MarkerKind::PopContext, 3, RussianCase::Nominative, 0, true};
    }
    if (StartsWithAt(text, offset, "@Cl[")) {
        return MarkerPrefix{MarkerKind::Lowercase, 4};
    }
    if (offset + 4 <= text.size() && text[offset] == '@' &&
        (text[offset + 1] == 'L' || text[offset + 1] == 'R') &&
        (text[offset + 2] == '+' || text[offset + 2] == '-') &&
        text[offset + 3] == '[') {
        return MarkerPrefix{
            text[offset + 1] == 'L' ? MarkerKind::Plural : MarkerKind::Agreement,
            4,
            RussianCase::Nominative,
            text[offset + 2],
            false,
        };
    }
    if (offset + 4 <= text.size() && text[offset] == '@' && text[offset + 1] == 'P' &&
        text[offset + 3] == '[') {
        RussianCase grammatical_case;
        switch (text[offset + 2]) {
            case 'i': grammatical_case = RussianCase::Nominative; break;
            case 'r': grammatical_case = RussianCase::Genitive; break;
            case 'd': grammatical_case = RussianCase::Dative; break;
            case 'v': grammatical_case = RussianCase::Accusative; break;
            case 't': grammatical_case = RussianCase::Instrumental; break;
            case 'p': grammatical_case = RussianCase::Prepositional; break;
            default: return std::nullopt;
        }
        return MarkerPrefix{MarkerKind::Case, 4, grammatical_case};
    }
    if (StartsWithAt(text, offset, "@I[")) return MarkerPrefix{MarkerKind::Integer, 3};
    if (StartsWithAt(text, offset, "@L[")) return MarkerPrefix{MarkerKind::Plural, 3};
    if (StartsWithAt(text, offset, "@R[")) return MarkerPrefix{MarkerKind::Agreement, 3};
    return std::nullopt;
}

std::optional<std::size_t> FindClosingBracket(std::string_view text, std::size_t open) {
    unsigned depth = 0;
    for (std::size_t i = open; i < text.size(); ++i) {
        if (text[i] == '[') {
            ++depth;
        } else if (text[i] == ']') {
            if (--depth == 0) return i;
        }
    }
    return std::nullopt;
}

std::vector<Node> ParseNodes(std::string_view text) {
    std::vector<Node> nodes;
    std::size_t literal_start = 0;
    std::size_t cursor = 0;
    auto flush_literal = [&](std::size_t end) {
        if (end > literal_start) {
            nodes.push_back(Node{MarkerKind::Text, std::string(text.substr(literal_start, end - literal_start))});
        }
    };

    while (cursor < text.size()) {
        const auto prefix = RecognizeMarker(text, cursor);
        if (!prefix) {
            ++cursor;
            continue;
        }
        flush_literal(cursor);
        if (prefix->standalone) {
            nodes.push_back(Node{prefix->kind});
            cursor += prefix->bytes;
            literal_start = cursor;
            continue;
        }

        const std::size_t open = cursor + prefix->bytes - 1;
        const auto close = FindClosingBracket(text, open);
        Node node;
        node.kind = prefix->kind;
        node.grammatical_case = prefix->grammatical_case;
        node.direction = prefix->direction;
        if (close) {
            node.body.assign(text.substr(open + 1, *close - open - 1));
            cursor = *close + 1;
            // One official row contains @Cl[{s4}]] while the same token later
            // in that row is spelled correctly.  A balanced nested @Cl already
            // consumes both required brackets, so one immediately trailing
            // bracket is unambiguously the duplicated delimiter.
            if (node.kind == MarkerKind::Lowercase && cursor < text.size() &&
                text[cursor] == ']') {
                ++cursor;
                node.malformed = true;
            }
        } else {
            // Fail closed: preserve the visible body but consume the official
            // prefix, so broken localization can never print raw marker syntax.
            node.body.assign(text.substr(open + 1));
            node.malformed = true;
            cursor = text.size();
        }
        nodes.push_back(std::move(node));
        literal_start = cursor;
    }
    flush_literal(text.size());
    return nodes;
}

std::vector<std::string_view> SplitFields(std::string_view body) {
    std::vector<std::string_view> fields;
    std::size_t field_start = 0;
    unsigned depth = 0;
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '[') {
            ++depth;
        } else if (body[i] == ']' && depth != 0) {
            --depth;
        } else if (body[i] == ';' && depth == 0) {
            fields.push_back(body.substr(field_start, i - field_start));
            field_start = i + 1;
        }
    }
    fields.push_back(body.substr(field_start));
    return fields;
}

std::optional<std::int64_t> ParseInteger(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    if (text.empty()) return std::nullopt;
    std::int64_t value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
    return value;
}

unsigned PluralCategory(std::int64_t number) {
    const std::uint64_t magnitude = number < 0
        ? static_cast<std::uint64_t>(-(number + 1)) + 1
        : static_cast<std::uint64_t>(number);
    const unsigned last = static_cast<unsigned>(magnitude % 10);
    const unsigned last_two = static_cast<unsigned>(magnitude % 100);
    if (last == 1 && last_two != 11) return 0;
    if (last >= 2 && last <= 4 && (last_two < 12 || last_two > 14)) return 1;
    return 2;
}

bool DecodeUtf8(std::string_view text, std::size_t offset, char32_t* value, std::size_t* bytes) {
    if (!value || !bytes || offset >= text.size()) return false;
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) {
        *value = first;
        *bytes = 1;
        return true;
    }
    unsigned count = 0;
    char32_t decoded = 0;
    if ((first & 0xE0) == 0xC0) {
        count = 2;
        decoded = first & 0x1F;
        if (decoded == 0) return false;
    } else if ((first & 0xF0) == 0xE0) {
        count = 3;
        decoded = first & 0x0F;
    } else if ((first & 0xF8) == 0xF0) {
        count = 4;
        decoded = first & 0x07;
    } else {
        return false;
    }
    if (count > text.size() - offset) return false;
    for (unsigned i = 1; i < count; ++i) {
        const auto next = static_cast<unsigned char>(text[offset + i]);
        if ((next & 0xC0) != 0x80) return false;
        decoded = (decoded << 6) | (next & 0x3F);
    }
    if ((count == 2 && decoded < 0x80) || (count == 3 && decoded < 0x800) ||
        (count == 4 && decoded < 0x10000) || decoded > 0x10FFFF ||
        (decoded >= 0xD800 && decoded <= 0xDFFF)) {
        return false;
    }
    *value = decoded;
    *bytes = count;
    return true;
}

void AppendUtf8(std::string& output, char32_t value) {
    if (value <= 0x7F) {
        output.push_back(static_cast<char>(value));
    } else if (value <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (value >> 6)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    } else if (value <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (value >> 12)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (value >> 18)));
        output.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    }
}

char32_t LowerCodePoint(char32_t value) {
    if (value >= U'A' && value <= U'Z') return value + (U'a' - U'A');
    if (value >= U'А' && value <= U'Я') return value + (U'а' - U'А');
    if (value == U'Ё') return U'ё';
    return value;
}

bool IsLetter(char32_t value) {
    return (value >= U'A' && value <= U'Z') || (value >= U'a' && value <= U'z') ||
           (value >= U'А' && value <= U'Я') || (value >= U'а' && value <= U'я') ||
           value == U'Ё' || value == U'ё';
}

bool IsUpper(char32_t value) {
    return (value >= U'A' && value <= U'Z') || (value >= U'А' && value <= U'Я') || value == U'Ё';
}

std::string LowerUtf8(std::string_view text) {
    std::string output;
    output.reserve(text.size());
    for (std::size_t cursor = 0; cursor < text.size();) {
        char32_t value = 0;
        std::size_t bytes = 0;
        if (!DecodeUtf8(text, cursor, &value, &bytes)) {
            output.push_back(text[cursor++]);
            continue;
        }
        AppendUtf8(output, LowerCodePoint(value));
        cursor += bytes;
    }
    return output;
}

std::string LowercaseFirstLetter(std::string text) {
    for (std::size_t cursor = 0; cursor < text.size();) {
        char32_t value = 0;
        std::size_t bytes = 0;
        if (!DecodeUtf8(text, cursor, &value, &bytes)) {
            ++cursor;
            continue;
        }
        const char32_t lowered = LowerCodePoint(value);
        if (lowered != value) {
            std::string encoded;
            AppendUtf8(encoded, lowered);
            text.replace(cursor, bytes, encoded);
            break;
        }
        if (IsLetter(value)) break;
        cursor += bytes;
    }
    return text;
}

std::vector<WordSpan> FindWords(std::string_view text) {
    std::vector<WordSpan> words;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        char32_t value = 0;
        std::size_t bytes = 0;
        if (!DecodeUtf8(text, cursor, &value, &bytes) || !IsLetter(value)) {
            cursor += bytes == 0 ? 1 : bytes;
            continue;
        }
        WordSpan word;
        word.begin = cursor;
        word.capitalized = IsUpper(value);
        cursor += bytes;
        while (cursor < text.size()) {
            if (!DecodeUtf8(text, cursor, &value, &bytes) || !IsLetter(value)) break;
            cursor += bytes;
        }
        word.end = cursor;
        words.push_back(word);
    }
    return words;
}

bool EndsWith(std::string_view text, std::string_view ending) {
    return ending.size() <= text.size() &&
           text.compare(text.size() - ending.size(), ending.size(), ending) == 0;
}

bool EqualsAny(std::string_view value, std::initializer_list<std::string_view> choices) {
    return std::find(choices.begin(), choices.end(), value) != choices.end();
}

bool IsPreposition(std::string_view lower) {
    return EqualsAny(lower, {"без", "в", "во", "для", "до", "из", "к", "ко", "между",
                             "на", "над", "о", "об", "от", "по", "под", "при", "про",
                             "с", "со", "у", "через", "де"});
}

enum class AdjectiveForm {
    None,
    MasculineHard,
    MasculineSoft,
    FeminineHard,
    FeminineSoft,
    NeuterHard,
    NeuterSoft,
    PluralHard,
    PluralSoft,
};

AdjectiveForm DetectAdjective(std::string_view lower) {
    if (EndsWith(lower, "ый") || EndsWith(lower, "ой")) return AdjectiveForm::MasculineHard;
    // Russian spelling writes -ий after г/к/х even for the hard adjectival
    // declension: короткий -> короткому, not короткему.
    if (EndsWith(lower, "гий") || EndsWith(lower, "кий") || EndsWith(lower, "хий")) {
        return AdjectiveForm::MasculineHard;
    }
    if (EndsWith(lower, "ий")) return AdjectiveForm::MasculineSoft;
    if (EndsWith(lower, "ая")) return AdjectiveForm::FeminineHard;
    if (EndsWith(lower, "яя")) return AdjectiveForm::FeminineSoft;
    if (EndsWith(lower, "ое")) return AdjectiveForm::NeuterHard;
    if (EndsWith(lower, "ее")) return AdjectiveForm::NeuterSoft;
    if (EndsWith(lower, "ые")) return AdjectiveForm::PluralHard;
    if (EndsWith(lower, "ие")) return AdjectiveForm::PluralSoft;
    return AdjectiveForm::None;
}

Agreement AgreementForAdjective(AdjectiveForm form) {
    switch (form) {
        case AdjectiveForm::MasculineHard:
        case AdjectiveForm::MasculineSoft: return Agreement::Masculine;
        case AdjectiveForm::FeminineHard:
        case AdjectiveForm::FeminineSoft: return Agreement::Feminine;
        case AdjectiveForm::NeuterHard:
        case AdjectiveForm::NeuterSoft: return Agreement::Neuter;
        case AdjectiveForm::PluralHard:
        case AdjectiveForm::PluralSoft: return Agreement::Plural;
        case AdjectiveForm::None: return Agreement::Unknown;
    }
    return Agreement::Unknown;
}

Agreement DetectAgreement(std::string_view phrase) {
    if (phrase.find('{') != std::string_view::npos || phrase.find('%') != std::string_view::npos) {
        return Agreement::Unknown;
    }
    const auto words = FindWords(phrase);
    if (words.empty()) return Agreement::Unknown;
    const std::string first = LowerUtf8(phrase.substr(words.front().begin,
                                                      words.front().end - words.front().begin));
    if (EqualsAny(first, {"леди", "госпожа", "королева", "княгиня", "женщина", "девушка",
                          "мать", "дочь", "жена", "вдова", "деревня"})) {
        return Agreement::Feminine;
    }
    if (EqualsAny(first, {"королевство", "ханство", "княжество", "село", "войско",
                          "ополчение", "оружие", "копье", "платье"})) {
        return Agreement::Neuter;
    }
    if (EqualsAny(first, {"люди", "войска", "участники", "отряды", "стрелы", "болты"})) {
        return Agreement::Plural;
    }

    // A leading adjective is the least ambiguous gender signal in arbitrary
    // localized noun phrases (for example "Верховая лошадь").
    if (words.size() > 1) {
        const Agreement adjective = AgreementForAdjective(DetectAdjective(first));
        if (adjective != Agreement::Unknown) return adjective;
    }

    std::size_t head_index = 0;
    while (head_index + 1 < words.size()) {
        const std::string word = LowerUtf8(phrase.substr(
            words[head_index].begin, words[head_index].end - words[head_index].begin));
        if (DetectAdjective(word) == AdjectiveForm::None) break;
        ++head_index;
    }
    const std::string head = LowerUtf8(phrase.substr(
        words[head_index].begin, words[head_index].end - words[head_index].begin));
    if (EndsWith(head, "а") || EndsWith(head, "я")) return Agreement::Feminine;
    if (EndsWith(head, "о") || EndsWith(head, "е") || EndsWith(head, "ё") ||
        EndsWith(head, "мя")) return Agreement::Neuter;
    if (EndsWith(head, "ы") || EndsWith(head, "и")) return Agreement::Plural;
    if (EndsWith(head, "ь")) {
        if (EndsWith(head, "ость") || EndsWith(head, "есть") || EndsWith(head, "жь") ||
            EndsWith(head, "шь") || EndsWith(head, "чь") || EndsWith(head, "щь")) {
            return Agreement::Feminine;
        }
        return Agreement::Masculine;
    }
    return Agreement::Masculine;
}

std::string ReplaceEnding(std::string_view word, std::string_view old_ending,
                          std::string_view new_ending) {
    std::string output(word.substr(0, word.size() - old_ending.size()));
    output.append(new_ending);
    return output;
}

std::string InflectAdjective(std::string_view word, AdjectiveForm form,
                             RussianCase grammatical_case, bool animate) {
    if (grammatical_case == RussianCase::Nominative) return std::string(word);
    std::string_view old_ending;
    std::array<std::string_view, 5> endings;
    switch (form) {
        case AdjectiveForm::MasculineHard:
            if (EndsWith(LowerUtf8(word), "ый")) {
                old_ending = "ый";
            } else if (EndsWith(LowerUtf8(word), "ий")) {
                old_ending = "ий";
            } else {
                old_ending = "ой";
            }
            endings = {"ого", "ому", animate ? "ого" : old_ending,
                       old_ending == "ий" ? std::string_view("им") : std::string_view("ым"),
                       "ом"};
            break;
        case AdjectiveForm::MasculineSoft:
            old_ending = "ий";
            endings = {"его", "ему", animate ? "его" : "ий", "им", "ем"};
            break;
        case AdjectiveForm::FeminineHard:
            old_ending = "ая";
            endings = {"ой", "ой", "ую", "ой", "ой"};
            break;
        case AdjectiveForm::FeminineSoft:
            old_ending = "яя";
            endings = {"ей", "ей", "юю", "ей", "ей"};
            break;
        case AdjectiveForm::NeuterHard:
            old_ending = "ое";
            endings = {"ого", "ому", "ое", "ым", "ом"};
            break;
        case AdjectiveForm::NeuterSoft:
            old_ending = "ее";
            endings = {"его", "ему", "ее", "им", "ем"};
            break;
        case AdjectiveForm::PluralHard:
            old_ending = "ые";
            endings = {"ых", "ым", animate ? "ых" : "ые", "ыми", "ых"};
            break;
        case AdjectiveForm::PluralSoft:
            old_ending = "ие";
            endings = {"их", "им", animate ? "их" : "ие", "ими", "их"};
            break;
        case AdjectiveForm::None: return std::string(word);
    }
    unsigned index = 0;
    switch (grammatical_case) {
        case RussianCase::Genitive: index = 0; break;
        case RussianCase::Dative: index = 1; break;
        case RussianCase::Accusative: index = 2; break;
        case RussianCase::Instrumental: index = 3; break;
        case RussianCase::Prepositional: index = 4; break;
        case RussianCase::Nominative: return std::string(word);
    }
    return ReplaceEnding(word, old_ending, endings[index]);
}

bool IsAnimateHead(std::string_view lower) {
    return EqualsAny(lower, {"барон", "боец", "воин", "всадник", "граф", "девушка", "жена",
                             "женщина", "король", "крестьянин", "князь", "леди", "лорд",
                             "мать", "муж", "пленник", "рыцарь", "султан", "хан", "человек",
                             "ярл"});
}

struct InflectedWord {
    std::string text;
    bool complete = true;
};

InflectedWord InflectNoun(std::string_view word, RussianCase grammatical_case,
                          Agreement phrase_agreement, bool animate) {
    if (grammatical_case == RussianCase::Nominative) return {std::string(word), true};
    const std::string lower = LowerUtf8(word);
    if (IsPreposition(lower) || EqualsAny(lower, {"леди", "мадам", "суно"})) {
        return {std::string(word), true};
    }

    auto replace = [&](std::string_view old_ending, std::string_view replacement) {
        return InflectedWord{ReplaceEnding(word, old_ending, replacement), true};
    };
    if (lower == "замок") {
        switch (grammatical_case) {
            case RussianCase::Genitive: return replace("ок", "ка");
            case RussianCase::Dative: return replace("ок", "ку");
            case RussianCase::Accusative: return {std::string(word), true};
            case RussianCase::Instrumental: return replace("ок", "ком");
            case RussianCase::Prepositional: return replace("ок", "ке");
            default: break;
        }
    }
    if (lower == "боец") {
        switch (grammatical_case) {
            case RussianCase::Genitive: return replace("оец", "ойца");
            case RussianCase::Dative: return replace("оец", "ойцу");
            case RussianCase::Accusative:
                return animate ? replace("оец", "ойца") : InflectedWord{std::string(word), true};
            case RussianCase::Instrumental: return replace("оец", "ойцом");
            case RussianCase::Prepositional: return replace("оец", "ойце");
            default: break;
        }
    }
    if (EndsWith(lower, "ия")) {
        switch (grammatical_case) {
            case RussianCase::Genitive:
            case RussianCase::Dative:
            case RussianCase::Prepositional: return replace("ия", "ии");
            case RussianCase::Accusative: return replace("ия", "ию");
            case RussianCase::Instrumental: return replace("ия", "ией");
            default: break;
        }
    }
    if (EndsWith(lower, "ие")) {
        switch (grammatical_case) {
            case RussianCase::Genitive: return replace("ие", "ия");
            case RussianCase::Dative: return replace("ие", "ию");
            case RussianCase::Accusative: return {std::string(word), true};
            case RussianCase::Instrumental: return replace("ие", "ием");
            case RussianCase::Prepositional: return replace("ие", "ии");
            default: break;
        }
    }
    if (EndsWith(lower, "а")) {
        switch (grammatical_case) {
            case RussianCase::Genitive: {
                const std::string stem = lower.substr(0, lower.size() - std::string_view("а").size());
                const bool use_i = EndsWith(stem, "г") || EndsWith(stem, "к") || EndsWith(stem, "х") ||
                                   EndsWith(stem, "ж") || EndsWith(stem, "ч") || EndsWith(stem, "ш") ||
                                   EndsWith(stem, "щ");
                return replace("а", use_i ? "и" : "ы");
            }
            case RussianCase::Dative:
            case RussianCase::Prepositional: return replace("а", "е");
            case RussianCase::Accusative: return replace("а", "у");
            case RussianCase::Instrumental: return replace("а", "ой");
            default: break;
        }
    }
    if (EndsWith(lower, "я")) {
        switch (grammatical_case) {
            case RussianCase::Genitive: return replace("я", "и");
            case RussianCase::Dative:
            case RussianCase::Prepositional: return replace("я", "е");
            case RussianCase::Accusative: return replace("я", "ю");
            case RussianCase::Instrumental: return replace("я", "ей");
            default: break;
        }
    }
    if (EndsWith(lower, "о")) {
        switch (grammatical_case) {
            case RussianCase::Genitive: return replace("о", "а");
            case RussianCase::Dative: return replace("о", "у");
            case RussianCase::Accusative: return {std::string(word), true};
            case RussianCase::Instrumental: return replace("о", "ом");
            case RussianCase::Prepositional: return replace("о", "е");
            default: break;
        }
    }
    if (EndsWith(lower, "е") || EndsWith(lower, "ё")) {
        const std::string_view ending = EndsWith(lower, "е") ? std::string_view("е") : std::string_view("ё");
        switch (grammatical_case) {
            case RussianCase::Genitive: return replace(ending, "я");
            case RussianCase::Dative: return replace(ending, "ю");
            case RussianCase::Accusative: return {std::string(word), true};
            case RussianCase::Instrumental: return replace(ending, "ем");
            case RussianCase::Prepositional: return replace(ending, "е");
            default: break;
        }
    }
    if (EndsWith(lower, "й")) {
        switch (grammatical_case) {
            case RussianCase::Genitive: return replace("й", "я");
            case RussianCase::Dative: return replace("й", "ю");
            case RussianCase::Accusative: return animate ? replace("й", "я") : InflectedWord{std::string(word), true};
            case RussianCase::Instrumental: return replace("й", "ем");
            case RussianCase::Prepositional: return replace("й", "е");
            default: break;
        }
    }
    if (EndsWith(lower, "ь")) {
        const bool feminine = phrase_agreement == Agreement::Feminine;
        switch (grammatical_case) {
            case RussianCase::Genitive: return replace("ь", feminine ? "и" : "я");
            case RussianCase::Dative: return replace("ь", feminine ? "и" : "ю");
            case RussianCase::Accusative:
                return (!feminine && animate) ? replace("ь", "я") : InflectedWord{std::string(word), true};
            case RussianCase::Instrumental: return replace("ь", feminine ? "ью" : "ем");
            case RussianCase::Prepositional: return replace("ь", feminine ? "и" : "е");
            default: break;
        }
    }
    if (EndsWith(lower, "ы") || EndsWith(lower, "и")) {
        const std::string_view ending = EndsWith(lower, "ы") ? std::string_view("ы") : std::string_view("и");
        const bool soft = ending == "и";
        switch (grammatical_case) {
            case RussianCase::Dative: return replace(ending, soft ? "ям" : "ам");
            case RussianCase::Instrumental: return replace(ending, soft ? "ями" : "ами");
            case RussianCase::Prepositional: return replace(ending, soft ? "ях" : "ах");
            case RussianCase::Accusative:
            case RussianCase::Genitive:
                // Russian genitive plural and animate accusative plural require
                // lexical information. Keeping the localized nominative is the
                // only corruption-free fallback available from a surface string.
                return {std::string(word), false};
            default: break;
        }
    }

    // A Cyrillic consonant-final noun follows the productive masculine pattern.
    char32_t final = 0;
    std::size_t final_bytes = 0;
    for (std::size_t cursor = 0; cursor < lower.size();) {
        char32_t value = 0;
        std::size_t bytes = 0;
        if (!DecodeUtf8(lower, cursor, &value, &bytes)) {
            ++cursor;
            continue;
        }
        final = value;
        final_bytes = bytes;
        cursor += bytes;
    }
    (void)final_bytes;
    const bool consonant = final >= U'а' && final <= U'я' &&
                           std::u32string_view(U"аеёиоуыэюя").find(final) == std::u32string_view::npos;
    if (consonant) {
        switch (grammatical_case) {
            case RussianCase::Genitive: return {std::string(word) + "а", true};
            case RussianCase::Dative: return {std::string(word) + "у", true};
            case RussianCase::Accusative:
                return animate ? InflectedWord{std::string(word) + "а", true}
                               : InflectedWord{std::string(word), true};
            case RussianCase::Instrumental: return {std::string(word) + "ом", true};
            case RussianCase::Prepositional: return {std::string(word) + "е", true};
            default: break;
        }
    }
    return {std::string(word), false};
}

struct InflectedPhrase {
    std::string text;
    bool complete = true;
};

InflectedPhrase InflectPhrase(std::string_view phrase, RussianCase grammatical_case,
                              Agreement agreement) {
    if (grammatical_case == RussianCase::Nominative) return {std::string(phrase), true};
    const auto words = FindWords(phrase);
    if (words.empty() || agreement == Agreement::Unknown) return {std::string(phrase), false};

    std::vector<bool> selected(words.size(), false);
    std::vector<AdjectiveForm> adjective_forms(words.size(), AdjectiveForm::None);
    std::size_t head = 0;
    while (head + 1 < words.size()) {
        const std::string lower = LowerUtf8(phrase.substr(words[head].begin,
                                                           words[head].end - words[head].begin));
        adjective_forms[head] = DetectAdjective(lower);
        if (adjective_forms[head] == AdjectiveForm::None) break;
        selected[head++] = true;
    }
    selected[head] = true;
    const std::string head_lower = LowerUtf8(phrase.substr(
        words[head].begin, words[head].end - words[head].begin));
    const bool animate = IsAnimateHead(head_lower);

    // Titles followed by proper names decline both words ("Король Харлаус"),
    // while a lowercase complement after the head is usually already genitive.
    for (std::size_t i = head + 1; i < words.size(); ++i) {
        const std::string lower = LowerUtf8(phrase.substr(words[i].begin,
                                                           words[i].end - words[i].begin));
        if (IsPreposition(lower)) break;
        if (words[i].capitalized) selected[i] = true;
    }

    std::string output;
    output.reserve(phrase.size() + 16);
    std::size_t cursor = 0;
    bool complete = true;
    for (std::size_t i = 0; i < words.size(); ++i) {
        output.append(phrase.substr(cursor, words[i].begin - cursor));
        const std::string_view word = phrase.substr(words[i].begin, words[i].end - words[i].begin);
        if (!selected[i]) {
            output.append(word);
        } else if (adjective_forms[i] != AdjectiveForm::None) {
            output.append(InflectAdjective(word, adjective_forms[i], grammatical_case, animate));
        } else {
            const InflectedWord inflected = InflectNoun(word, grammatical_case, agreement, animate);
            output.append(inflected.text);
            complete = complete && inflected.complete;
        }
        cursor = words[i].end;
    }
    output.append(phrase.substr(cursor));
    return {std::move(output), complete};
}

std::string StripMarkerPrefixes(std::string_view text) {
    std::string output;
    output.reserve(text.size());
    for (std::size_t cursor = 0; cursor < text.size();) {
        const auto prefix = RecognizeMarker(text, cursor);
        if (!prefix) {
            output.push_back(text[cursor++]);
            continue;
        }
        cursor += prefix->bytes;
    }
    return output;
}

class Processor {
public:
    RussianTextResult Run(std::string_view input) {
        GrammarState state;
        std::string output = Render(input, state, 0);
        if (ContainsRussianTextMarkup(output)) {
            fallback_ = true;
            do {
                output = StripMarkerPrefixes(output);
            } while (ContainsRussianTextMarkup(output));
        }
        return {std::move(output), had_markup_, fallback_};
    }

private:
    std::string Render(std::string_view text, GrammarState& state, unsigned depth) {
        if (depth >= kMaximumNesting) {
            fallback_ = true;
            return StripMarkerPrefixes(text);
        }
        const auto nodes = ParseNodes(text);
        std::string output;
        output.reserve(text.size());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const Node& node = nodes[i];
            if (node.kind == MarkerKind::Text) {
                output.append(node.body);
                continue;
            }
            had_markup_ = true;
            fallback_ = fallback_ || node.malformed;
            switch (node.kind) {
                case MarkerKind::Integer: {
                    std::string rendered = Render(node.body, state, depth + 1);
                    state.number = ParseInteger(rendered);
                    if (!state.number) fallback_ = true;
                    output.append(rendered);
                    break;
                }
                case MarkerKind::Plural:
                    output.append(RenderPlural(nodes, i, node, state, depth));
                    break;
                case MarkerKind::Case: {
                    const auto fields = SplitFields(node.body);
                    if (fields.size() != 1) {
                        // Official game_strings.csv has one corrupt token,
                        // @Pr[сказал;;а;о], in the exact position and shape of
                        // an @R token. Recover its supplied m/f/n alternatives;
                        // the absent plural ending is the productive past-tense
                        // -и after its -л stem. This is intentionally structural
                        // recovery, not a new localization key or dictionary.
                        fallback_ = true;
                        Agreement agreement = state.agreement;
                        std::size_t suffix = 1;
                        if (agreement == Agreement::Feminine) suffix = 2;
                        if (agreement == Agreement::Neuter) suffix = 3;
                        std::string recovered = Render(fields[0], state, depth + 1);
                        if (agreement == Agreement::Plural && fields.size() == 4 &&
                            EndsWith(LowerUtf8(fields[0]), "л")) {
                            recovered.append("и");
                        } else if (suffix < fields.size()) {
                            recovered.append(Render(fields[suffix], state, depth + 1));
                        }
                        output.append(recovered);
                        break;
                    }
                    std::string rendered = Render(node.body, state, depth + 1);
                    state.agreement = DetectAgreement(rendered);
                    if (state.agreement == Agreement::Unknown) fallback_ = true;
                    const InflectedPhrase inflected =
                        InflectPhrase(rendered, node.grammatical_case, state.agreement);
                    fallback_ = fallback_ || !inflected.complete;
                    output.append(inflected.text);
                    break;
                }
                case MarkerKind::Agreement:
                    output.append(RenderAgreement(nodes, i, node, state, depth));
                    break;
                case MarkerKind::Lowercase: {
                    output.append(LowercaseFirstLetter(Render(node.body, state, depth + 1)));
                    break;
                }
                case MarkerKind::PushContext:
                    state.saved.emplace_back(state.number, state.agreement);
                    break;
                case MarkerKind::PopContext:
                    if (state.saved.empty()) {
                        fallback_ = true;
                    } else {
                        state.number = state.saved.back().first;
                        state.agreement = state.saved.back().second;
                        state.saved.pop_back();
                    }
                    break;
                case MarkerKind::Text: break;
            }
        }
        return output;
    }

    std::optional<std::int64_t> FindNextNumber(const std::vector<Node>& nodes,
                                                std::size_t start,
                                                const GrammarState& state,
                                                unsigned depth) {
        for (std::size_t i = start; i < nodes.size(); ++i) {
            if (nodes[i].kind == MarkerKind::Integer) {
                GrammarState probe = state;
                return ParseInteger(Render(nodes[i].body, probe, depth + 1));
            }
            if (nodes[i].kind == MarkerKind::Lowercase) {
                const auto nested = ParseNodes(nodes[i].body);
                if (auto number = FindNextNumber(nested, 0, state, depth + 1)) return number;
            }
        }
        return std::nullopt;
    }

    Agreement FindNextAgreement(const std::vector<Node>& nodes, std::size_t start,
                                const GrammarState& state, unsigned depth) {
        for (std::size_t i = start; i < nodes.size(); ++i) {
            if (nodes[i].kind == MarkerKind::Case) {
                GrammarState probe = state;
                const Agreement found = DetectAgreement(Render(nodes[i].body, probe, depth + 1));
                if (found != Agreement::Unknown) return found;
            }
            if (nodes[i].kind == MarkerKind::Lowercase) {
                const Agreement found = FindNextAgreement(ParseNodes(nodes[i].body), 0, state, depth + 1);
                if (found != Agreement::Unknown) return found;
            }
        }
        return Agreement::Unknown;
    }

    std::string RenderPlural(const std::vector<Node>& nodes, std::size_t index,
                             const Node& node, GrammarState& state, unsigned depth) {
        const auto fields = SplitFields(node.body);
        if (fields.size() != 4) {
            fallback_ = true;
            if (fields.empty()) return {};
            std::string output = Render(fields.front(), state, depth + 1);
            if (fields.size() > 1) output.append(Render(fields[1], state, depth + 1));
            return output;
        }
        std::optional<std::int64_t> number;
        if (node.direction == '+') number = FindNextNumber(nodes, index + 1, state, depth);
        if (!number) number = state.number;
        if (!number) number = FindNextNumber(nodes, index + 1, state, depth);
        unsigned category = 2;
        if (number) {
            category = PluralCategory(*number);
        } else {
            fallback_ = true;
        }
        std::string output = Render(fields[0], state, depth + 1);
        output.append(Render(fields[category + 1], state, depth + 1));
        return output;
    }

    std::string RenderAgreement(const std::vector<Node>& nodes, std::size_t index,
                                const Node& node, GrammarState& state, unsigned depth) {
        const auto fields = SplitFields(node.body);
        if (fields.size() != 5) {
            fallback_ = true;
            if (fields.empty()) return {};
            std::string output = Render(fields.front(), state, depth + 1);
            if (fields.size() > 1) output.append(Render(fields[1], state, depth + 1));
            return output;
        }
        Agreement agreement = Agreement::Unknown;
        // In the official dialect both @R+ and the rare bare @R bind to the
        // following @P controller; @R- binds to the preceding one.
        if (node.direction != '-') agreement = FindNextAgreement(nodes, index + 1, state, depth);
        if (agreement == Agreement::Unknown) agreement = state.agreement;
        // One official row incorrectly uses '-' before its controlling @P token.
        // Looking ahead only when no valid prior context exists repairs it
        // without changing normal backward semantics.
        if (agreement == Agreement::Unknown) {
            agreement = FindNextAgreement(nodes, index + 1, state, depth);
        }
        unsigned suffix = 1;
        switch (agreement) {
            case Agreement::Masculine: suffix = 1; break;
            case Agreement::Feminine: suffix = 2; break;
            case Agreement::Neuter: suffix = 3; break;
            case Agreement::Plural: suffix = 4; break;
            case Agreement::Unknown:
                suffix = 1;
                fallback_ = true;
                break;
        }
        std::string output = Render(fields[0], state, depth + 1);
        output.append(Render(fields[suffix], state, depth + 1));
        return output;
    }

    bool had_markup_ = false;
    bool fallback_ = false;
};

bool RangeFits(std::size_t arena_size, std::uint64_t address, std::uint64_t bytes) {
    return address <= arena_size && bytes <= arena_size - address;
}

std::uint32_t ReadLe32(std::span<const std::uint8_t> memory, std::size_t offset) {
    return static_cast<std::uint32_t>(memory[offset]) |
           (static_cast<std::uint32_t>(memory[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(memory[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(memory[offset + 3]) << 24);
}

void WriteLe32(std::span<std::uint8_t> memory, std::size_t offset, std::uint32_t value) {
    memory[offset] = static_cast<std::uint8_t>(value);
    memory[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    memory[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    memory[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

}  // namespace

bool ContainsRussianTextMarkup(std::string_view text) {
    for (std::size_t cursor = 0; cursor < text.size(); ++cursor) {
        if (RecognizeMarker(text, cursor)) return true;
    }
    return false;
}

RussianTextResult ProcessRussianText(std::string_view input) {
    return Processor{}.Run(input);
}

GuestRussianTextStatus ProcessGuestRussianText(std::span<std::uint8_t> guest_memory,
                                               std::uint32_t guest_object,
                                               bool* used_fallback) {
    if (used_fallback) *used_fallback = false;
    constexpr std::uint32_t kDataOffset = 4;
    constexpr std::uint32_t kCapacityOffset = 8;
    constexpr std::uint32_t kLengthOffset = 12;
    constexpr std::uint32_t kObjectHeaderBytes = 16;

    if (guest_object == 0 ||
        !RangeFits(guest_memory.size(), guest_object, kObjectHeaderBytes)) {
        return GuestRussianTextStatus::InvalidObject;
    }
    const std::uint32_t data = ReadLe32(guest_memory, guest_object + kDataOffset);
    const std::uint32_t capacity = ReadLe32(guest_memory, guest_object + kCapacityOffset);
    const std::uint32_t length = ReadLe32(guest_memory, guest_object + kLengthOffset);
    if (data == 0 || capacity == 0 ||
        static_cast<std::uint64_t>(length) + 1 > capacity ||
        length > kMaximumGuestTextBytes ||
        !RangeFits(guest_memory.size(), data, static_cast<std::uint64_t>(length) + 1)) {
        return GuestRussianTextStatus::InvalidData;
    }
    const std::uint64_t data_end = static_cast<std::uint64_t>(data) + length + 1;
    const std::uint64_t object_end = static_cast<std::uint64_t>(guest_object) + kObjectHeaderBytes;
    if (data < object_end && data_end > guest_object) {
        return GuestRussianTextStatus::InvalidData;
    }
    if (guest_memory[data + length] != 0) return GuestRussianTextStatus::InvalidData;

    const std::string_view original(
        reinterpret_cast<const char*>(guest_memory.data() + data), length);
    if (!ContainsRussianTextMarkup(original)) return GuestRussianTextStatus::NoMarkup;
    RussianTextResult result = ProcessRussianText(original);
    if (ContainsRussianTextMarkup(result.text)) return GuestRussianTextStatus::InvalidData;
    if (result.text.size() + 1 > capacity ||
        !RangeFits(guest_memory.size(), data, result.text.size() + 1)) {
        return GuestRussianTextStatus::InsufficientCapacity;
    }

    std::memmove(guest_memory.data() + data, result.text.data(), result.text.size());
    guest_memory[data + result.text.size()] = 0;
    WriteLe32(guest_memory, guest_object + kLengthOffset,
              static_cast<std::uint32_t>(result.text.size()));
    if (used_fallback) *used_fallback = result.used_fallback;
    return GuestRussianTextStatus::Processed;
}

}  // namespace warband::localization
