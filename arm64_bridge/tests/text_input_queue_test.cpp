#include "text_input_queue.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++g_failures;
}

void TestCyrillicEditAndFrameDrain() {
    using namespace warband::text_input;
    SessionQueue queue;
    const std::uint64_t token = queue.Begin();
    const std::array<std::uint16_t, 8> name = {
        0x042Eu, 0x043Bu, 0x0438u, 0x044Fu,  // Yu-li-ya
        0x0401u, 0x0436u, 0x0034u, 0x0032u,  // Yo-zh-4-2
    };
    const std::array<std::uint16_t, 1> short_i = {0x0439u};

    Check(queue.Apply(token, 0, name, false), "accept Cyrillic name edit");
    Check(queue.Apply(token, 2, short_i, true), "accept delete/delete/short-i/submit edit");

    std::vector<std::uint32_t> expected(name.begin(), name.end());
    expected.push_back(kBackspaceCodePoint);
    expected.push_back(kBackspaceCodePoint);
    expected.push_back(0x0439u);
    expected.push_back(kSubmitCodePoint);
    Check(queue.Size() == expected.size(), "queue exact Cyrillic edit length");

    std::uint32_t slot = 0u;
    std::size_t writes = 0;
    const auto read_slot = [&slot]() { return slot; };
    const auto write_slot = [&slot, &writes](std::uint32_t value) {
        slot = value;
        ++writes;
    };
    Check(queue.DrainOne(read_slot, write_slot) == DrainResult::Busy,
          "busy guest slot is not overwritten");
    Check(queue.Size() == expected.size() && writes == 0,
          "busy guest slot preserves queue");

    for (std::size_t index = 0; index < expected.size(); ++index) {
        slot = kPendingSlotEmpty;
        const std::size_t before = queue.Size();
        Check(queue.DrainOne(read_slot, write_slot) == DrainResult::Drained,
              "one value drains on frame");
        Check(slot == expected[index], "frame drain preserves command order");
        Check(queue.Size() + 1u == before, "frame drain removes exactly one value");
        if (index + 1u < expected.size()) {
            Check(queue.DrainOne(read_slot, write_slot) == DrainResult::Busy,
                  "written slot blocks a second drain before guest consumption");
        }
    }
    Check(queue.Empty(), "Cyrillic edit queue fully drained");
}

void TestAtomicOverflow() {
    using namespace warband::text_input;
    std::array<std::uint16_t, kMaxEditCodePoints> block{};
    block.fill(0x0410u);

    SessionQueue maximal_edit;
    const std::uint64_t maximal_token = maximal_edit.Begin();
    Check(maximal_edit.Apply(maximal_token, kMaxDeleteBefore, block, true),
          "maximal 256-delete/256-insert/submit edit fits atomically");
    Check(maximal_edit.Size() == 2u * kMaxEditCodePoints + 1u,
          "maximal edit queues exactly 513 commands");

    SessionQueue queue;
    const std::uint64_t token = queue.Begin();
    for (std::size_t index = 0; index < kQueueCapacity / block.size(); ++index) {
        Check(queue.Apply(token, 0, block, false), "fill bounded queue");
    }
    Check(queue.Size() == kQueueCapacity, "bounded queue reaches documented capacity");

    const std::array<std::uint16_t, 1> extra = {0x0439u};
    const std::size_t before = queue.Size();
    Check(!queue.Apply(token, 2, extra, true), "overflowing edit is rejected");
    Check(queue.Size() == before, "overflow rejection is atomic");

    std::uint32_t slot = kPendingSlotEmpty;
    for (std::size_t index = 0; index < kQueueCapacity; ++index) {
        Check(queue.DrainOne([&slot]() { return slot; },
                             [&slot](std::uint32_t value) { slot = value; }) ==
                  DrainResult::Drained,
              "filled queue drains");
        Check(slot == 0x0410u, "rejected overflow appended no partial command");
        slot = kPendingSlotEmpty;
    }
    Check(queue.Empty(), "filled queue drains to empty");
}

void TestUtf16ValidationIsAtomic() {
    using namespace warband::text_input;
    SessionQueue queue;
    const std::uint64_t token = queue.Begin();
    const std::array<std::uint16_t, 6> valid_boundaries = {
        0x0020u, 0x007Eu, 0x00A0u, 0xD7FFu, 0xE000u, 0xFFFDu,
    };
    Check(queue.Apply(token, 0, valid_boundaries, false), "accept exact UTF-16 boundaries");
    const std::size_t accepted_size = queue.Size();

    const std::array<std::uint16_t, 10> invalid_units = {
        0x0000u, 0x000Au, 0x001Fu, 0x007Fu, 0x0080u,
        0x009Fu, 0xD800u, 0xDFFFu, 0xFFFEu, 0xFFFFu,
    };
    for (const std::uint16_t unit : invalid_units) {
        const std::array<std::uint16_t, 1> text = {unit};
        Check(!queue.Apply(token, 1, text, true), "reject forbidden UTF-16 unit");
        Check(queue.Size() == accepted_size, "invalid-unit rejection is atomic");
    }

    const std::array<std::uint16_t, 2> supplementary_pair = {0xD83Du, 0xDE03u};
    Check(!queue.Apply(token, 2, supplementary_pair, true),
          "reject supplementary surrogate pair");
    Check(queue.Size() == accepted_size, "surrogate rejection is atomic");
    std::array<std::uint16_t, kMaxEditCodePoints + 1u> oversized{};
    oversized.fill(0x0041u);
    Check(!queue.Apply(token, 0, oversized, false), "reject edit above 256 BMP units");
    Check(!queue.Apply(token, -1, {}, false), "reject negative delete count");
    Check(!queue.Apply(token, kMaxDeleteBefore + 1, {}, false),
          "reject delete count above 256");
    Check(queue.Size() == accepted_size, "invalid delete rejection is atomic");
}

void TestStaleTokensAndEnd() {
    using namespace warband::text_input;
    SessionQueue queue;
    const std::array<std::uint16_t, 1> text = {0x0416u};
    const std::uint64_t first = queue.Begin();
    Check(first != 0, "begin returns nonzero token");
    Check(queue.Apply(first, 0, text, false), "first token applies");

    const std::uint64_t second = queue.Begin();
    Check(second == first + 1u, "normal tokens increase monotonically");
    Check(queue.Empty(), "begin clears previous queue");
    Check(!queue.Apply(first, 0, text, false), "stale token cannot apply");
    Check(queue.Apply(second, 0, text, false), "second token applies");
    Check(!queue.End(first), "stale end is ignored");
    Check(queue.ActiveToken() == second, "stale end preserves active session");
    Check(queue.Size() == 1u, "stale end preserves active queue");
    Check(queue.Apply(second, 0, text, false), "active token still applies after stale end");
    Check(queue.End(second), "matching end succeeds");
    Check(queue.ActiveToken() == 0 && queue.Empty(), "matching end invalidates and clears");

    SessionQueue wrapping(std::numeric_limits<std::uint64_t>::max() - 1u);
    Check(wrapping.Begin() == std::numeric_limits<std::uint64_t>::max(),
          "last nonzero token is issued");
    Check(wrapping.Begin() == 1u, "token wrap skips zero");
}

void TestResetAndWriteVerification() {
    using namespace warband::text_input;
    SessionQueue queue;
    const std::array<std::uint16_t, 1> text = {0x042Eu};
    const std::uint64_t token = queue.Begin();
    Check(queue.Apply(token, 0, text, false), "seed reset test");

    std::uint32_t slot = kPendingSlotEmpty;
    Check(queue.DrainOne([&slot]() { return slot; },
                         [](std::uint32_t) {}) == DrainResult::WriteNotObserved,
          "unobserved write is reported");
    Check(queue.Size() == 1u, "failed write verification does not pop");

    queue.InvalidateForBridgeLifecycle();
    Check(queue.ActiveToken() == 0 && queue.Empty(),
          "bridge lifecycle invalidation clears active session");
    Check(!queue.Apply(token, 0, text, false),
          "pre-invalidation token stays stale");
    const std::uint64_t next = queue.Begin();
    Check(next != 0 && next != token,
          "bridge lifecycle invalidation preserves unique token watermark");
}

std::string_view FunctionBody(std::string_view source, std::string_view signature) {
    const std::size_t function = source.find(signature);
    if (function == std::string_view::npos) return {};
    const std::size_t open = source.find('{', function + signature.size());
    if (open == std::string_view::npos) return {};

    std::size_t depth = 0;
    for (std::size_t index = open; index < source.size(); ++index) {
        if (source[index] == '{') {
            ++depth;
        } else if (source[index] == '}') {
            if (--depth == 0) return source.substr(open, index - open + 1u);
        }
    }
    return {};
}

std::size_t CountOccurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t offset = 0;
         (offset = text.find(needle, offset)) != std::string_view::npos;
         offset += needle.size()) {
        ++count;
    }
    return count;
}

void TestProductionSessionOwnershipInvariant() {
#ifdef WARBAND_JNI_BRIDGE_SOURCE
    std::ifstream input(WARBAND_JNI_BRIDGE_SOURCE, std::ios::binary);
    Check(input.good(), "open production JNI bridge for ownership audit");
    if (!input) return;
    const std::string source((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());

    const std::string_view pop = FunctionBody(source, "uint32_t PopPendingGuestEventLocked(");
    Check(pop.find("g_discrete_buttons.PopFrameEncodedEvent()") != std::string_view::npos,
          "production discrete drain preserves each key transition across ticks");
    const std::string_view frame = FunctionBody(source, "uint32_t DrainFrameInputLocked(");
    Check(frame.find("g_discrete_buttons.FinishFrame();") != std::string_view::npos,
          "post-rglKeys tick hook reopens the discrete frame gate");
    Check(frame.find("DrainTextInputLocked();") != std::string_view::npos,
          "text FIFO continues independently of the blocked discrete head");

    constexpr std::string_view invalidator =
        "g_text_input_queue.InvalidateForBridgeLifecycle();";
    Check(CountOccurrences(source, invalidator) == 1u,
          "only the bridge-lifecycle owner may directly invalidate text input");

    for (const std::string_view signature : {
             std::string_view{"void Native_Start("},
             std::string_view{"void Native_Shutdown("},
             std::string_view{"void Native_Reset("},
             std::string_view{"void ResetObservedPhysicalInput("},
         }) {
        const std::string_view body = FunctionBody(source, signature);
        Check(!body.empty(), "find control/input reset function for ownership audit");
        Check(body.find("InvalidateTextInputForBridgeLifecycle") == std::string_view::npos &&
                  body.find("g_text_input_queue.") == std::string_view::npos,
              "controls and AInputQueue lifecycle must not mutate text sessions");
    }

    const std::string_view boot = FunctionBody(source, "bool BootGuest(");
    const std::size_t existing_host = boot.find("if (g_host) return true;");
    const std::size_t fresh_init =
        boot.find("InvalidateTextInputForBridgeLifecycle(true);");
    Check(!boot.empty() && existing_host != std::string_view::npos &&
              fresh_init != std::string_view::npos && existing_host < fresh_init,
          "an existing bridge must retain a live Java text token");

    const std::string_view drain = FunctionBody(source, "void DrainTextInputLocked(");
    Check(drain.find("InvalidateTextInputForBridgeLifecycleLocked(true);") !=
              std::string_view::npos,
          "invalid guest slot remains a fatal text-bridge teardown");
#else
    Check(false, "WARBAND_JNI_BRIDGE_SOURCE must be defined");
#endif
}

}  // namespace

int main() {
    TestCyrillicEditAndFrameDrain();
    TestAtomicOverflow();
    TestUtf16ValidationIsAtomic();
    TestStaleTokensAndEnd();
    TestResetAndWriteVerification();
    TestProductionSessionOwnershipInvariant();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "text input queue tests passed\n";
    return EXIT_SUCCESS;
}
