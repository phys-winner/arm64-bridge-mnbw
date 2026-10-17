#define WARBAND_FMOD_HOST_DECODE_TEST_ONLY
#include "../src/fmod_host_decode.cpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

namespace {

using warband::audio::AllocateFmodScratch;
using warband::audio::FmodScratchAllocResult;
using warband::audio::GuestSoundPublishResult;
using warband::audio::PublishGuestSound;

static_assert(warband::audio::kFmodCreateSoundExInfoBytes == 0x88);
static_assert(warband::audio::kFmodCreateSoundExInfoAlignment == 8);

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

#define CHECK(state, expression) (state).Check(static_cast<bool>(expression), #expression, __LINE__)

struct AllocationRequest {
    std::size_t bytes;
    std::size_t alignment;

    bool operator==(const AllocationRequest&) const = default;
};

struct FakeHost {
    std::size_t memory_size = 0x1000;
    bool drop_writes = false;
    std::vector<std::uint32_t> allocation_results;
    std::size_t next_allocation = 0;
    std::vector<AllocationRequest> allocation_requests;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> writes;
    std::vector<std::uint32_t> frees;
    std::map<std::uint32_t, std::uint32_t> memory;

    std::uint32_t GuestAlloc(std::size_t bytes, std::size_t alignment) {
        allocation_requests.push_back({bytes, alignment});
        if (next_allocation == allocation_results.size()) return 0;
        return allocation_results[next_allocation++];
    }

    void GuestFree(std::uint32_t address) { frees.push_back(address); }

    void MemoryWrite32(std::uint32_t address, std::uint32_t value) {
        writes.emplace_back(address, value);
        if (!drop_writes && static_cast<std::uint64_t>(address) + 4 <= memory_size) {
            memory[address] = value;
        }
    }

    std::uint32_t MemoryRead32(std::uint32_t address) const {
        const auto found = memory.find(address);
        return found == memory.end() ? 0 : found->second;
    }

    std::size_t GetGuestMemorySize() const { return memory_size; }
};

void TestOutHolderAllocationFailure(TestState& state) {
    FakeHost host;
    host.allocation_results = {0};
    std::uint32_t holder = 0xAAAAAAAAu;
    std::uint32_t exinfo = 0xBBBBBBBBu;

    CHECK(state, AllocateFmodScratch(host, holder, exinfo) ==
                     FmodScratchAllocResult::kOutHolderFailed);
    CHECK(state, holder == 0 && exinfo == 0);
    CHECK(state, (host.allocation_requests ==
                  std::vector<AllocationRequest>{{sizeof(std::uint32_t),
                                                  alignof(std::uint32_t)}}));
    CHECK(state, host.writes.empty());
    CHECK(state, host.frees.empty());
}

void TestExInfoAllocationFailureRollsBack(TestState& state) {
    FakeHost host;
    host.allocation_results = {0x100u, 0};
    std::uint32_t holder = 0xAAAAAAAAu;
    std::uint32_t exinfo = 0xBBBBBBBBu;

    CHECK(state, AllocateFmodScratch(host, holder, exinfo) ==
                     FmodScratchAllocResult::kExInfoFailed);
    CHECK(state, holder == 0 && exinfo == 0);
    CHECK(state, (host.allocation_requests ==
                  std::vector<AllocationRequest>{
                      {sizeof(std::uint32_t), alignof(std::uint32_t)},
                      {0x88, 8},
                  }));
    CHECK(state, (host.writes ==
                  std::vector<std::pair<std::uint32_t, std::uint32_t>>{{0x100u, 0}}));
    CHECK(state, host.frees == std::vector<std::uint32_t>{0x100u});
}

void TestScratchAllocationSuccess(TestState& state) {
    FakeHost host;
    host.allocation_results = {0x100u, 0x200u};
    std::uint32_t holder = 0;
    std::uint32_t exinfo = 0;

    CHECK(state, AllocateFmodScratch(host, holder, exinfo) ==
                     FmodScratchAllocResult::kOk);
    CHECK(state, holder == 0x100u && exinfo == 0x200u);
    CHECK(state, host.MemoryRead32(holder) == 0);
    CHECK(state, host.frees.empty());
}

void TestPublishRequiresSuccessfulCreate(TestState& state) {
    FakeHost host;

    CHECK(state, PublishGuestSound(host, 19, 0x400u, 0x100u) ==
                     GuestSoundPublishResult::kCreateFailed);
    CHECK(state, PublishGuestSound(host, 0, 0, 0x100u) ==
                     GuestSoundPublishResult::kCreateFailed);
    CHECK(state, host.writes.empty());
}

void TestPublishWritesOriginalOutput(TestState& state) {
    FakeHost host;
    constexpr std::uint32_t kOutSound = 0x0FFCu;
    constexpr std::uint32_t kSound = 0x76543210u;

    CHECK(state, PublishGuestSound(host, 0, kSound, kOutSound) ==
                     GuestSoundPublishResult::kPublished);
    CHECK(state, host.MemoryRead32(kOutSound) == kSound);
    CHECK(state, (host.writes ==
                  std::vector<std::pair<std::uint32_t, std::uint32_t>>{
                      {kOutSound, kSound}}));
}

void TestPublishRejectsUnsafeOutput(TestState& state) {
    FakeHost host;

    CHECK(state, PublishGuestSound(host, 0, 0x400u, 0) ==
                     GuestSoundPublishResult::kOutputRejected);
    CHECK(state, PublishGuestSound(host, 0, 0x400u, 0x101u) ==
                     GuestSoundPublishResult::kOutputRejected);
    CHECK(state, PublishGuestSound(host, 0, 0x400u, 0x1000u) ==
                     GuestSoundPublishResult::kOutputRejected);
    CHECK(state, PublishGuestSound(host, 0, 0x400u, 0xFFFFFFFCu) ==
                     GuestSoundPublishResult::kOutputRejected);
    CHECK(state, host.writes.empty());

    host.drop_writes = true;
    CHECK(state, PublishGuestSound(host, 0, 0x400u, 0x100u) ==
                     GuestSoundPublishResult::kOutputRejected);
    CHECK(state, host.writes.size() == 1);
}

}  // namespace

int main() {
    TestState state;
    TestOutHolderAllocationFailure(state);
    TestExInfoAllocationFailureRollsBack(state);
    TestScratchAllocationSuccess(state);
    TestPublishRequiresSuccessfulCreate(state);
    TestPublishWritesOriginalOutput(state);
    TestPublishRejectsUnsafeOutput(state);
    std::printf("fmod_host_decode_contract_test: %s (%d checks)\n",
                state.failures ? "FAIL" : "PASS", state.checks);
    return state.failures ? 1 : 0;
}
