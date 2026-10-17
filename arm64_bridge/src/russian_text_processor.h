#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace warband::localization {

struct RussianTextResult {
    std::string text;
    bool had_markup = false;
    // True when an official marker was malformed or a grammatical decision was
    // genuinely ambiguous.  The returned text is still guaranteed not to expose
    // any supported marker syntax to the player.
    bool used_fallback = false;
};

// Implements the marker dialect used by the official Warband Russian CSV files:
// @I/@L[+-], @Pi/@Pr/@Pd/@Pv/@Pt/@Pp, @R[+-], @Cl, and @#+/@#-.
RussianTextResult ProcessRussianText(std::string_view input);

bool ContainsRussianTextMarkup(std::string_view text);

enum class GuestRussianTextStatus {
    NoMarkup,
    Processed,
    InvalidObject,
    InvalidData,
    InsufficientCapacity,
};

// Process the ARM32 rglString at guest_object in place.  The verified Warband
// layout is data/capacity-including-NUL/length at +4/+8/+12 and the inline
// buffer at +16.
// No guest memory is changed on validation or capacity failure.
GuestRussianTextStatus ProcessGuestRussianText(
    std::span<std::uint8_t> guest_memory,
    std::uint32_t guest_object,
    bool* used_fallback = nullptr);

}  // namespace warband::localization
