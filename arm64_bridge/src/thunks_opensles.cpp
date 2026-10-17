#include "thunks_opensles.h"
#include "thunk_manager.h"
#include "dynarmic_host.h"
#include "thunk_types.h"
#include "opensles_host.h"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <android/log.h>
#include <cstring>
#include <vector>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <algorithm>
#include <limits>

#if defined(__ANDROID__)
#include <sys/resource.h>
#endif

#define LOG_TAG "Arm32Bridge_Audio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

DynarmicHost* g_opensles_host = nullptr;

// ponytail: one FMOD output player, as before; independent players need separate state.
opensles::HostObjects g_hostAudio;
SLObjectItf& g_hostEngineObject = g_hostAudio.engine;
SLEngineItf& g_hostEngineEngine = g_hostAudio.engine_interface;
SLObjectItf& g_hostOutputMixObject = g_hostAudio.mix;
SLObjectItf& g_hostPlayerObject = g_hostAudio.player;
SLPlayItf& g_hostPlayerPlay = g_hostAudio.play;
SLAndroidSimpleBufferQueueItf& g_hostPlayerBufferQueue = g_hostAudio.queue;
SLVolumeItf& g_hostPlayerVolume = g_hostAudio.volume;

// Bounded process-lifetime singleton shells, reused after host replacement as before.
// Guest handles are invalid after Destroy; HAL tokens, not shell addresses, identify owners.
uint32_t g_guestEngineObjectAddr = 0;
uint32_t g_guestEngineVtblAddr = 0;
uint32_t g_guestEngineInterfaceAddr = 0;
uint32_t g_guestEngineInterfaceVtblAddr = 0;

uint32_t g_guestOutputMixObjectAddr = 0;
uint32_t g_guestOutputMixVtblAddr = 0;

uint32_t g_guestPlayerObjectAddr = 0;
uint32_t g_guestPlayerVtblAddr = 0;
uint32_t g_guestPlayInterfaceAddr = 0;
uint32_t g_guestPlayInterfaceVtblAddr = 0;
uint32_t g_guestBufferQueueInterfaceAddr = 0;
uint32_t g_guestBufferQueueInterfaceVtblAddr = 0;
uint32_t g_guestVolumeInterfaceAddr = 0;
uint32_t g_guestVolumeInterfaceVtblAddr = 0;

// High-capacity ring buffer for OpenSL ES hardware audio stream (128 buffers = ~2.7s cushion)
static constexpr size_t BUFFER_QUEUE_COUNT = opensles::kQueueCapacity;
static constexpr size_t MAX_BUFFER_SIZE = opensles::kMaxBufferSize;
static uint8_t g_hostAudioBuffers[BUFFER_QUEUE_COUNT][MAX_BUFFER_SIZE];
static size_t g_currentBufferIndex = 0;
static std::mutex g_audioMutex;

static uint32_t g_guestCallbackAddr = 0;
static uint32_t g_guestCallbackContext = 0;

static uint32_t g_guestConfigInterfaceAddr = 0;
static uint32_t g_guestConfigInterfaceVtblAddr = 0;

static std::unordered_map<std::string, uint32_t> g_iid_pointer_map;
static std::atomic<uint32_t> g_bqCallbackPending{0};

// Dispatch owns guest callback lifetime; the short state lock never spans guest/HAL calls.
// Recursive only for callback-self Destroy/Register; never wait here with audio/host locks.
static std::recursive_mutex g_dispatchMutex;
static std::mutex g_callbackMutex;
static uintptr_t g_nextAudioToken = 0;
static uintptr_t g_engineToken = 0, g_mixToken = 0, g_playerToken = 0;
static unsigned g_retiring = 0;  // Concurrent Destroy requests keep the gate closed together.

static uintptr_t NextAudioToken() {
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    if (g_nextAudioToken == std::numeric_limits<uintptr_t>::max()) return 0;
    return ++g_nextAudioToken;
}

static bool AudioRetiring() {
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    return g_retiring != 0;
}

// Background Audio Thread synchronization
static std::atomic<bool> g_audioThreadRunning{false};
static std::atomic<bool> g_audioPaused{false};
static std::mutex g_audioCvMutex;
static std::condition_variable g_audioCv;
static std::unique_ptr<std::thread> g_audioThread;

// Host OpenSL ES Buffer Queue Callback (fired on real-time audio thread by Android HAL)
static void HostBqPlayerCallback(SLAndroidSimpleBufferQueueItf, void* context) {
    {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        const uintptr_t token = reinterpret_cast<uintptr_t>(context);
        if (!token || token != g_playerToken || g_retiring || !g_guestCallbackAddr) return;
        if (g_bqCallbackPending.load(std::memory_order_relaxed) != UINT32_MAX) {
            g_bqCallbackPending.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_audioCv.notify_one();
}

bool GuestRange(DynarmicHost& host, uint32_t address, uint64_t bytes) {
    return opensles::GuestRange(host.GetGuestMemorySize(), address, bytes);
}

bool IID_Matches(DynarmicHost& host, uint32_t iid_ptr, const SLInterfaceID host_iid) {
    if (!host_iid) return false;
    if (GuestRange(host, iid_ptr, sizeof(SLInterfaceID_)) &&
        memcmp(host.GetGuestMemoryBase() + iid_ptr, host_iid, sizeof(SLInterfaceID_)) == 0) {
        return true;
    }
    if (GuestRange(host, iid_ptr, 4)) {
        const uint32_t deref = host.MemoryRead32(iid_ptr);
        if (GuestRange(host, deref, sizeof(SLInterfaceID_)) &&
            memcmp(host.GetGuestMemoryBase() + deref, host_iid, sizeof(SLInterfaceID_)) == 0) {
            return true;
        }
    }
    return false;
}

constexpr uint32_t kEngine = 1, kQueue = 2, kPlay = 4, kVolume = 8, kConfig = 16;

SLresult ValidateInterfaces(DynarmicHost& host, uint32_t count, uint32_t ids,
                            uint32_t required, uint32_t supported, uint32_t& requirements) {
    requirements = 0;
    // ponytail: bounded list for the singleton bridge; FMOD requests two interfaces.
    if (count > 32 || (count && (!GuestRange(host, ids, uint64_t(count) * 4) ||
                                !GuestRange(host, required, uint64_t(count) * 4)))) {
        return SL_RESULT_PARAMETER_INVALID;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t iid = host.MemoryRead32(ids + i * 4);
        const uint32_t need = host.MemoryRead32(required + i * 4);
        if (need > SL_BOOLEAN_TRUE || !GuestRange(host, iid, sizeof(SLInterfaceID_))) {
            return SL_RESULT_PARAMETER_INVALID;
        }
        uint32_t kind = 0;
        if (IID_Matches(host, iid, SL_IID_ENGINE)) kind = kEngine;
        else if (IID_Matches(host, iid, SL_IID_ANDROIDSIMPLEBUFFERQUEUE) ||
                 IID_Matches(host, iid, SL_IID_BUFFERQUEUE)) kind = kQueue;
        else if (IID_Matches(host, iid, SL_IID_PLAY)) kind = kPlay;
        else if (IID_Matches(host, iid, SL_IID_VOLUME)) kind = kVolume;
        else if (IID_Matches(host, iid, SL_IID_ANDROIDCONFIGURATION)) kind = kConfig;
        if (need && !(kind & supported)) return SL_RESULT_FEATURE_UNSUPPORTED;
        if (need) requirements |= kind;
    }
    return SL_RESULT_SUCCESS;
}

bool AllocGuestObject(DynarmicHost& host, uint32_t& object, uint32_t& table) {
    const uint32_t new_table = host.GuestAlloc(64 * 4, 8);
    const uint32_t new_object = new_table ? host.GuestAlloc(16, 8) : 0;
    if (!new_object) {
        if (new_table) host.GuestFree(new_table);
        return false;
    }
    table = new_table;
    object = new_object;
    host.MemoryWrite32(object, table);
    return true;
}

SLObjectItf HostObject(uint32_t guest) {
    if (guest && guest == g_guestEngineObjectAddr) return g_hostEngineObject;
    if (guest && guest == g_guestOutputMixObjectAddr) return g_hostOutputMixObject;
    if (guest && guest == g_guestPlayerObjectAddr) return g_hostPlayerObject;
    return nullptr;
}

static void DestroyGuestObject(uint32_t guest) {
    unsigned kind;
    uintptr_t expected;
    {
        std::lock_guard<std::mutex> audio(g_audioMutex);
        std::lock_guard<std::mutex> state(g_callbackMutex);
        if (guest && guest == g_guestEngineObjectAddr) { kind = 1; expected = g_engineToken; }
        else if (guest && guest == g_guestOutputMixObjectAddr) { kind = 2; expected = g_mixToken; }
        else if (guest && guest == g_guestPlayerObjectAddr) { kind = 3; expected = g_playerToken; }
        else return;
        if (!expected) return;
        ++g_retiring;
        g_bqCallbackPending.store(0, std::memory_order_relaxed);
        g_guestCallbackAddr = g_guestCallbackContext = 0;
    }
    // Seal first, then wait without audio/state/host locks. Self-close is reentrant.
    std::lock_guard<std::recursive_mutex> dispatch(g_dispatchMutex);
    opensles::HostObjects retired;
    {
        std::lock_guard<std::mutex> audio(g_audioMutex);
        std::lock_guard<std::mutex> state(g_callbackMutex);
        const uintptr_t current = kind == 1 ? g_engineToken : kind == 2 ? g_mixToken : g_playerToken;
        if (current == expected) {
            retired.player = g_hostAudio.player;
            g_hostAudio.player = nullptr;
            g_hostAudio.play = nullptr;
            g_hostAudio.queue = nullptr;
            g_hostAudio.volume = nullptr;
            g_hostAudio.config = nullptr;
            g_hostAudio.media_configured = false;
            g_playerToken = 0;
            g_currentBufferIndex = 0;
            if (kind <= 2) {
                retired.mix = g_hostAudio.mix;
                g_hostAudio.mix = nullptr;
                g_mixToken = 0;
            }
            if (kind == 1) {
                retired.engine = g_hostAudio.engine;
                g_hostAudio.engine = nullptr;
                g_hostAudio.engine_interface = nullptr;
                g_engineToken = 0;
            }
        }
    }
    // HAL may issue its last completion here; no callback-needed lock is held.
    // Creation remains sealed until hardware relinquishes the shared PCM storage.
    if (retired.player) (*retired.player)->Destroy(retired.player);
    if (retired.mix) (*retired.mix)->Destroy(retired.mix);
    if (retired.engine) (*retired.engine)->Destroy(retired.engine);
    {
        std::lock_guard<std::mutex> state(g_callbackMutex);
        --g_retiring;
    }
    LOGI("SLObject_Destroy retired guest=0x%08X owner=%zu", guest, size_t(expected));
}

static uint32_t AllocAndRegisterIID(ThunkManager& manager, DynarmicHost& host, const std::string& name, const SLInterfaceID host_iid) {
    // 1. Allocate 16 bytes for struct SLInterfaceID_
    uint32_t struct_addr = host.GuestAlloc(sizeof(struct SLInterfaceID_), 4);
    if (host_iid) {
        memcpy(host.GetGuestMemoryBase() + struct_addr, host_iid, sizeof(struct SLInterfaceID_));
    }

    // 2. Allocate 4 bytes for pointer to struct
    uint32_t ptr_addr = host.GuestAlloc(4, 4);
    host.MemoryWrite32(ptr_addr, struct_addr);

    // 3. Register in ThunkManager and map
    manager.RegisterSymbol(name, ptr_addr);
    g_iid_pointer_map[name] = ptr_addr;

    LOGI("Registered OpenSL ES IID %s: ptr=0x%08X -> struct=0x%08X", name.c_str(), ptr_addr, struct_addr);
    return ptr_addr;
}

} // namespace

bool HasPendingAudioCallbacks() {
    return g_bqCallbackPending.load(std::memory_order_relaxed) > 0;
}

void PumpOpenSLESCallbacks(DynarmicHost& host) {
    if (g_audioPaused.load(std::memory_order_relaxed) || host.IsJitRunning()) return;
    std::lock_guard<std::recursive_mutex> dispatch(g_dispatchMutex);
    uint32_t callback, context, queue;
    {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        if (g_retiring || !g_playerToken || !g_guestCallbackAddr ||
            !g_bqCallbackPending.load(std::memory_order_relaxed)) return;
        g_bqCallbackPending.fetch_sub(1, std::memory_order_relaxed);
        callback = g_guestCallbackAddr;
        context = g_guestCallbackContext;
        queue = g_guestBufferQueueInterfaceAddr;
    }
    // One leased invocation, never an unowned local batch. Destroy retires this lease.
    host.RunFunction(callback, {queue, context});
}

void PumpAudioWorkers(DynarmicHost&) {
    // FMOD workers run on real host threads through the threaded pthread thunk;
    // no cooperative worker list is needed in this host.
}

static void AudioThreadLoop(DynarmicHost* host) {
    LOGI("Audio callback thread started");
#if defined(__ANDROID__)
    setpriority(PRIO_PROCESS, 0, -16); // High priority audio thread
#endif

    while (g_audioThreadRunning.load(std::memory_order_relaxed)) {
        {
            std::unique_lock<std::mutex> lock(g_audioCvMutex);
            g_audioCv.wait_for(lock, std::chrono::milliseconds(5), []() {
                return !g_audioThreadRunning.load(std::memory_order_relaxed) ||
                       g_bqCallbackPending.load(std::memory_order_relaxed) > 0;
            });
        }

        if (!g_audioThreadRunning.load(std::memory_order_relaxed)) break;
        if (!host || g_audioPaused.load(std::memory_order_relaxed)) continue;

        if (g_bqCallbackPending.load(std::memory_order_relaxed) > 0) {
            PumpOpenSLESCallbacks(*host);
        }
    }

    LOGI("Dedicated audio thread stopped.");
}

void StartAudioThread(DynarmicHost* host) {
    if (g_audioThreadRunning.load(std::memory_order_relaxed)) return;
    if (!host) return;
    g_audioThreadRunning.store(true, std::memory_order_relaxed);
    g_audioPaused.store(false, std::memory_order_relaxed);
    g_audioThread = std::make_unique<std::thread>(AudioThreadLoop, host);
    LOGI("Audio thread launched successfully!");
}

void StopAudioThread() {
    if (!g_audioThreadRunning.load(std::memory_order_relaxed)) return;
    g_audioThreadRunning.store(false, std::memory_order_relaxed);
    g_audioCv.notify_all();
    if (g_audioThread && g_audioThread->joinable()) {
        g_audioThread->join();
    }
    g_audioThread.reset();
}

void PauseAudioPlayback() {
    LOGI("PauseAudioPlayback requested");
    g_audioPaused.store(true, std::memory_order_release);
    if (g_hostPlayerPlay) {
        (*g_hostPlayerPlay)->SetPlayState(g_hostPlayerPlay, SL_PLAYSTATE_PAUSED);
    }
}

void ResumeAudioPlayback() {
    LOGI("ResumeAudioPlayback requested");
    g_audioPaused.store(false, std::memory_order_release);
    if (g_hostPlayerPlay) {
        (*g_hostPlayerPlay)->SetPlayState(g_hostPlayerPlay, SL_PLAYSTATE_PLAYING);
    }
}

uint32_t GetOpenSLESInterfacePointer(const std::string& name) {
    auto it = g_iid_pointer_map.find(name);
    if (it != g_iid_pointer_map.end()) {
        return it->second;
    }
    return 0;
}

void RegisterOpenSLESThunks(ThunkManager& manager, DynarmicHost& host) {
    g_opensles_host = &host;
    LOGI("Registering OpenSL ES guest bridge thunks...");

    // Register all standard OpenSL ES interface IDs
    AllocAndRegisterIID(manager, host, "SL_IID_ENGINE", SL_IID_ENGINE);
    AllocAndRegisterIID(manager, host, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", SL_IID_ANDROIDSIMPLEBUFFERQUEUE);
    AllocAndRegisterIID(manager, host, "SL_IID_ANDROIDCONFIGURATION", SL_IID_ANDROIDCONFIGURATION);
    AllocAndRegisterIID(manager, host, "SL_IID_PLAY", SL_IID_PLAY);
    AllocAndRegisterIID(manager, host, "SL_IID_VOLUME", SL_IID_VOLUME);
    AllocAndRegisterIID(manager, host, "SL_IID_OUTPUTMIX", SL_IID_OUTPUTMIX);
    AllocAndRegisterIID(manager, host, "SL_IID_BUFFERQUEUE", SL_IID_BUFFERQUEUE);
    AllocAndRegisterIID(manager, host, "SL_IID_RECORD", SL_IID_RECORD);

    // 1. slCreateEngine
    manager.RegisterThunk("slCreateEngine", [&host](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
        uint32_t pEngine = jit.Regs()[0];
        LOGI("slCreateEngine called! pEngine=0x%08X", pEngine);

        if (!GuestRange(host, pEngine, 4)) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        host.MemoryWrite32(pEngine, 0);
        const uint32_t options = jit.Regs()[1], count = jit.Regs()[3], sp = jit.Regs()[13];
        if ((options && !GuestRange(host, jit.Regs()[2], uint64_t(options) * 8)) ||
            (count && !GuestRange(host, sp, 8))) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        uint32_t requirements = 0;
        const SLresult validation = ValidateInterfaces(host, count,
            count ? host.MemoryRead32(sp) : 0, count ? host.MemoryRead32(sp + 4) : 0,
            kEngine, requirements);
        if (validation != SL_RESULT_SUCCESS || options != 0) {
            jit.Regs()[0] = validation != SL_RESULT_SUCCESS ? validation : SL_RESULT_FEATURE_UNSUPPORTED;
            return;
        }
        std::lock_guard<std::recursive_mutex> dispatch(g_dispatchMutex);
        std::lock_guard<std::mutex> lock(g_audioMutex);
        if (AudioRetiring() || g_hostEngineObject) {
            jit.Regs()[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
            return;
        }
        // Allocate the guest shell before creating any host resources.
        if (g_guestEngineObjectAddr == 0) {
            if (!AllocGuestObject(host, g_guestEngineObjectAddr, g_guestEngineVtblAddr)) {
                jit.Regs()[0] = SL_RESULT_MEMORY_FAILURE;
                return;
            }

            // Populate VTable with thunk addresses
            host.MemoryWrite32(g_guestEngineVtblAddr + 0 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLObject_Realize"));
            host.MemoryWrite32(g_guestEngineVtblAddr + 1 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLObject_Resume"));
            host.MemoryWrite32(g_guestEngineVtblAddr + 2 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLObject_GetState"));
            host.MemoryWrite32(g_guestEngineVtblAddr + 3 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLEngine_GetInterface"));
            host.MemoryWrite32(g_guestEngineVtblAddr + 4 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLObject_RegisterCallback"));
            host.MemoryWrite32(g_guestEngineVtblAddr + 5 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLObject_AbortAsyncOperation"));
            host.MemoryWrite32(g_guestEngineVtblAddr + 6 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLObject_Destroy"));
        }

        const uintptr_t token = NextAudioToken();
        const SLresult result = token ? opensles::InitializeEngine(g_hostAudio)
                                      : SL_RESULT_RESOURCE_ERROR;
        if (result == SL_RESULT_SUCCESS) {
            std::lock_guard<std::mutex> state(g_callbackMutex);
            g_engineToken = g_mixToken = token;
            host.MemoryWrite32(pEngine, g_guestEngineObjectAddr);
        } else LOGE("Host engine/output mix initialization failed: %u", result);
        jit.Regs()[0] = result;
    });

    // 2. Generic SLObject Thunks
    manager.RegisterThunk("SLObject_Realize", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(g_audioMutex);
        // Host objects are already realized by the transactional create path.
        jit.Regs()[0] = HostObject(jit.Regs()[0]) ? SL_RESULT_SUCCESS
                                               : SL_RESULT_PRECONDITIONS_VIOLATED;
    });

    manager.RegisterThunk("SLObject_Resume", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(g_audioMutex);
        SLObjectItf object = HostObject(jit.Regs()[0]);
        jit.Regs()[0] = object ? (*object)->Resume(object, jit.Regs()[1])
                              : SL_RESULT_PRECONDITIONS_VIOLATED;
    });

    manager.RegisterThunk("SLObject_GetState", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t output = jit.Regs()[1];
        if (!GuestRange(host, output, 4)) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        std::lock_guard<std::mutex> lock(g_audioMutex);
        SLObjectItf object = HostObject(jit.Regs()[0]);
        SLuint32 state = 0;
        const SLresult result = object ? (*object)->GetState(object, &state)
                                       : SL_RESULT_PRECONDITIONS_VIOLATED;
        if (result == SL_RESULT_SUCCESS) host.MemoryWrite32(output, state);
        jit.Regs()[0] = result;
    });

    manager.RegisterThunk("SLObject_RegisterCallback", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = SL_RESULT_SUCCESS;
    });

    manager.RegisterThunk("SLObject_AbortAsyncOperation", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    });

    manager.RegisterThunk("SLObject_Destroy", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        DestroyGuestObject(jit.Regs()[0]);
    });

    // 3. SLEngine_GetInterface
    manager.RegisterThunk("SLEngine_GetInterface", [&host](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
        uint32_t self = jit.Regs()[0];
        uint32_t iid_ptr = jit.Regs()[1];
        uint32_t pInterface = jit.Regs()[2];

        LOGI("SLEngine_GetInterface self=0x%08X, iid=0x%08X, pInterface=0x%08X", self, iid_ptr, pInterface);
        if (!GuestRange(host, pInterface, 4) || !GuestRange(host, iid_ptr, sizeof(SLInterfaceID_))) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        host.MemoryWrite32(pInterface, 0);
        std::lock_guard<std::mutex> lock(g_audioMutex);
        if (!self || self != g_guestEngineObjectAddr || !g_hostEngineEngine) {
            jit.Regs()[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
            return;
        }
        if (IID_Matches(host, iid_ptr, SL_IID_ENGINE)) {
            if (g_guestEngineInterfaceAddr == 0) {
                if (!AllocGuestObject(host, g_guestEngineInterfaceAddr, g_guestEngineInterfaceVtblAddr)) {
                    jit.Regs()[0] = SL_RESULT_MEMORY_FAILURE;
                    return;
                }

                // Populate SLEngineItf VTable
                host.MemoryWrite32(g_guestEngineInterfaceVtblAddr + 2 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLEngine_CreateAudioPlayer"));
                host.MemoryWrite32(g_guestEngineInterfaceVtblAddr + 7 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLEngine_CreateOutputMix"));
            }

            if (pInterface) {
                host.MemoryWrite32(pInterface, g_guestEngineInterfaceAddr);
            }
            jit.Regs()[0] = SL_RESULT_SUCCESS;
            return;
        }

        LOGI("SLEngine_GetInterface: unsupported IID 0x%08X", iid_ptr);
        jit.Regs()[0] = SL_RESULT_FEATURE_UNSUPPORTED;
    });

    // 4. SLEngine_CreateOutputMix
    manager.RegisterThunk("SLEngine_CreateOutputMix", [&host](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
        uint32_t pMix = jit.Regs()[1];
        LOGI("SLEngine_CreateOutputMix called! pMix=0x%08X", pMix);
        const uint32_t count = jit.Regs()[2], sp = jit.Regs()[13];
        if (!GuestRange(host, pMix, 4) || (count && !GuestRange(host, sp, 4))) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        host.MemoryWrite32(pMix, 0);
        uint32_t requirements = 0;
        const SLresult validation = ValidateInterfaces(host, count, jit.Regs()[3],
            count ? host.MemoryRead32(sp) : 0, 0, requirements);
        if (validation != SL_RESULT_SUCCESS) {
            jit.Regs()[0] = validation;
            return;
        }
        std::lock_guard<std::recursive_mutex> dispatch(g_dispatchMutex);
        std::lock_guard<std::mutex> lock(g_audioMutex);
        if (!jit.Regs()[0] || jit.Regs()[0] != g_guestEngineInterfaceAddr ||
            !g_hostEngineEngine || AudioRetiring()) {
            jit.Regs()[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
            return;
        }
        if (g_guestOutputMixObjectAddr == 0) {
            if (!AllocGuestObject(host, g_guestOutputMixObjectAddr, g_guestOutputMixVtblAddr)) {
                jit.Regs()[0] = SL_RESULT_MEMORY_FAILURE;
                return;
            }

            host.MemoryWrite32(g_guestOutputMixVtblAddr + 0 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLObject_Realize"));
            host.MemoryWrite32(g_guestOutputMixVtblAddr + 6 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLObject_Destroy"));
        }

        if (!g_hostOutputMixObject) {
            const uintptr_t token = NextAudioToken();
            const SLresult result = token ? opensles::InitializeMix(g_hostAudio)
                                          : SL_RESULT_RESOURCE_ERROR;
            if (result != SL_RESULT_SUCCESS) {
                jit.Regs()[0] = result;
                return;
            }
            std::lock_guard<std::mutex> state(g_callbackMutex);
            g_mixToken = token;
        }
        host.MemoryWrite32(pMix, g_guestOutputMixObjectAddr);
        jit.Regs()[0] = SL_RESULT_SUCCESS;
    });

    // 5. SLEngine_CreateAudioPlayer
    manager.RegisterThunk("SLEngine_CreateAudioPlayer", [&host](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
        uint32_t self = jit.Regs()[0];
        uint32_t pPlayer = jit.Regs()[1];
        uint32_t pAudioSrc = jit.Regs()[2];
        uint32_t pAudioSnk = jit.Regs()[3];
        const uint32_t sp = jit.Regs()[13];
        LOGI("SLEngine_CreateAudioPlayer called! pPlayer=0x%08X, pAudioSrc=0x%08X, pAudioSnk=0x%08X",
             pPlayer, pAudioSrc, pAudioSnk);
        if (!GuestRange(host, pPlayer, 4)) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        host.MemoryWrite32(pPlayer, 0);
        if (!GuestRange(host, sp, 12) || !GuestRange(host, pAudioSrc, 8) ||
            !GuestRange(host, pAudioSnk, 8)) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        uint32_t requirements = 0;
        const SLresult validation = ValidateInterfaces(host, host.MemoryRead32(sp),
            host.MemoryRead32(sp + 4), host.MemoryRead32(sp + 8),
            kQueue | kPlay | kVolume | kConfig, requirements);
        if (validation != SL_RESULT_SUCCESS) {
            jit.Regs()[0] = validation;
            return;
        }
        const uint32_t source_locator = host.MemoryRead32(pAudioSrc);
        const uint32_t format_address = host.MemoryRead32(pAudioSrc + 4);
        const uint32_t sink_locator = host.MemoryRead32(pAudioSnk);
        if (!GuestRange(host, source_locator, 8) || !GuestRange(host, sink_locator, 8) ||
            !GuestRange(host, format_address, sizeof(SLDataFormat_PCM))) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        SLDataFormat_PCM format_pcm{};
        static_assert(sizeof(format_pcm) == 28);
        memcpy(&format_pcm, host.GetGuestMemoryBase() + format_address, sizeof(format_pcm));
        const uint32_t locator_type = host.MemoryRead32(source_locator);
        if ((locator_type != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE &&
             locator_type != SL_DATALOCATOR_BUFFERQUEUE) ||
            host.MemoryRead32(sink_locator) != SL_DATALOCATOR_OUTPUTMIX ||
            host.MemoryRead32(pAudioSnk + 4) != 0 ||
            format_pcm.formatType != SL_DATAFORMAT_PCM ||
            format_pcm.bitsPerSample != SL_PCMSAMPLEFORMAT_FIXED_16 ||
            format_pcm.containerSize != SL_PCMSAMPLEFORMAT_FIXED_16 ||
            format_pcm.endianness != SL_BYTEORDER_LITTLEENDIAN) {
            jit.Regs()[0] = SL_RESULT_FEATURE_UNSUPPORTED;
            return;
        }
        if (host.MemoryRead32(source_locator + 4) == 0 ||
            format_pcm.numChannels == 0 || format_pcm.numChannels > 2 ||
            format_pcm.samplesPerSec == 0) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        std::lock_guard<std::recursive_mutex> dispatch(g_dispatchMutex);
        std::lock_guard<std::mutex> lock(g_audioMutex);
        if (!self || self != g_guestEngineInterfaceAddr || !g_hostEngineEngine ||
            !g_guestOutputMixObjectAddr || !g_hostOutputMixObject || g_hostPlayerObject ||
            AudioRetiring() || host.MemoryRead32(sink_locator + 4) != g_guestOutputMixObjectAddr) {
            jit.Regs()[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
            return;
        }
        if (g_guestPlayerObjectAddr == 0) {
            if (!AllocGuestObject(host, g_guestPlayerObjectAddr, g_guestPlayerVtblAddr)) {
                jit.Regs()[0] = SL_RESULT_MEMORY_FAILURE;
                return;
            }

            host.MemoryWrite32(g_guestPlayerVtblAddr + 0 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLObject_Realize"));
            host.MemoryWrite32(g_guestPlayerVtblAddr + 3 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLPlayer_GetInterface"));
            host.MemoryWrite32(g_guestPlayerVtblAddr + 6 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLObject_Destroy"));
        }

        const uintptr_t token = NextAudioToken();
        const SLresult result = token ? opensles::InitializePlayer(g_hostAudio, format_pcm,
            (requirements & kVolume) != 0, (requirements & kConfig) != 0,
            HostBqPlayerCallback, reinterpret_cast<void*>(token)) : SL_RESULT_RESOURCE_ERROR;
        if (result == SL_RESULT_SUCCESS) {
            {
                std::lock_guard<std::mutex> state(g_callbackMutex);
                g_playerToken = token;
                g_bqCallbackPending.store(0, std::memory_order_relaxed);
                g_guestCallbackAddr = g_guestCallbackContext = 0;
            }
            host.MemoryWrite32(pPlayer, g_guestPlayerObjectAddr);
            LOGI("Host Audio Player ready: channels=%u rate=%u volume=%d config=%d",
                 format_pcm.numChannels, format_pcm.samplesPerSec,
                 g_hostAudio.volume != nullptr, g_hostAudio.media_configured);
        } else {
            LOGE("Host Audio Player initialization failed: %u", result);
        }
        jit.Regs()[0] = result;
    });

    // 6. SLPlayer_GetInterface
    manager.RegisterThunk("SLPlayer_GetInterface", [&host](Dynarmic::A32::Jit& jit, DynarmicHost& h) {
        uint32_t self = jit.Regs()[0];
        uint32_t iid_ptr = jit.Regs()[1];
        uint32_t pInterface = jit.Regs()[2];

        LOGI("SLPlayer_GetInterface self=0x%08X, iid=0x%08X, pInterface=0x%08X", self, iid_ptr, pInterface);
        if (!GuestRange(host, pInterface, 4) || !GuestRange(host, iid_ptr, sizeof(SLInterfaceID_))) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        host.MemoryWrite32(pInterface, 0);
        std::lock_guard<std::mutex> lock(g_audioMutex);
        if (!self || self != g_guestPlayerObjectAddr || !g_hostPlayerObject) {
            jit.Regs()[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
            return;
        }
        if ((IID_Matches(host, iid_ptr, SL_IID_PLAY) && !g_hostPlayerPlay) ||
            ((IID_Matches(host, iid_ptr, SL_IID_ANDROIDSIMPLEBUFFERQUEUE) ||
              IID_Matches(host, iid_ptr, SL_IID_BUFFERQUEUE)) && !g_hostPlayerBufferQueue) ||
            (IID_Matches(host, iid_ptr, SL_IID_VOLUME) && !g_hostPlayerVolume) ||
            (IID_Matches(host, iid_ptr, SL_IID_ANDROIDCONFIGURATION) && !g_hostAudio.media_configured)) {
            jit.Regs()[0] = SL_RESULT_FEATURE_UNSUPPORTED;
            return;
        }
        if (IID_Matches(host, iid_ptr, SL_IID_PLAY)) {
            LOGI("SLPlayer_GetInterface: matched SL_IID_PLAY");
            if (g_guestPlayInterfaceAddr == 0) {
                if (!AllocGuestObject(host, g_guestPlayInterfaceAddr, g_guestPlayInterfaceVtblAddr)) {
                    jit.Regs()[0] = SL_RESULT_MEMORY_FAILURE;
                    return;
                }

                host.MemoryWrite32(g_guestPlayInterfaceVtblAddr + 0 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLPlay_SetPlayState"));
                host.MemoryWrite32(g_guestPlayInterfaceVtblAddr + 1 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLPlay_GetPlayState"));
            }
            if (pInterface) host.MemoryWrite32(pInterface, g_guestPlayInterfaceAddr);
            jit.Regs()[0] = SL_RESULT_SUCCESS;
            return;
        }

        if (IID_Matches(host, iid_ptr, SL_IID_ANDROIDSIMPLEBUFFERQUEUE) || IID_Matches(host, iid_ptr, SL_IID_BUFFERQUEUE)) {
            LOGI("SLPlayer_GetInterface: matched SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
            if (g_guestBufferQueueInterfaceAddr == 0) {
                if (!AllocGuestObject(host, g_guestBufferQueueInterfaceAddr, g_guestBufferQueueInterfaceVtblAddr)) {
                    jit.Regs()[0] = SL_RESULT_MEMORY_FAILURE;
                    return;
                }

                host.MemoryWrite32(g_guestBufferQueueInterfaceVtblAddr + 0 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLBufferQueue_Enqueue"));
                host.MemoryWrite32(g_guestBufferQueueInterfaceVtblAddr + 1 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLBufferQueue_Clear"));
                host.MemoryWrite32(g_guestBufferQueueInterfaceVtblAddr + 2 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLBufferQueue_GetState"));
                host.MemoryWrite32(g_guestBufferQueueInterfaceVtblAddr + 3 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLBufferQueue_RegisterCallback"));
            }
            if (pInterface) host.MemoryWrite32(pInterface, g_guestBufferQueueInterfaceAddr);
            jit.Regs()[0] = SL_RESULT_SUCCESS;
            return;
        }

        if (IID_Matches(host, iid_ptr, SL_IID_VOLUME)) {
            LOGI("SLPlayer_GetInterface: matched SL_IID_VOLUME");
            if (g_guestVolumeInterfaceAddr == 0) {
                if (!AllocGuestObject(host, g_guestVolumeInterfaceAddr, g_guestVolumeInterfaceVtblAddr)) {
                    jit.Regs()[0] = SL_RESULT_MEMORY_FAILURE;
                    return;
                }

                host.MemoryWrite32(g_guestVolumeInterfaceVtblAddr + 0 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLVolume_SetVolumeLevel"));
            }
            if (pInterface) host.MemoryWrite32(pInterface, g_guestVolumeInterfaceAddr);
            jit.Regs()[0] = SL_RESULT_SUCCESS;
            return;
        }

        if (IID_Matches(host, iid_ptr, SL_IID_ANDROIDCONFIGURATION)) {
            LOGI("SLPlayer_GetInterface: matched SL_IID_ANDROIDCONFIGURATION");
            if (g_guestConfigInterfaceAddr == 0) {
                if (!AllocGuestObject(host, g_guestConfigInterfaceAddr, g_guestConfigInterfaceVtblAddr)) {
                    jit.Regs()[0] = SL_RESULT_MEMORY_FAILURE;
                    return;
                }

                host.MemoryWrite32(g_guestConfigInterfaceVtblAddr + 0 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLConfig_SetConfiguration"));
                host.MemoryWrite32(g_guestConfigInterfaceVtblAddr + 1 * 4, host.GetThunkManager().GetOrCreateThunkAddress("SLConfig_GetConfiguration"));
            }
            if (pInterface) host.MemoryWrite32(pInterface, g_guestConfigInterfaceAddr);
            jit.Regs()[0] = SL_RESULT_SUCCESS;
            return;
        }

        LOGI("SLPlayer_GetInterface: unsupported IID 0x%08X (returning FEATURE_UNSUPPORTED)", iid_ptr);
        jit.Regs()[0] = SL_RESULT_FEATURE_UNSUPPORTED;
    });

    auto config_key = [](DynarmicHost& host, uint32_t address) -> SLresult {
        if (!GuestRange(host, address, 1)) return SL_RESULT_PARAMETER_INVALID;
        const char* key = reinterpret_cast<const char*>(host.GetGuestMemoryBase() + address);
        const size_t available = std::min<size_t>(256, host.GetGuestMemorySize() - address);
        if (!memchr(key, 0, available)) return SL_RESULT_PARAMETER_INVALID;
        return strcmp(key, reinterpret_cast<const char*>(SL_ANDROID_KEY_STREAM_TYPE)) == 0
            ? SL_RESULT_SUCCESS : SL_RESULT_FEATURE_UNSUPPORTED;
    };
    manager.RegisterThunk("SLConfig_SetConfiguration", [config_key](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t value = jit.Regs()[2], size = jit.Regs()[3];
        SLresult result = config_key(host, jit.Regs()[1]);
        if (result == SL_RESULT_SUCCESS && (size != 4 || !GuestRange(host, value, size))) {
            result = SL_RESULT_PARAMETER_INVALID;
        }
        if (result != SL_RESULT_SUCCESS) {
            jit.Regs()[0] = result;
            return;
        }
        std::lock_guard<std::mutex> lock(g_audioMutex);
        if (!jit.Regs()[0] || jit.Regs()[0] != g_guestConfigInterfaceAddr || !g_hostAudio.media_configured) {
            jit.Regs()[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
            return;
        }
        // This exact configuration already succeeded before the eager host Realize.
        jit.Regs()[0] = host.MemoryRead32(value) == SL_ANDROID_STREAM_MEDIA
            ? SL_RESULT_SUCCESS : SL_RESULT_FEATURE_UNSUPPORTED;
    });

    manager.RegisterThunk("SLConfig_GetConfiguration", [config_key](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t size_pointer = jit.Regs()[2], value = jit.Regs()[3];
        SLresult result = config_key(host, jit.Regs()[1]);
        if (result == SL_RESULT_SUCCESS && !GuestRange(host, size_pointer, 4)) {
            result = SL_RESULT_PARAMETER_INVALID;
        }
        if (result != SL_RESULT_SUCCESS) {
            jit.Regs()[0] = result;
            return;
        }
        const uint32_t capacity = host.MemoryRead32(size_pointer);
        if (value && !GuestRange(host, value, capacity)) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        std::lock_guard<std::mutex> lock(g_audioMutex);
        if (!jit.Regs()[0] || jit.Regs()[0] != g_guestConfigInterfaceAddr || !g_hostAudio.media_configured) {
            jit.Regs()[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
            return;
        }
        host.MemoryWrite32(size_pointer, 4);
        if (value && capacity < 4) result = SL_RESULT_BUFFER_INSUFFICIENT;
        else if (value) host.MemoryWrite32(value, SL_ANDROID_STREAM_MEDIA);
        jit.Regs()[0] = result;
    });

    // 7. SLPlay Thunks
    manager.RegisterThunk("SLPlay_SetPlayState", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t state = jit.Regs()[1];
        LOGI("SLPlay_SetPlayState: %u", state);
        std::lock_guard<std::mutex> lock(g_audioMutex);
        jit.Regs()[0] = jit.Regs()[0] && jit.Regs()[0] == g_guestPlayInterfaceAddr
            ? opensles::SetPlayState(g_hostAudio, state) : SL_RESULT_PRECONDITIONS_VIOLATED;
    });

    manager.RegisterThunk("SLPlay_GetPlayState", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t pState = jit.Regs()[1];
        if (!GuestRange(host, pState, 4)) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        std::lock_guard<std::mutex> lock(g_audioMutex);
        SLuint32 state = 0;
        const SLresult result = jit.Regs()[0] && jit.Regs()[0] == g_guestPlayInterfaceAddr
            ? opensles::GetPlayState(g_hostAudio, &state) : SL_RESULT_PRECONDITIONS_VIOLATED;
        if (result == SL_RESULT_SUCCESS) host.MemoryWrite32(pState, state);
        jit.Regs()[0] = result;
    });

    // 8. SLBufferQueue Thunks
    manager.RegisterThunk("SLBufferQueue_RegisterCallback", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t callback = jit.Regs()[1];
        if (callback && !GuestRange(host, callback & ~1u, (callback & 1u) ? 2 : 4)) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        std::lock_guard<std::recursive_mutex> dispatch(g_dispatchMutex);
        std::lock_guard<std::mutex> lock(g_audioMutex);
        if (!jit.Regs()[0] || jit.Regs()[0] != g_guestBufferQueueInterfaceAddr ||
            !g_hostPlayerBufferQueue || AudioRetiring()) {
            jit.Regs()[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
            return;
        }
        {
            std::lock_guard<std::mutex> state(g_callbackMutex);
            g_guestCallbackAddr = callback;
            g_guestCallbackContext = jit.Regs()[2];
            g_bqCallbackPending.store(0, std::memory_order_relaxed);
        }
        LOGI("SLBufferQueue_RegisterCallback: callback=0x%08X, context=0x%08X",
             callback, jit.Regs()[2]);
        if (callback) StartAudioThread(&host);
        jit.Regs()[0] = SL_RESULT_SUCCESS;
    });

    manager.RegisterThunk("SLBufferQueue_Clear", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(g_audioMutex);
        // Clear releases buffer storage, not the guest's callback registration.
        const SLresult result = jit.Regs()[0] && jit.Regs()[0] == g_guestBufferQueueInterfaceAddr
            ? opensles::Clear(g_hostAudio, g_currentBufferIndex) : SL_RESULT_PRECONDITIONS_VIOLATED;
        jit.Regs()[0] = result;
    });

    manager.RegisterThunk("SLBufferQueue_GetState", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t pState = jit.Regs()[1];
        if (!GuestRange(host, pState, 8)) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        std::lock_guard<std::mutex> lock(g_audioMutex);
        SLAndroidSimpleBufferQueueState state{};
        const SLresult result = jit.Regs()[0] && jit.Regs()[0] == g_guestBufferQueueInterfaceAddr
            ? opensles::GetQueueState(g_hostAudio, &state) : SL_RESULT_PRECONDITIONS_VIOLATED;
        if (result == SL_RESULT_SUCCESS) {
            host.MemoryWrite32(pState, state.count);
            host.MemoryWrite32(pState + 4, state.index);
        }
        jit.Regs()[0] = result;
    });

    manager.RegisterThunk("SLBufferQueue_Enqueue", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t self = jit.Regs()[0];
        uint32_t pBuffer = jit.Regs()[1];
        uint32_t size = jit.Regs()[2];
        if (!GuestRange(host, pBuffer, size) || size == 0 || size > MAX_BUFFER_SIZE) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        std::lock_guard<std::mutex> lock(g_audioMutex);
        static uint32_t s_enq_count = 0;
        if (++s_enq_count <= 10 || s_enq_count % 200 == 0) {
            LOGI("SLBufferQueue_Enqueue #%u: size=%u, pBuffer=0x%08X, idx=%zu", s_enq_count, size, pBuffer, g_currentBufferIndex);
        }
        jit.Regs()[0] = self && self == g_guestBufferQueueInterfaceAddr
            ? opensles::Enqueue(g_hostPlayerBufferQueue, host.GetGuestMemoryBase() + pBuffer,
                                size, g_hostAudioBuffers, g_currentBufferIndex)
            : SL_RESULT_PRECONDITIONS_VIOLATED;
    });

    // 9. SLVolume Thunk
    manager.RegisterThunk("SLVolume_SetVolumeLevel", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const SLmillibel level = static_cast<SLmillibel>(jit.Regs()[1]);
        std::lock_guard<std::mutex> lock(g_audioMutex);
        jit.Regs()[0] = jit.Regs()[0] && jit.Regs()[0] == g_guestVolumeInterfaceAddr
            ? opensles::SetVolume(g_hostAudio, level) : SL_RESULT_PRECONDITIONS_VIOLATED;
    });
}
