// Host-side Ogg Vorbis decode for the guest's eager FMOD_CREATESAMPLE sounds.
//
// Startup used to spend 8-21 seconds inside the guest FMOD's Vorbis decoder:
// rglSound_manager::load_sound creates every sound effect as CREATESAMPLE, and
// the bundled decoder runs under the recompiler on the single engine thread.
// The Ogg files are plain Vorbis-in-Ogg, so the decode can move to the host
// entirely: a prefetch pool decodes each effect once through the platform
// MediaCodec while the engine is still busy with textures, the createSound
// thunk hands FMOD the finished PCM as a WAV with OPENMEMORY, and FMOD stores
// it without running its own decoder. Music is untouched - the game opens
// those files through createStream and decodes during mixing, which was never
// the slow path.
//
// Failure degrades to exactly the previous behaviour: any decode problem, a
// missing platform Vorbis codec or an unexpected call shape falls through to
// the original guest createSound with the original arguments.
//
// Lifetime follows the frozen phase-2 FMOD ABI (work/phase2_fmod_abi.md):
// OPENMEMORY data may be retained by FMOD until Sound::release, so the guest
// WAV buffer is registered against the returned FMOD::Sound* and freed only
// when the intercepted release reports success.

#include <cstddef>
#include <cstdint>

namespace warband::audio {

constexpr std::size_t kFmodCreateSoundExInfoBytes = 0x88;
constexpr std::size_t kFmodCreateSoundExInfoAlignment = 8;

enum class FmodScratchAllocResult {
    kOk,
    kOutHolderFailed,
    kExInfoFailed,
};

// Allocate both guest-side arguments as one transaction.  A partial
// allocation is rolled back here, before either address can reach FMOD.
template <typename Host>
FmodScratchAllocResult AllocateFmodScratch(Host& host, std::uint32_t& out_holder,
                                           std::uint32_t& exinfo) {
    out_holder = 0;
    exinfo = 0;
    const std::uint32_t holder =
        host.GuestAlloc(sizeof(std::uint32_t), alignof(std::uint32_t));
    if (!holder) return FmodScratchAllocResult::kOutHolderFailed;
    host.MemoryWrite32(holder, 0);

    const std::uint32_t info =
        host.GuestAlloc(kFmodCreateSoundExInfoBytes, kFmodCreateSoundExInfoAlignment);
    if (!info) {
        host.GuestFree(holder);
        return FmodScratchAllocResult::kExInfoFailed;
    }
    out_holder = holder;
    exinfo = info;
    return FmodScratchAllocResult::kOk;
}

enum class GuestSoundPublishResult {
    kCreateFailed,
    kOutputRejected,
    kPublished,
};

// FMOD returns the Sound* through its fifth argument.  The host fast path uses
// a private fifth argument for the nested createSound, so publishing that value
// back to the original guest pointer is part of completing the ABI contract.
template <typename Host>
GuestSoundPublishResult PublishGuestSound(Host& host, std::uint32_t create_result,
                                          std::uint32_t sound,
                                          std::uint32_t out_sound) {
    if (create_result != 0 || !sound) return GuestSoundPublishResult::kCreateFailed;
    if (!out_sound || (out_sound & (alignof(std::uint32_t) - 1)) != 0 ||
        static_cast<std::uint64_t>(out_sound) + sizeof(std::uint32_t) >
            host.GetGuestMemorySize()) {
        return GuestSoundPublishResult::kOutputRejected;
    }
    host.MemoryWrite32(out_sound, sound);
    return host.MemoryRead32(out_sound) == sound ? GuestSoundPublishResult::kPublished
                                                  : GuestSoundPublishResult::kOutputRejected;
}

}  // namespace warband::audio

#if !defined(WARBAND_FMOD_HOST_DECODE_TEST_ONLY)

#include "fmod_host_decode.h"

#include "apk_reader.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "game_data_vfs.h"
#include "thunk_manager.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cctype>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define LOG_TAG "WarbandAudio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using Dynarmic::A32::Jit;

namespace {

constexpr const char* kCreateSoundSymbol =
    "_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE";
constexpr const char* kReleaseSymbol = "_ZN4FMOD5Sound7releaseEv";
constexpr const char* kAssetsPrefix = "assets/";

// FMOD Ex mode bits (work/phase2_fmod_abi.md).
constexpr uint32_t kModeCreateSample = 0x100;
constexpr uint32_t kModeCreateCompressedSample = 0x200;
constexpr uint32_t kModeOpenMemory = 0x800;

constexpr size_t kWavHeaderSize = 44;

struct DecodedPcm {
    std::vector<uint8_t> data;  // interleaved PCM16 little-endian
    int channels = 0;
    int sample_rate = 0;
};

struct CacheEntry {
    enum class State { Queued, InFlight, Ready, Failed };
    State state = State::Queued;
    DecodedPcm pcm;
    size_t bytes = 0;  // decoded bytes held before consumption
};

std::mutex g_mutex;              // guards cache and queue
std::condition_variable g_progress;
std::mutex g_budget_mutex;       // separate: workers must not hold the queue
std::condition_variable g_budget;  // mutex while consumers drain decoded PCM
std::map<std::string, CacheEntry> g_cache;
std::deque<std::string> g_queue;
std::vector<std::thread> g_workers;
std::atomic<bool> g_running{false};
std::atomic<bool> g_started{false};

std::atomic<uint64_t> g_ready_bytes{0};
std::atomic<uint32_t> g_decoded{0};
std::atomic<uint64_t> g_decode_ms{0};
std::atomic<uint32_t> g_hooked{0};
std::atomic<uint32_t> g_fallbacks{0};

std::atomic<uint32_t> g_create_sound_addr{0};
std::atomic<uint32_t> g_release_addr{0};

size_t CacheCapBytes() {
    // The sound effects total about 100 MB of PCM; music is streamed and never
    // decoded here. The cap only exists so an unexpected data set cannot grow
    // the cache without bound.
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.warband.audio_pcm_cache_mb", value) > 0) {
        long parsed = strtol(value, nullptr, 10);
        if (parsed > 0) return static_cast<size_t>(parsed) * 1024 * 1024;
    }
    return 256ull * 1024 * 1024;
}

int WorkerCount() {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.warband.audio_workers", value) > 0) {
        long parsed = strtol(value, nullptr, 10);
        if (parsed >= 1 && parsed <= 8) return static_cast<int>(parsed);
    }
    return 2;
}

bool HostDecodeEnabled() {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.warband.audio_host_decode", value) > 0 &&
        (strcmp(value, "0") == 0 || strcmp(value, "false") == 0)) {
        return false;
    }
    return true;
}

bool EndsWithOgg(const char* path, size_t length) {
    if (!path || length < 4) return false;
    const char* tail = path + (length - 4);
    return std::tolower(static_cast<unsigned char>(tail[0])) == '.' &&
           std::tolower(static_cast<unsigned char>(tail[1])) == 'o' &&
           std::tolower(static_cast<unsigned char>(tail[2])) == 'g' &&
           std::tolower(static_cast<unsigned char>(tail[3])) == 'g';
}

// createSound has been seen with both root-absolute paths and plain module
// relative paths; both map onto the one queued key.
std::string NormalizeKey(const std::string& root, const char* name, size_t length) {
    std::string key(name, length);
    if (!root.empty() && key.size() > root.size() + 1 &&
        key.compare(0, root.size(), root) == 0 && key[root.size()] == '/') {
        key.erase(0, root.size() + 1);
    }
    if (key.compare(0, 7, kAssetsPrefix) == 0) key.erase(0, 7);
    return key;
}

// The platform Vorbis decoder outputs native-endian 16-bit PCM; a few devices
// report float output instead, which is converted on the way out.
//
// Demuxing runs here rather than through AMediaExtractor: the extractor lives
// in a separate media process and refused the bridge's sealed in-memory entry
// files on the field device, while a Vorbis-in-Ogg page walk is a few dozen
// lines and keeps everything in-process. The decoder is fed one OGG packet per
// input buffer with the identification and setup headers as csd-0/csd-1, which
// is exactly the shape the software Vorbis codec consumes.

struct OggPacket {
    std::vector<uint8_t> data;
    uint32_t page_frames = 0;  // trailing int32 the codec reads; 0 keeps the budget
};

struct OggDemux {
    int channels = 0;
    int sample_rate = 0;
    std::vector<uint8_t> info_header;   // type-1 packet, fed as a plain input
    std::vector<uint8_t> books_header;  // type-5 packet, fed as a plain input
    std::vector<OggPacket> packets;     // audio packets with their page budgets
};

bool ReadAllBytes(int fd, off_t size, std::vector<uint8_t>* out) {
    out->resize(static_cast<size_t>(size));
    size_t done = 0;
    while (done < out->size()) {
        ssize_t got = read(fd, out->data() + done, out->size() - done);
        if (got < 0) {
            if (errno == EINTR) continue;
            LOGE("read fd=%d at %zu/%lld failed: %s", fd, done,
                 static_cast<long long>(size), strerror(errno));
            return false;
        }
        if (got == 0) { out->resize(done); break; }
        done += static_cast<size_t>(got);
    }
    return done == out->size();
}

// Walks Vorbis-in-Ogg pages and splits packets the way the platform OGG
// extractor presents them to the codec: raw header packets first (the decoder
// recognises them by their 1/5 + "vorbis" prefix and never sees the comment),
// then one audio packet per buffer, each followed by the int32 number of PCM
// frames its page contributes. The first packet completing inside a page
// carries the page's granule delta, later ones carry zero so the decoder's
// running budget is not reset mid-page.
bool DemuxOgg(const std::vector<uint8_t>& data, OggDemux* out) {
    size_t pos = 0;
    std::vector<uint8_t> packet;
    uint64_t prev_granule = 0;
    bool audio_started = false;
    while (pos + 27 <= data.size()) {
        if (memcmp(&data[pos], "OggS", 4) != 0) return false;
        const uint8_t segment_count = data[pos + 26];
        const size_t table = pos + 27;
        if (table + segment_count > data.size()) return false;
        uint64_t granule = 0;
        memcpy(&granule, &data[pos + 6], 8);
        size_t payload = table + segment_count;

        // The trailing int32 seeds the decoder's output budget whenever it is
        // non-negative, so zero would mute the packet. The first packet of a
        // page carries the page's granule delta (trimming the block padding at
        // the page end); a zero delta or any later packet carries -1, which
        // keeps the running budget.
        uint32_t page_frames = 0xFFFFFFFFu;  // -1
        if (granule != 0xFFFFFFFFFFFFFFFFull && granule > prev_granule) {
            page_frames = static_cast<uint32_t>(granule - prev_granule);
        }
        bool page_first_packet = true;

        for (size_t s = 0; s < segment_count; ++s) {
            const size_t segment = data[table + s];
            if (payload + segment > data.size()) return false;
            packet.insert(packet.end(), &data[payload], &data[payload + segment]);
            payload += segment;
            if (segment < 255) {
                if (!audio_started) {
                    if (packet.size() >= 7 && memcmp(&packet[1], "vorbis", 6) == 0) {
                        if (packet[0] == 0x01) {
                            out->info_header = packet;
                            out->channels = packet[11];
                            memcpy(&out->sample_rate, &packet[12], 4);
                            packet.clear();
                            continue;
                        }
                        if (packet[0] == 0x05) {
                            out->books_header = packet;
                            audio_started = true;
                            packet.clear();
                            continue;
                        }
                        // The comment header is dropped: the platform decoder
                        // treats a type-3 packet as corruption if it ever
                        // reaches it while books are still unpacked.
                    }
                    packet.clear();
                    continue;
                }
                OggPacket out_packet;
                out_packet.data = std::move(packet);
                out_packet.page_frames = page_first_packet ? page_frames : 0xFFFFFFFFu;
                page_first_packet = false;
                packet.clear();
                if (out_packet.data.empty()) continue;  // no audio, no buffer
                out->packets.push_back(std::move(out_packet));
            }
        }
        prev_granule = granule;
        pos = payload;
    }
    if (!packet.empty() && audio_started) {
        OggPacket out_packet;
        out_packet.data = std::move(packet);
        out->packets.push_back(std::move(out_packet));
    }
    return audio_started && out->channels && out->sample_rate && !out->books_header.empty();
}

using VorbisCodecPtr = std::unique_ptr<AMediaCodec, decltype(&AMediaCodec_delete)>;

bool DecodeOggToPcm(int fd, off_t size, DecodedPcm* out, VorbisCodecPtr* reusable = nullptr) {
    bool ok = false;
    std::vector<uint8_t> bytes;
    AMediaCodec* codec = nullptr;
    AMediaFormat* format = nullptr;
    do {
        if (!ReadAllBytes(fd, size, &bytes) || bytes.size() < 4) break;
        OggDemux demux;
        if (!DemuxOgg(bytes, &demux)) {
            LOGE("ogg demux failed (%lld bytes)", static_cast<long long>(size));
            break;
        }
        // Only a prefetch worker passes its owner. Stop/configure/start resets
        // stream state while avoiding one client/looper allocation per effect.
        codec = reusable ? reusable->release() : nullptr;
        const bool reused_codec = codec != nullptr;
        if (!codec) codec = AMediaCodec_createDecoderByType("audio/vorbis");
        format = AMediaFormat_new();
        if (!codec || !format) break;
        // No csd: the codec picks the headers up from the first two input
        // buffers by their 1/5 + "vorbis" prefix, and merging csd into one
        // codec-config buffer breaks that on some devices.
        AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "audio/vorbis");
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, demux.sample_rate);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, demux.channels);
        const auto ConfigureAndStart = [&] {
            if (AMediaCodec_configure(codec, format, nullptr, nullptr, 0) != AMEDIA_OK) {
                LOGE("codec configure failed");
                return false;
            }
            if (AMediaCodec_start(codec) != AMEDIA_OK) {
                LOGE("codec start failed");
                return false;
            }
            return true;
        };
        if (!ConfigureAndStart()) {
            if (!reused_codec) break;
            AMediaCodec_delete(codec);
            codec = AMediaCodec_createDecoderByType("audio/vorbis");
            if (!codec || !ConfigureAndStart()) break;
        }

        bool input_eos = false;
        bool output_eos = false;
        bool float_output = false;
        size_t next_packet = 0;
        // The first two plain inputs are the raw headers, then one OggPacket
        // per buffer with the trailing frame-count int32 the codec consumes.
        const std::vector<uint8_t>* headers[2] = {&demux.info_header, &demux.books_header};
        size_t next_header = 0;
        out->data.reserve(static_cast<size_t>(size) * 8 + 65536);
        // Every wait below is a bounded poll, so the whole loop is bounded by
        // this deadline. A file that overruns it fails alone and that one sound
        // falls back to the guest decoder; nothing is allowed to hang startup.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!output_eos) {
            if (std::chrono::steady_clock::now() > deadline) {
                LOGE("decode deadline exceeded (%lld bytes)", static_cast<long long>(size));
                break;
            }
            // Header packets produce no PCM. Feeding just one packet and then
            // waiting for output charged a 4-ms timeout for each such packet
            // and prevented the decoder from using its available input queue.
            // Keep packet/EOS order, but fill ready input buffers before waiting.
            bool input_failed = false;
            while (!input_eos) {
                if (std::chrono::steady_clock::now() > deadline) {
                    LOGE("decode deadline exceeded (%lld bytes)", static_cast<long long>(size));
                    input_failed = true;
                    break;
                }
                ssize_t index = AMediaCodec_dequeueInputBuffer(codec, 0);
                if (index == -1 /* TRY_AGAIN */) {
                    break; // No ready input slot; drain output or wait for it.
                } else if (index < 0) {
                    LOGE("dequeueInputBuffer error %zd", index);
                    input_failed = true;
                    break;
                } else {
                    size_t capacity = 0;
                    uint8_t* buffer =
                        AMediaCodec_getInputBuffer(codec, static_cast<size_t>(index), &capacity);
                    const uint8_t* queued_data = nullptr;
                    size_t queued_size = 0;
                    uint32_t flags = 0;
                    if (next_header < 2) {
                        const std::vector<uint8_t>& header = *headers[next_header];
                        if (!buffer || header.size() > capacity) {
                            LOGE("header %zu bytes exceeds input buffer %zu", header.size(),
                                 capacity);
                            input_failed = true;
                            break;
                        }
                        memcpy(buffer, header.data(), header.size());
                        queued_data = header.data();
                        queued_size = header.size();
                        ++next_header;
                    } else if (next_packet < demux.packets.size()) {
                        const OggPacket& candidate = demux.packets[next_packet];
                        if (!buffer || candidate.data.size() + 4 > capacity) {
                            // Delivering a partial Vorbis packet is impossible,
                            // so this file cannot be decoded here.
                            LOGE("packet %zu bytes exceeds input buffer %zu",
                                 candidate.data.size() + 4, capacity);
                            input_failed = true;
                            break;
                        }
                        memcpy(buffer, candidate.data.data(), candidate.data.size());
                        memcpy(buffer + candidate.data.size(), &candidate.page_frames,
                               sizeof(candidate.page_frames));
                        queued_data = buffer;
                        queued_size = candidate.data.size() + 4;
                        ++next_packet;
                    } else {
                        flags = AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
                        input_eos = true;
                    }
                    media_status_t status = AMediaCodec_queueInputBuffer(
                        codec, static_cast<size_t>(index), 0, queued_size, 0, flags);
                    if (status != AMEDIA_OK) {
                        LOGE("queueInputBuffer error %d", static_cast<int>(status));
                        input_failed = true;
                        break;
                    }
                }
            }
            if (input_failed) break;
            AMediaCodecBufferInfo info{};
            ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, 4000);
            if (index < 0 && index != AMEDIACODEC_INFO_TRY_AGAIN_LATER &&
                index != AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED &&
                index != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                LOGE("dequeueOutputBuffer error %zd", index);
                break;
            }
            if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat* output = AMediaCodec_getOutputFormat(codec);
                AMediaFormat_getInt32(output, AMEDIAFORMAT_KEY_SAMPLE_RATE, &out->sample_rate);
                AMediaFormat_getInt32(output, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &out->channels);
                int32_t encoding = 0;
                if (AMediaFormat_getInt32(output, "pcm-encoding", &encoding) &&
                    encoding == 4 /* ENCODING_PCM_FLOAT */) {
                    float_output = true;
                }
                AMediaFormat_delete(output);
            } else if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED ||
                       index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                continue;
            } else if (index >= 0) {
                if (info.size > 0) {
                    size_t capacity = 0;
                    const uint8_t* buffer = AMediaCodec_getOutputBuffer(
                        codec, static_cast<size_t>(index), &capacity);
                    if (!buffer || static_cast<size_t>(info.offset) + info.size > capacity) {
                        LOGE("bad output buffer");
                        break;
                    }
                    const uint8_t* begin = buffer + info.offset;
                    if (float_output) {
                        const size_t samples = static_cast<size_t>(info.size) / 4;
                        out->data.resize(out->data.size() + samples * 2);
                        uint8_t* write = out->data.data() + out->data.size() - samples * 2;
                        for (size_t s = 0; s < samples; ++s) {
                            float value;
                            memcpy(&value, begin + s * 4, 4);
                            int clamped = static_cast<int>(value * 32768.0f);
                            clamped = std::max(-32768, std::min(32767, clamped));
                            int16_t narrowed = static_cast<int16_t>(clamped);
                            memcpy(write + s * 2, &narrowed, 2);
                        }
                    } else {
                        out->data.insert(out->data.end(), begin, begin + info.size);
                    }
                }
                AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(index), false);
                if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) output_eos = true;
            }
        }
        if (output_eos && out->channels && out->sample_rate && !out->data.empty()) {
            ok = true;
        } else {
            LOGE("decode produced no pcm (eos=%d ch=%d rate=%d)", output_eos,
                 out->channels, out->sample_rate);
        }
    } while (false);
    if (format) AMediaFormat_delete(format);
    if (codec) {
        const media_status_t stopped = AMediaCodec_stop(codec);
        if (reusable && ok && stopped == AMEDIA_OK) reusable->reset(codec);
        else AMediaCodec_delete(codec);
    }
    if (!ok && reusable) reusable->reset();
    return ok;
}

int OpenGuestAudioFile(const std::string& path, off_t* out_size) {
    // Queued keys are module-relative ("Sounds/x.ogg") while the engine hands
    // createSound root-absolute names; try every shape both sides can produce.
    const std::string root = GameDataMainRoot();
    if (!root.empty()) {
        int fd = GameDataOpenEntry((root + "/" + path).c_str(), out_size);
        if (fd >= 0) return fd;
    }
    int fd = GameDataOpenEntry(path.c_str(), out_size);
    if (fd >= 0) return fd;
    fd = ApkOpenEntry(("assets/" + path).c_str(), out_size);
    if (fd >= 0) return fd;
    return ApkOpenEntry(path.c_str(), out_size);
}

void Publish(const std::string& path, DecodedPcm&& pcm, bool ok) {
    size_t bytes = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_cache.find(path);
        if (it == g_cache.end()) return;  // consumed or dropped while decoding
        it->second.state = ok ? CacheEntry::State::Ready : CacheEntry::State::Failed;
        if (ok) {
            it->second.pcm = std::move(pcm);
            bytes = it->second.pcm.data.size();
            it->second.bytes = bytes;
        }
    }
    if (ok) {
        g_ready_bytes.fetch_add(bytes, std::memory_order_relaxed);
        g_decoded.fetch_add(1, std::memory_order_relaxed);
    }
    g_progress.notify_all();
}

bool DecodeOwned(const std::string& path, DecodedPcm* out) {
    const auto started = std::chrono::steady_clock::now();
    DecodedPcm pcm;
    off_t size = 0;
    bool ok = false;
    int fd = OpenGuestAudioFile(path, &size);
    if (fd >= 0) {
        ok = DecodeOggToPcm(fd, size, &pcm);
        close(fd);
    } else {
        LOGE("open failed: %s", path.c_str());
    }
    if (ok) {
        g_decode_ms.fetch_add(
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count()),
            std::memory_order_relaxed);
    }
    Publish(path, std::move(pcm), ok);
    if (!ok) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_cache.find(path);
    if (it == g_cache.end() || it->second.state != CacheEntry::State::Ready) return false;
    *out = std::move(it->second.pcm);
    g_ready_bytes.fetch_sub(it->second.bytes, std::memory_order_relaxed);
    g_cache.erase(it);
    g_progress.notify_all();
    g_budget.notify_all();
    return true;
}

// Takes a ready entry out of the cache, or returns false when the caller has to
// fall back to the guest decoder. A queued entry nobody owns yet is decoded on
// the calling thread, so a consumer can never wait behind the prefetch budget.
bool AcquireDecoded(const std::string& key, DecodedPcm* out) {
    std::unique_lock<std::mutex> lock(g_mutex);
    auto it = g_cache.find(key);
    if (it == g_cache.end()) return false;
    switch (it->second.state) {
        case CacheEntry::State::Ready:
            *out = std::move(it->second.pcm);
            g_ready_bytes.fetch_sub(it->second.bytes, std::memory_order_relaxed);
            g_cache.erase(it);
            g_progress.notify_all();
            g_budget.notify_all();
            return true;
        case CacheEntry::State::InFlight: {
            // The worker never blocks mid-decode, so this wait is bounded by the
            // file itself. Stop when the pool shuts down without a result.
            g_progress.wait(lock, [&] {
                auto probe = g_cache.find(key);
                return !g_running.load() || probe == g_cache.end() ||
                       probe->second.state == CacheEntry::State::Ready ||
                       probe->second.state == CacheEntry::State::Failed;
            });
            it = g_cache.find(key);
            if (it != g_cache.end() && it->second.state == CacheEntry::State::Ready) {
                *out = std::move(it->second.pcm);
                g_ready_bytes.fetch_sub(it->second.bytes, std::memory_order_relaxed);
                g_cache.erase(it);
                g_progress.notify_all();
                g_budget.notify_all();
                return true;
            }
            return false;
        }
        case CacheEntry::State::Queued:
            it->second.state = CacheEntry::State::InFlight;
            lock.unlock();
            return DecodeOwned(key, out);
        case CacheEntry::State::Failed:
            return false;
    }
    return false;
}

void WorkerLoop() {
    const size_t cap = CacheCapBytes();
    VorbisCodecPtr decoder{nullptr, AMediaCodec_delete};
    for (;;) {
        std::string path;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            if (g_queue.empty()) {
                // Release the idle codec without blocking a PCM consumer on
                // the queue mutex. The worker owner also releases it on exit.
                lock.unlock();
                decoder.reset();
                lock.lock();
            }
            g_progress.wait(lock, [] { return !g_running.load() || !g_queue.empty(); });
            if (!g_running.load()) return;
            path = std::move(g_queue.front());
            g_queue.pop_front();
            auto it = g_cache.find(path);
            if (it == g_cache.end() || it->second.state != CacheEntry::State::Queued) continue;
            it->second.state = CacheEntry::State::InFlight;
        }
        // Stay under the cache budget, but never stall the queue: if consumers
        // are not draining fast enough, decode anyway.
        {
            std::unique_lock<std::mutex> budget(g_budget_mutex);
            g_budget.wait_for(budget, std::chrono::seconds(2),
                              [&] { return g_ready_bytes.load(std::memory_order_relaxed) < cap; });
        }
        DecodedPcm pcm;
        off_t size = 0;
        bool ok = false;
        int fd = OpenGuestAudioFile(path, &size);
        if (fd >= 0) {
            const auto started = std::chrono::steady_clock::now();
            ok = DecodeOggToPcm(fd, size, &pcm, &decoder);
            close(fd);
            if (ok) {
                g_decode_ms.fetch_add(
                    static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started).count()),
                    std::memory_order_relaxed);
            }
        } else {
            LOGE("prefetch open failed: %s", path.c_str());
        }
        Publish(path, std::move(pcm), ok);
    }
}

// --- FMOD::Sound* -> guest WAV buffer lifetime ------------------------------

std::mutex g_retained_mutex;
std::map<uint32_t, uint32_t> g_retained_buffers;  // guest Sound* -> guest WAV buffer

uint32_t OriginalSymbol(DynarmicHost& host, const char* name) {
    ElfLoader* fmodex = host.GetLibrary("libfmodex.so");
    return fmodex ? fmodex->GetSymbolAddress(name) : 0;
}

void AppendWavHeader(uint8_t* out, size_t data_bytes, int channels, int rate) {
    const uint32_t block_align = static_cast<uint32_t>(channels * 2);
    const uint32_t byte_rate = static_cast<uint32_t>(rate) * block_align;
    auto write32 = [out](size_t at, uint32_t v) { memcpy(out + at, &v, 4); };
    auto write16 = [out](size_t at, uint16_t v) { memcpy(out + at, &v, 2); };
    memcpy(out, "RIFF", 4);
    write32(4, static_cast<uint32_t>(36 + data_bytes));
    memcpy(out + 8, "WAVE", 4);
    memcpy(out + 12, "fmt ", 4);
    write32(16, 16);  // PCM format chunk size
    write16(20, 1);   // WAVE_FORMAT_PCM
    write16(22, static_cast<uint16_t>(channels));
    write32(24, static_cast<uint32_t>(rate));  // sample rate
    write32(28, byte_rate);
    write16(32, static_cast<uint16_t>(block_align));
    write16(34, 16);  // bits per sample
    memcpy(out + 36, "data", 4);
    write32(40, static_cast<uint32_t>(data_bytes));
}

std::atomic<uint32_t> g_symbol_resolved{0};

}  // namespace

void WarbandAudioPrefetchStart() {
    if (!HostDecodeEnabled() || g_started.exchange(true)) return;
    const std::string root = GameDataMainRoot();
    std::vector<std::string> effects;
    ApkForEachEntry(
        [](const char* name, uint32_t, void* ud) -> bool {
            // Music is opened with createStream and decoded during mixing, so
            // pre-decoding it would only hold PCM the game never asks for.
            if (strncmp(name, "assets/music/", 13) != 0 && strncmp(name, kAssetsPrefix, 7) != 0) {
                return true;
            }
            const size_t length = strlen(name);
            if (strncmp(name, "assets/music/", 13) == 0 || !EndsWithOgg(name, length)) {
                return true;
            }
            static_cast<std::vector<std::string>*>(ud)->emplace_back(name + 7);
            return true;
        },
        &effects);
    if (effects.empty()) {
        LOGI("prefetch: no ogg sound effects found");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (std::string& effect : effects) {
            if (g_cache.emplace(effect, CacheEntry{}).second) g_queue.push_back(effect);
        }
    }
    g_running.store(true);
    const int workers = WorkerCount();
    for (int i = 0; i < workers; ++i) g_workers.emplace_back(WorkerLoop);
    LOGI("prefetch started: %zu ogg effects, %d workers", effects.size(), workers);
}

void WarbandAudioPrefetchStop() {
    if (!g_started.exchange(false)) return;
    g_running.store(false);
    g_progress.notify_all();
    g_budget.notify_all();
    for (auto& worker : g_workers) {
        if (worker.joinable()) worker.join();
    }
    g_workers.clear();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cache.clear();
    g_queue.clear();
    g_ready_bytes.store(0);
}

void ThunkManager::RegisterFmodHostDecodeThunks() {
    RegisterThunk(
        kCreateSoundSymbol,
        [](Jit& jit, DynarmicHost& host) {
            const uint32_t system = jit.Regs()[0];
            const uint32_t name_ptr = jit.Regs()[1];
            const uint32_t mode = jit.Regs()[2];
            const uint32_t exinfo = jit.Regs()[3];
            const uint32_t out_sound = host.MemoryRead32(jit.Regs()[13]);
            const char* name =
                reinterpret_cast<const char*>(host.GetGuestMemoryBase() + name_ptr);
            const size_t name_length = name ? strnlen(name, 2048) : 0;

            if (g_symbol_resolved.load(std::memory_order_acquire) == 0) {
                uint32_t create = OriginalSymbol(host, kCreateSoundSymbol);
                uint32_t release = OriginalSymbol(host, kReleaseSymbol);
                uint32_t expected = 0;
                if (create && release &&
                    g_symbol_resolved.compare_exchange_strong(expected, 1)) {
                    g_create_sound_addr.store(create, std::memory_order_release);
                    g_release_addr.store(release, std::memory_order_release);
                } else if (!create || !release) {
                    LOGE("fmodex symbols not found (create=%u release=%u)", create, release);
                    if (out_sound) host.MemoryWrite32(out_sound, 0);
                    jit.Regs()[0] = 0xFFFFFFFF;  // FMOD_ERR_UNINITIALIZED
                    return;
                }
            }
            uint32_t create_addr = g_create_sound_addr.load(std::memory_order_acquire);
            while (!create_addr) {
                std::this_thread::yield();
                create_addr = g_create_sound_addr.load(std::memory_order_acquire);
            }
            if (!out_sound || !name || !name_length || !(mode & kModeCreateSample) ||
                exinfo != 0 || !EndsWithOgg(name, name_length)) {
                jit.Regs()[0] =
                    host.CallGuestNested(create_addr, {system, name_ptr, mode, exinfo, out_sound});
                return;
            }

            DecodedPcm pcm;
            if (AcquireDecoded(NormalizeKey(GameDataMainRoot(), name, name_length), &pcm)) {
                const size_t wav_bytes = kWavHeaderSize + pcm.data.size();
                const uint32_t buffer = host.GuestAlloc(wav_bytes, 16);
                if (buffer) {
                    uint8_t* base = host.GetGuestMemoryBase() + buffer;
                    AppendWavHeader(base, pcm.data.size(), pcm.channels, pcm.sample_rate);
                    memcpy(base + kWavHeaderSize, pcm.data.data(), pcm.data.size());
                    uint32_t out_holder = 0;
                    uint32_t wav_exinfo = 0;
                    const auto scratch = warband::audio::AllocateFmodScratch(
                        host, out_holder, wav_exinfo);
                    if (scratch != warband::audio::FmodScratchAllocResult::kOk) {
                        if (scratch ==
                            warband::audio::FmodScratchAllocResult::kOutHolderFailed) {
                            LOGE("guest alloc 4 bytes for FMOD out holder failed");
                        } else {
                            LOGE("guest alloc 136 bytes for FMOD exinfo failed");
                        }
                        host.GuestFree(buffer);
                    } else {
                        // OPENMEMORY has no path to stat, so the guest FMOD takes
                        // the data length from exinfo->length (frozen 0x88-byte
                        // FMOD 4.44.39 structure, phase2 ABI document).
                        uint8_t* ex = host.GetGuestMemoryBase() + wav_exinfo;
                        memset(ex, 0, warband::audio::kFmodCreateSoundExInfoBytes);
                        {
                            const uint32_t cbsize = static_cast<uint32_t>(
                                warband::audio::kFmodCreateSoundExInfoBytes);
                            memcpy(ex, &cbsize, 4);
                            const uint32_t length = static_cast<uint32_t>(wav_bytes);
                            memcpy(ex + 4, &length, 4);
                        }
                        const uint32_t wav_mode =
                            (mode & ~(kModeCreateSample | kModeCreateCompressedSample)) |
                            kModeOpenMemory;
                        const uint32_t result = host.CallGuestNested(
                            create_addr, {system, buffer, wav_mode, wav_exinfo, out_holder});
                        const uint32_t sound = host.MemoryRead32(out_holder);
                        host.GuestFree(wav_exinfo);
                        host.GuestFree(out_holder);

                        const auto publish = warband::audio::PublishGuestSound(
                            host, result, sound, out_sound);
                        if (publish == warband::audio::GuestSoundPublishResult::kPublished) {
                            {
                                std::lock_guard<std::mutex> lock(g_retained_mutex);
                                // OPENMEMORY data may live until Sound::release.
                                g_retained_buffers[sound] = buffer;
                            }
                            const uint32_t total =
                                g_hooked.fetch_add(1, std::memory_order_relaxed) + 1;
                            if (total % 64 == 0) {
                                LOGI("host decode: sounds=%u fallbacks=%u decoded=%u decode_ms=%llu",
                                     total, g_fallbacks.load(), g_decoded.load(),
                                     static_cast<unsigned long long>(g_decode_ms.load()));
                            }
                            jit.Regs()[0] = 0;
                            return;
                        }
                        if (publish ==
                            warband::audio::GuestSoundPublishResult::kOutputRejected) {
                            // Do not report FMOD_OK when its required output value was not
                            // delivered.  Release the otherwise successful nested Sound;
                            // retain OPENMEMORY on release failure rather than risk a UAF.
                            const uint32_t release_addr =
                                g_release_addr.load(std::memory_order_acquire);
                            const uint32_t release_result =
                                release_addr ? host.CallGuestNested(release_addr, {sound})
                                             : 0xFFFFFFFFu;
                            if (release_result == 0) {
                                host.GuestFree(buffer);
                            } else {
                                std::lock_guard<std::mutex> lock(g_retained_mutex);
                                g_retained_buffers[sound] = buffer;
                            }
                            LOGE("failed to publish guest sound=%u to out=0x%08X; release=%u",
                                 sound, out_sound, release_result);
                            jit.Regs()[0] = 0xFFFFFFFFu;
                            return;
                        }
                        LOGE("guest createSound(wav) failed result=%u sound=%u", result, sound);
                        host.GuestFree(buffer);
                    }
                } else {
                    LOGE("guest alloc %zu bytes for wav failed", wav_bytes);
                }
            } else {
                const uint32_t total = g_fallbacks.fetch_add(1, std::memory_order_relaxed) + 1;
                if (total == 1) LOGE("host decode unavailable for %.180s; using guest decoder", name);
            }

            jit.Regs()[0] =
                host.CallGuestNested(create_addr, {system, name_ptr, mode, exinfo, out_sound});
        });

    RegisterThunk(kReleaseSymbol, [](Jit& jit, DynarmicHost& host) {
        const uint32_t sound = jit.Regs()[0];
        const uint32_t release_addr = g_release_addr.load(std::memory_order_acquire);
        const uint32_t result = release_addr ? host.CallGuestNested(release_addr, {sound}) : 0;
        if (result == 0 && sound) {
            uint32_t buffer = 0;
            {
                std::lock_guard<std::mutex> lock(g_retained_mutex);
                auto it = g_retained_buffers.find(sound);
                if (it != g_retained_buffers.end()) {
                    buffer = it->second;
                    g_retained_buffers.erase(it);
                }
            }
            if (buffer) host.GuestFree(buffer);
        }
        jit.Regs()[0] = result;
    });
}

#endif  // !WARBAND_FMOD_HOST_DECODE_TEST_ONLY
