#pragma once

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

// The production host boundary is also used by the Android failure-injection check.
namespace opensles {

inline constexpr size_t kQueueCapacity = 128;
inline constexpr size_t kMaxBufferSize = 16384;

inline bool GuestRange(size_t arena_size, uint32_t address, uint64_t bytes) {
    return address != 0 && address <= arena_size && bytes <= arena_size - address;
}

struct HostObjects {
    SLObjectItf engine = nullptr;
    SLEngineItf engine_interface = nullptr;
    SLObjectItf mix = nullptr;
    SLObjectItf player = nullptr;
    SLPlayItf play = nullptr;
    SLAndroidSimpleBufferQueueItf queue = nullptr;
    SLVolumeItf volume = nullptr;
    SLAndroidConfigurationItf config = nullptr;
    bool media_configured = false;
};

template <typename Interface>
inline SLresult InterfaceResult(SLresult result, Interface value) {
    if (result != SL_RESULT_SUCCESS) return result;
    return value && *value ? SL_RESULT_SUCCESS : SL_RESULT_INTERNAL_ERROR;
}

inline SLresult InitializeMix(HostObjects& out) {
    if (out.mix) return SL_RESULT_SUCCESS;
    if (!out.engine_interface) return SL_RESULT_PRECONDITIONS_VIOLATED;
    SLObjectItf mix = nullptr;
    SLresult result = (*out.engine_interface)->CreateOutputMix(out.engine_interface,
                                                              &mix, 0, nullptr, nullptr);
    result = InterfaceResult(result, mix);
    if (result == SL_RESULT_SUCCESS) result = (*mix)->Realize(mix, SL_BOOLEAN_FALSE);
    if (result == SL_RESULT_SUCCESS) out.mix = mix;
    else if (mix && *mix) (*mix)->Destroy(mix);
    return result;
}

inline SLresult InitializeEngine(HostObjects& out,
                                decltype(&slCreateEngine) create_engine = slCreateEngine) {
    if (out.engine) {
        return out.engine_interface && out.mix ? SL_RESULT_SUCCESS
                                              : SL_RESULT_PRECONDITIONS_VIOLATED;
    }
    HostObjects pending;
    auto fail = [&](SLresult result) {
        if (pending.mix && *pending.mix) (*pending.mix)->Destroy(pending.mix);
        if (pending.engine && *pending.engine) (*pending.engine)->Destroy(pending.engine);
        return result;
    };
    SLresult result = create_engine(&pending.engine, 0, nullptr, 0, nullptr, nullptr);
    result = InterfaceResult(result, pending.engine);
    if (result != SL_RESULT_SUCCESS) return fail(result);
    result = (*pending.engine)->Realize(pending.engine, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) return fail(result);
    result = (*pending.engine)->GetInterface(pending.engine, SL_IID_ENGINE,
                                           &pending.engine_interface);
    result = InterfaceResult(result, pending.engine_interface);
    if (result != SL_RESULT_SUCCESS) return fail(result);
    result = InitializeMix(pending);
    if (result != SL_RESULT_SUCCESS) return fail(result);
    out.engine = pending.engine;
    out.engine_interface = pending.engine_interface;
    out.mix = pending.mix;
    return SL_RESULT_SUCCESS;
}

inline SLresult InitializePlayer(HostObjects& out, SLDataFormat_PCM format,
                                bool volume_required, bool config_required,
                                slAndroidSimpleBufferQueueCallback callback,
                                void* callback_context = nullptr) {
    if (!out.engine_interface || !out.mix) return SL_RESULT_PRECONDITIONS_VIOLATED;
    if (out.player) {
        if (!out.play || !out.queue) return SL_RESULT_PRECONDITIONS_VIOLATED;
        if ((volume_required && !out.volume) || (config_required && !out.media_configured)) {
            return SL_RESULT_FEATURE_UNSUPPORTED;
        }
        return SL_RESULT_SUCCESS;
    }
    HostObjects pending;
    auto fail = [&](SLresult result) {
        if (pending.player && *pending.player) (*pending.player)->Destroy(pending.player);
        return result;
    };
    SLDataLocator_AndroidSimpleBufferQueue locator = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, static_cast<SLuint32>(kQueueCapacity)};
    SLDataSource source = {&locator, &format};
    SLDataLocator_OutputMix mix = {SL_DATALOCATOR_OUTPUTMIX, out.mix};
    SLDataSink sink = {&mix, nullptr};
    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_PLAY,
                                 SL_IID_VOLUME, SL_IID_ANDROIDCONFIGURATION};
    const SLboolean required[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE,
                                  volume_required ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE,
                                  config_required ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE};
    SLresult result = (*out.engine_interface)->CreateAudioPlayer(
        out.engine_interface, &pending.player, &source, &sink, 4, ids, required);
    result = InterfaceResult(result, pending.player);
    if (result != SL_RESULT_SUCCESS) return fail(result);

    // FMOD sets MEDIA before guest Realize; the bridge realizes eagerly, so do it here.
    result = (*pending.player)->GetInterface(pending.player, SL_IID_ANDROIDCONFIGURATION,
                                             &pending.config);
    if (result == SL_RESULT_SUCCESS) {
        result = InterfaceResult(result, pending.config);
        if (result != SL_RESULT_SUCCESS) return fail(result);
        const SLint32 stream_type = SL_ANDROID_STREAM_MEDIA;
        result = (*pending.config)->SetConfiguration(pending.config, SL_ANDROID_KEY_STREAM_TYPE,
                                                     &stream_type, sizeof(stream_type));
        if (result == SL_RESULT_SUCCESS) pending.media_configured = true;
        else if (config_required || result != SL_RESULT_FEATURE_UNSUPPORTED) return fail(result);
        else pending.config = nullptr;
    } else if (config_required || result != SL_RESULT_FEATURE_UNSUPPORTED) {
        return fail(result);
    } else {
        pending.config = nullptr;
    }
    result = (*pending.player)->Realize(pending.player, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) return fail(result);
    result = (*pending.player)->GetInterface(pending.player, SL_IID_PLAY, &pending.play);
    result = InterfaceResult(result, pending.play);
    if (result != SL_RESULT_SUCCESS) return fail(result);
    result = (*pending.player)->GetInterface(pending.player, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                             &pending.queue);
    result = InterfaceResult(result, pending.queue);
    if (result != SL_RESULT_SUCCESS) return fail(result);
    result = (*pending.player)->GetInterface(pending.player, SL_IID_VOLUME, &pending.volume);
    if (result == SL_RESULT_SUCCESS) {
        result = InterfaceResult(result, pending.volume);
        if (result != SL_RESULT_SUCCESS) return fail(result);
    } else if (volume_required || result != SL_RESULT_FEATURE_UNSUPPORTED) {
        return fail(result);
    } else {
        pending.volume = nullptr;
    }
    result = (*pending.queue)->RegisterCallback(pending.queue, callback, callback_context);
    if (result != SL_RESULT_SUCCESS) return fail(result);
    out.player = pending.player;
    out.play = pending.play;
    out.queue = pending.queue;
    out.volume = pending.volume;
    out.config = pending.config;
    out.media_configured = pending.media_configured;
    return SL_RESULT_SUCCESS;
}

inline SLresult SetPlayState(const HostObjects& host, SLuint32 state) {
    return host.play ? (*host.play)->SetPlayState(host.play, state) : SL_RESULT_PRECONDITIONS_VIOLATED;
}

inline SLresult GetPlayState(const HostObjects& host, SLuint32* state) {
    return host.play ? (*host.play)->GetPlayState(host.play, state) : SL_RESULT_PRECONDITIONS_VIOLATED;
}

inline SLresult Clear(const HostObjects& host, size_t& next) {
    const SLresult result = host.queue ? (*host.queue)->Clear(host.queue) : SL_RESULT_PRECONDITIONS_VIOLATED;
    if (result == SL_RESULT_SUCCESS) next = 0;
    return result;
}

inline SLresult GetQueueState(const HostObjects& host, SLAndroidSimpleBufferQueueState* state) {
    return host.queue ? (*host.queue)->GetState(host.queue, state) : SL_RESULT_PRECONDITIONS_VIOLATED;
}

inline SLresult SetVolume(const HostObjects& host, SLmillibel level) {
    return host.volume ? (*host.volume)->SetVolumeLevel(host.volume, level) : SL_RESULT_FEATURE_UNSUPPORTED;
}

// Caller serializes this and Clear with the same mutex. A failed submission must
// neither advance the producer nor overwrite a slot the hardware still owns.
inline SLresult Enqueue(SLAndroidSimpleBufferQueueItf queue, const void* data, uint32_t size,
                        uint8_t (&buffers)[kQueueCapacity][kMaxBufferSize], size_t& next) {
    if (!data || size == 0 || size > kMaxBufferSize) return SL_RESULT_PARAMETER_INVALID;
    if (!queue) return SL_RESULT_PRECONDITIONS_VIOLATED;
    SLAndroidSimpleBufferQueueState state{};
    SLresult result = (*queue)->GetState(queue, &state);
    if (result != SL_RESULT_SUCCESS) return result;
    if (state.count >= kQueueCapacity) return SL_RESULT_BUFFER_INSUFFICIENT;
    memcpy(buffers[next], data, size);
    result = (*queue)->Enqueue(queue, buffers[next], size);
    if (result == SL_RESULT_SUCCESS) next = (next + 1) % kQueueCapacity;
    return result;
}

}  // namespace opensles
