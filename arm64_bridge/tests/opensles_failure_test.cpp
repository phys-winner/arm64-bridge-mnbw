#include "../src/opensles_host.h"

#include <cstdio>
#include <vector>

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); ++failures; \
} } while (0)

enum Step {
    None, EngineCreate, EngineRealize, EngineGet, MixCreate, MixRealize,
    PlayerCreate, ConfigGet, ConfigSet, PlayerRealize, PlayGet, QueueGet,
    VolumeGet, Register, Enqueue, Clear, QueueState, PlaySet, PlayState, VolumeSet
};
Step fail_step = None, null_step = None;
SLresult injected_result = SL_RESULT_RESOURCE_ERROR;
bool create_on_failure = false;
bool player_realized = false;
bool requested_volume = false, requested_config = false;
std::vector<int> destroyed;
SLuint32 queue_count = 0;
unsigned enqueue_calls = 0;
const void* submitted_buffer = nullptr;
SLuint32 submitted_size = 0;
slAndroidSimpleBufferQueueCallback registered_callback = nullptr;

SLObjectItf_ object_methods{};
const SLObjectItf_* engine_table = &object_methods;
const SLObjectItf_* mix_table = &object_methods;
const SLObjectItf_* player_table = &object_methods;
SLEngineItf_ engine_methods{};
const SLEngineItf_* engine_interface_table = &engine_methods;
SLPlayItf_ play_methods{};
const SLPlayItf_* play_table = &play_methods;
SLAndroidSimpleBufferQueueItf_ queue_methods{};
const SLAndroidSimpleBufferQueueItf_* queue_table = &queue_methods;
SLVolumeItf_ volume_methods{};
const SLVolumeItf_* volume_table = &volume_methods;
SLAndroidConfigurationItf_ config_methods{};
const SLAndroidConfigurationItf_* config_table = &config_methods;

SLresult Result(Step step) { return step == fail_step ? injected_result : SL_RESULT_SUCCESS; }

void Destroy(SLObjectItf object) {
    destroyed.push_back(object == &engine_table ? 1 : object == &mix_table ? 2 : 3);
    if (object == &player_table) registered_callback = nullptr;
}

SLresult CreateEngine(SLObjectItf* output, SLuint32 options, const SLEngineOption*,
                      SLuint32 count, const SLInterfaceID*, const SLboolean*) {
    CHECK(options == 0 && count == 0);
    const SLresult result = Result(EngineCreate);
    *output = null_step != EngineCreate && (result == SL_RESULT_SUCCESS || create_on_failure)
        ? &engine_table : nullptr;
    return result;
}

SLresult Realize(SLObjectItf object, SLboolean async) {
    CHECK(async == SL_BOOLEAN_FALSE);
    const Step step = object == &engine_table ? EngineRealize
                    : object == &mix_table ? MixRealize : PlayerRealize;
    const SLresult result = Result(step);
    if (step == PlayerRealize && result == SL_RESULT_SUCCESS) player_realized = true;
    return result;
}

SLresult GetInterface(SLObjectItf object, SLInterfaceID iid, void* output) {
    Step step = None;
    if (object == &engine_table && iid == SL_IID_ENGINE) step = EngineGet;
    else if (iid == SL_IID_ANDROIDCONFIGURATION) step = ConfigGet;
    else if (iid == SL_IID_PLAY) step = PlayGet;
    else if (iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) step = QueueGet;
    else if (iid == SL_IID_VOLUME) step = VolumeGet;
    CHECK(step != None);
    const SLresult result = Result(step);
    const bool available = result == SL_RESULT_SUCCESS && null_step != step;
    if (step == EngineGet) *static_cast<SLEngineItf*>(output) = available ? &engine_interface_table : nullptr;
    if (step == ConfigGet) {
        CHECK(!player_realized);
        *static_cast<SLAndroidConfigurationItf*>(output) = available ? &config_table : nullptr;
    }
    if (step == PlayGet) *static_cast<SLPlayItf*>(output) = available ? &play_table : nullptr;
    if (step == QueueGet) *static_cast<SLAndroidSimpleBufferQueueItf*>(output) = available ? &queue_table : nullptr;
    if (step == VolumeGet) *static_cast<SLVolumeItf*>(output) = available ? &volume_table : nullptr;
    return result;
}

SLresult CreateMix(SLEngineItf, SLObjectItf* output, SLuint32 count,
                   const SLInterfaceID*, const SLboolean*) {
    CHECK(count == 0);
    const SLresult result = Result(MixCreate);
    *output = null_step != MixCreate && (result == SL_RESULT_SUCCESS || create_on_failure)
        ? &mix_table : nullptr;
    return result;
}

SLresult CreatePlayer(SLEngineItf, SLObjectItf* output, SLDataSource* source, SLDataSink* sink,
                      SLuint32 count, const SLInterfaceID* ids, const SLboolean* required) {
    CHECK(count == 4);
    CHECK(ids[0] == SL_IID_ANDROIDSIMPLEBUFFERQUEUE && required[0] == SL_BOOLEAN_TRUE);
    CHECK(ids[1] == SL_IID_PLAY && required[1] == SL_BOOLEAN_TRUE);
    requested_volume = required[2] == SL_BOOLEAN_TRUE;
    requested_config = required[3] == SL_BOOLEAN_TRUE;
    CHECK(static_cast<SLDataLocator_AndroidSimpleBufferQueue*>(source->pLocator)->numBuffers == opensles::kQueueCapacity);
    CHECK(static_cast<SLDataLocator_OutputMix*>(sink->pLocator)->outputMix == &mix_table);
    const auto* pcm = static_cast<SLDataFormat_PCM*>(source->pFormat);
    CHECK(pcm->samplesPerSec == 24000000 && pcm->numChannels == 2 && pcm->bitsPerSample == 16);
    const SLresult result = Result(PlayerCreate);
    *output = null_step != PlayerCreate && (result == SL_RESULT_SUCCESS || create_on_failure)
        ? &player_table : nullptr;
    return result;
}

SLresult SetConfiguration(SLAndroidConfigurationItf, const SLchar* key,
                          const void* value, SLuint32 size) {
    CHECK(!player_realized);
    CHECK(std::strcmp(reinterpret_cast<const char*>(key),
                      reinterpret_cast<const char*>(SL_ANDROID_KEY_STREAM_TYPE)) == 0);
    CHECK(size == sizeof(SLint32) && *static_cast<const SLint32*>(value) == SL_ANDROID_STREAM_MEDIA);
    return Result(ConfigSet);
}

SLresult RegisterCallback(SLAndroidSimpleBufferQueueItf,
                          slAndroidSimpleBufferQueueCallback callback, void*) {
    CHECK(player_realized);
    const SLresult result = Result(Register);
    if (result == SL_RESULT_SUCCESS) registered_callback = callback;
    return result;
}

SLresult QueueEnqueue(SLAndroidSimpleBufferQueueItf, const void* buffer, SLuint32 size) {
    ++enqueue_calls;
    submitted_buffer = buffer;
    submitted_size = size;
    const SLresult result = Result(Enqueue);
    if (result == SL_RESULT_SUCCESS) ++queue_count;
    return result;
}

SLresult QueueClear(SLAndroidSimpleBufferQueueItf) {
    const SLresult result = Result(Clear);
    if (result == SL_RESULT_SUCCESS) queue_count = 0;
    return result;
}

SLresult QueueGetState(SLAndroidSimpleBufferQueueItf, SLAndroidSimpleBufferQueueState* state) {
    const SLresult result = Result(QueueState);
    if (result == SL_RESULT_SUCCESS) *state = {queue_count, 19};
    return result;
}

SLresult SetPlay(SLPlayItf, SLuint32) { return Result(PlaySet); }
SLresult GetPlay(SLPlayItf, SLuint32* output) {
    const SLresult result = Result(PlayState);
    if (result == SL_RESULT_SUCCESS) *output = SL_PLAYSTATE_PAUSED;
    return result;
}
SLresult SetVolume(SLVolumeItf, SLmillibel) { return Result(VolumeSet); }
void Callback(SLAndroidSimpleBufferQueueItf, void*) {}

void Reset() {
    fail_step = null_step = None;
    injected_result = SL_RESULT_RESOURCE_ERROR;
    create_on_failure = player_realized = false;
    requested_volume = requested_config = false;
    destroyed.clear();
    queue_count = enqueue_calls = 0;
    submitted_buffer = nullptr;
    submitted_size = 0;
    registered_callback = nullptr;
    object_methods.Realize = Realize;
    object_methods.GetInterface = GetInterface;
    object_methods.Destroy = Destroy;
    engine_methods.CreateOutputMix = CreateMix;
    engine_methods.CreateAudioPlayer = CreatePlayer;
    config_methods.SetConfiguration = SetConfiguration;
    queue_methods.RegisterCallback = RegisterCallback;
    queue_methods.Enqueue = QueueEnqueue;
    queue_methods.Clear = QueueClear;
    queue_methods.GetState = QueueGetState;
    play_methods.SetPlayState = SetPlay;
    play_methods.GetPlayState = GetPlay;
    volume_methods.SetVolumeLevel = SetVolume;
}

SLDataFormat_PCM Format() {
    return {SL_DATAFORMAT_PCM, 2, 24000000, SL_PCMSAMPLEFORMAT_FIXED_16,
            SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
            SL_BYTEORDER_LITTLEENDIAN};
}

bool EmptyEngine(const opensles::HostObjects& host) {
    return !host.engine && !host.engine_interface && !host.mix;
}
bool EmptyPlayer(const opensles::HostObjects& host) {
    return !host.player && !host.play && !host.queue && !host.volume &&
           !host.config && !host.media_configured;
}
void Cleanup(opensles::HostObjects& host) {
    if (host.player) (*host.player)->Destroy(host.player);
    if (host.mix) (*host.mix)->Destroy(host.mix);
    if (host.engine) (*host.engine)->Destroy(host.engine);
    host = {};
}

void EngineFailures() {
    for (Step step : {EngineCreate, EngineRealize, EngineGet, MixCreate, MixRealize}) {
        Reset();
        opensles::HostObjects host;
        fail_step = step;
        CHECK(opensles::InitializeEngine(host, CreateEngine) == injected_result);
        CHECK(EmptyEngine(host) && EmptyPlayer(host));
        const std::vector<int> expected = step == EngineCreate ? std::vector<int>{}
            : step == MixRealize ? std::vector<int>{2, 1} : std::vector<int>{1};
        CHECK(destroyed == expected);
        fail_step = None;
        CHECK(opensles::InitializeEngine(host, CreateEngine) == SL_RESULT_SUCCESS);
        CHECK(host.engine && host.engine_interface && host.mix);
        const size_t before = destroyed.size();
        CHECK(opensles::InitializeEngine(host, CreateEngine) == SL_RESULT_SUCCESS);
        CHECK(destroyed.size() == before);
        Cleanup(host);
    }
    for (Step step : {EngineCreate, EngineGet, MixCreate}) {
        Reset();
        opensles::HostObjects host;
        null_step = step;
        CHECK(opensles::InitializeEngine(host, CreateEngine) == SL_RESULT_INTERNAL_ERROR);
        CHECK(EmptyEngine(host));
    }
    Reset();
    opensles::HostObjects host;
    fail_step = MixCreate;
    create_on_failure = true;
    CHECK(opensles::InitializeEngine(host, CreateEngine) == injected_result);
    CHECK((destroyed == std::vector<int>{2, 1}));
}

void PlayerFailures() {
    for (Step step : {PlayerCreate, ConfigGet, ConfigSet, PlayerRealize, PlayGet, QueueGet, VolumeGet, Register}) {
        Reset();
        opensles::HostObjects host;
        CHECK(opensles::InitializeEngine(host, CreateEngine) == SL_RESULT_SUCCESS);
        fail_step = step;
        CHECK(opensles::InitializePlayer(host, Format(), true, true, Callback) == injected_result);
        CHECK(EmptyPlayer(host) && host.engine && host.mix);
        CHECK((destroyed == (step == PlayerCreate ? std::vector<int>{} : std::vector<int>{3})));
        fail_step = None;
        player_realized = false;
        CHECK(opensles::InitializePlayer(host, Format(), true, true, Callback) == SL_RESULT_SUCCESS);
        CHECK(host.player && host.play && host.queue && host.volume && host.config && host.media_configured);
        CHECK(registered_callback == Callback);
        Cleanup(host);
    }
    for (Step step : {PlayerCreate, ConfigGet, PlayGet, QueueGet, VolumeGet}) {
        Reset();
        opensles::HostObjects host;
        CHECK(opensles::InitializeEngine(host, CreateEngine) == SL_RESULT_SUCCESS);
        null_step = step;
        CHECK(opensles::InitializePlayer(host, Format(), true, true, Callback) == SL_RESULT_INTERNAL_ERROR);
        CHECK(EmptyPlayer(host));
        Cleanup(host);
    }
    Reset();
    opensles::HostObjects host;
    CHECK(opensles::InitializeEngine(host, CreateEngine) == SL_RESULT_SUCCESS);
    fail_step = PlayerCreate;
    create_on_failure = true;
    CHECK(opensles::InitializePlayer(host, Format(), true, true, Callback) == injected_result);
    CHECK((destroyed == std::vector<int>{3}));
    CHECK(EmptyPlayer(host));
    Cleanup(host);

    for (Step step : {VolumeGet, ConfigGet}) {
        Reset();
        CHECK(opensles::InitializeEngine(host, CreateEngine) == SL_RESULT_SUCCESS);
        null_step = step;
        CHECK(opensles::InitializePlayer(host, Format(), false, false, Callback) == SL_RESULT_INTERNAL_ERROR);
        CHECK(EmptyPlayer(host) && destroyed == std::vector<int>{3});
        Cleanup(host);
    }
    for (Step step : {VolumeGet, ConfigGet, ConfigSet}) {
        Reset();
        CHECK(opensles::InitializeEngine(host, CreateEngine) == SL_RESULT_SUCCESS);
        fail_step = step;
        CHECK(opensles::InitializePlayer(host, Format(), false, false, Callback) == injected_result);
        CHECK(EmptyPlayer(host) && destroyed == std::vector<int>{3});
        Cleanup(host);
    }
    for (Step step : {VolumeGet, ConfigGet, ConfigSet}) {
        Reset();
        CHECK(opensles::InitializeEngine(host, CreateEngine) == SL_RESULT_SUCCESS);
        fail_step = step;
        injected_result = SL_RESULT_FEATURE_UNSUPPORTED;
        CHECK(opensles::InitializePlayer(host, Format(), false, false, Callback) == SL_RESULT_SUCCESS);
        CHECK(!requested_volume && !requested_config && host.play && host.queue);
        CHECK(step == VolumeGet ? !host.volume : !host.config && !host.media_configured);
        Cleanup(host);
    }
    Reset();
    CHECK(opensles::InitializeEngine(host, CreateEngine) == SL_RESULT_SUCCESS);
    fail_step = VolumeGet;
    injected_result = SL_RESULT_FEATURE_UNSUPPORTED;
    CHECK(opensles::InitializePlayer(host, Format(), false, true, Callback) == SL_RESULT_SUCCESS);
    CHECK(!requested_volume && requested_config && host.media_configured && !host.volume);
    CHECK(opensles::InitializePlayer(host, Format(), true, true, Callback) == SL_RESULT_FEATURE_UNSUPPORTED);
    Cleanup(host);
    Reset();
    CHECK(opensles::InitializePlayer(host, Format(), false, true, Callback) == SL_RESULT_PRECONDITIONS_VIOLATED);
}

void QueueAndForwarding() {
    Reset();
    opensles::HostObjects host;
    CHECK(opensles::InitializeEngine(host, CreateEngine) == SL_RESULT_SUCCESS);
    CHECK(opensles::InitializePlayer(host, Format(), false, true, Callback) == SL_RESULT_SUCCESS);
    static uint8_t buffers[opensles::kQueueCapacity][opensles::kMaxBufferSize];
    uint8_t data[opensles::kMaxBufferSize + 1]{};
    data[0] = 41;
    size_t next = 0;
    for (size_t i = 0; i < opensles::kQueueCapacity; ++i) {
        CHECK(opensles::Enqueue(host.queue, data, 4, buffers, next) == SL_RESULT_SUCCESS);
        CHECK(submitted_buffer == buffers[i] && submitted_size == 4 &&
              std::memcmp(submitted_buffer, data, 4) == 0);
    }
    CHECK(next == 0 && queue_count == opensles::kQueueCapacity);
    data[0] = 99;
    CHECK(opensles::Enqueue(host.queue, data, 4, buffers, next) == SL_RESULT_BUFFER_INSUFFICIENT);
    CHECK(next == 0 && buffers[0][0] == 41 && enqueue_calls == opensles::kQueueCapacity);
    --queue_count; // hardware returns the oldest slot
    CHECK(opensles::Enqueue(host.queue, data, 4, buffers, next) == SL_RESULT_SUCCESS);
    CHECK(next == 1 && buffers[0][0] == 99);

    fail_step = Clear;
    CHECK(opensles::Clear(host, next) == injected_result);
    CHECK(next == 1 && queue_count == opensles::kQueueCapacity && registered_callback == Callback);
    fail_step = None;
    CHECK(opensles::Clear(host, next) == SL_RESULT_SUCCESS);
    CHECK(next == 0 && queue_count == 0 && registered_callback == Callback);
    buffers[0][0] = 17;
    fail_step = QueueState;
    CHECK(opensles::Enqueue(host.queue, data, 4, buffers, next) == injected_result);
    CHECK(next == 0 && buffers[0][0] == 17);
    fail_step = Enqueue;
    for (SLresult error : {SL_RESULT_BUFFER_INSUFFICIENT, SL_RESULT_RESOURCE_ERROR, SL_RESULT_PARAMETER_INVALID}) {
        injected_result = error;
        CHECK(opensles::Enqueue(host.queue, data, 4, buffers, next) == error);
        CHECK(next == 0 && queue_count == 0);
    }
    fail_step = None;
    CHECK(opensles::Enqueue(host.queue, data, opensles::kMaxBufferSize, buffers, next) == SL_RESULT_SUCCESS);
    CHECK(submitted_buffer == buffers[0] && submitted_size == opensles::kMaxBufferSize &&
          std::memcmp(submitted_buffer, data, opensles::kMaxBufferSize) == 0);
    CHECK(next == 1);
    CHECK(opensles::Enqueue(host.queue, data, sizeof(data), buffers, next) == SL_RESULT_PARAMETER_INVALID);
    CHECK(opensles::Enqueue(host.queue, nullptr, 4, buffers, next) == SL_RESULT_PARAMETER_INVALID);
    CHECK(opensles::Enqueue(host.queue, data, 0, buffers, next) == SL_RESULT_PARAMETER_INVALID);
    CHECK(opensles::Enqueue(nullptr, data, 4, buffers, next) == SL_RESULT_PRECONDITIONS_VIOLATED);
    CHECK(next == 1);

    injected_result = SL_RESULT_RESOURCE_ERROR;
    fail_step = PlaySet;
    CHECK(opensles::SetPlayState(host, SL_PLAYSTATE_PLAYING) == injected_result);
    fail_step = PlayState;
    SLuint32 state = 91;
    CHECK(opensles::GetPlayState(host, &state) == injected_result && state == 91);
    fail_step = QueueState;
    SLAndroidSimpleBufferQueueState queue_state{91, 92};
    CHECK(opensles::GetQueueState(host, &queue_state) == injected_result && queue_state.count == 91);
    fail_step = VolumeSet;
    CHECK(opensles::SetVolume(host, 0) == injected_result);
    fail_step = None;
    CHECK(opensles::GetPlayState(host, &state) == SL_RESULT_SUCCESS && state == SL_PLAYSTATE_PAUSED);
    CHECK(opensles::GetQueueState(host, &queue_state) == SL_RESULT_SUCCESS && queue_state.count == 1);
    Cleanup(host);
    CHECK(opensles::SetVolume(host, 0) == SL_RESULT_FEATURE_UNSUPPORTED);
    CHECK(opensles::Clear(host, next) == SL_RESULT_PRECONDITIONS_VIOLATED && next == 1);
    CHECK(opensles::SetPlayState(host, 3) == SL_RESULT_PRECONDITIONS_VIOLATED);
}

void GuestBounds() {
    CHECK(!opensles::GuestRange(0x1000, 0, 4));
    CHECK(opensles::GuestRange(0x1000, 0xffc, 4));
    CHECK(!opensles::GuestRange(0x1000, 0xffd, 4));
    CHECK(!opensles::GuestRange(0x1000, 0x1001, 0));
    CHECK(!opensles::GuestRange(0x1000, 1, UINT64_MAX));
    CHECK(opensles::GuestRange(0x100000000ULL, 0xfffffffc, 4));
    CHECK(!opensles::GuestRange(0x100000000ULL, 0xfffffffc, 8));
    CHECK(!opensles::GuestRange(0x80000000ULL, 0xfffffffc, 4));
    CHECK(!opensles::GuestRange(0x1000, 0xff0, sizeof(SLDataFormat_PCM)));
}
}  // namespace

int main() {
    EngineFailures();
    PlayerFailures();
    QueueAndForwarding();
    GuestBounds();
    if (failures) {
        std::fprintf(stderr, "OpenSL failure check: %d FAILED\n", failures);
        return 1;
    }
    std::puts("OpenSL failure check: PASS (rollback/retry, required/optional/null interfaces, full queue ownership, host errors, guest bounds)");
    return 0;
}
