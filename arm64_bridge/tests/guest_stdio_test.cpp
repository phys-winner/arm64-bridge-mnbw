#include "guest_stdio_helpers.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace {

struct TestState {
    int checks = 0;
    int failures = 0;

    void Check(bool condition, const char* expression, int line) {
        ++checks;
        if (!condition) {
            ++failures;
            std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        }
    }
};

#define CHECK(state, expression) \
    (state).Check(static_cast<bool>(expression), #expression, __LINE__)

void TestCharacterWrites(TestState& state) {
    constexpr uint32_t kGuestFile = 0x1234u;
#if defined(_MSC_VER)
    FILE* stream = nullptr;
    const int temp_file_result = tmpfile_s(&stream);
    CHECK(state, temp_file_result == 0);
#else
    FILE* stream = std::tmpfile();
    CHECK(state, stream != nullptr);
#endif
    if (!stream) return;

    std::unordered_map<uint32_t, FILE*> files{{kGuestFile, stream}};
    std::mutex file_mutex;

    CHECK(state, warband::guest_stdio::PutCharacter(files, file_mutex, kGuestFile, 'A') == 'A');
    CHECK(state, warband::guest_stdio::PutCharacter(files, file_mutex, kGuestFile, 0) == 0);
    CHECK(state, warband::guest_stdio::PutCharacter(files, file_mutex, kGuestFile, 0xFF) == 0xFF);
    CHECK(state, std::fflush(stream) == 0);
    CHECK(state, std::fseek(stream, 0, SEEK_SET) == 0);

    std::array<unsigned char, 3> actual{};
    CHECK(state, std::fread(actual.data(), 1, actual.size(), stream) == actual.size());
    constexpr std::array<unsigned char, 3> expected{{'A', 0, 0xFF}};
    CHECK(state, std::memcmp(actual.data(), expected.data(), expected.size()) == 0);

    errno = 0;
    CHECK(state, warband::guest_stdio::PutCharacter(files, file_mutex, 0x9999u, 'x') == EOF);
    CHECK(state, errno == EBADF);
    CHECK(state, std::fclose(stream) == 0);
}

void TestFormattedAndStringWrites(TestState& state) {
    constexpr uint32_t kGuestFile = 0x5678u;
#if defined(_MSC_VER)
    FILE* stream = nullptr;
    const int temp_file_result = tmpfile_s(&stream);
    CHECK(state, temp_file_result == 0);
#else
    FILE* stream = std::tmpfile();
    CHECK(state, stream != nullptr);
#endif
    if (!stream) return;

    std::unordered_map<uint32_t, FILE*> files{{kGuestFile, stream}};
    std::mutex file_mutex;
    constexpr char kFormatted[] = "battle_size = 0.7500\n";
    constexpr char kString[] = "verbose_damage = 0\n";

    CHECK(state, warband::guest_stdio::WriteBytes(
        files, file_mutex, kGuestFile, kFormatted, sizeof(kFormatted) - 1) ==
        static_cast<int>(sizeof(kFormatted) - 1));
    CHECK(state, warband::guest_stdio::PutString(
        files, file_mutex, kGuestFile, kString) != EOF);
    CHECK(state, std::fflush(stream) == 0);
    CHECK(state, std::fseek(stream, 0, SEEK_SET) == 0);

    constexpr char kExpected[] =
        "battle_size = 0.7500\n"
        "verbose_damage = 0\n";
    std::array<char, sizeof(kExpected)> actual{};
    CHECK(state, std::fread(actual.data(), 1, sizeof(kExpected) - 1, stream) ==
        sizeof(kExpected) - 1);
    CHECK(state, std::memcmp(actual.data(), kExpected, sizeof(kExpected) - 1) == 0);

    errno = 0;
    CHECK(state, warband::guest_stdio::WriteBytes(
        files, file_mutex, 0x9999u, kFormatted, sizeof(kFormatted) - 1) == EOF);
    CHECK(state, errno == EBADF);
    errno = 0;
    CHECK(state, warband::guest_stdio::PutString(
        files, file_mutex, 0x9999u, kString) == EOF);
    CHECK(state, errno == EBADF);
    CHECK(state, std::fclose(stream) == 0);
}

}  // namespace

int main() {
    TestState state;
    TestCharacterWrites(state);
    TestFormattedAndStringWrites(state);
    std::printf("%d checks, %d failures\n", state.checks, state.failures);
    return state.failures == 0 ? 0 : 1;
}
